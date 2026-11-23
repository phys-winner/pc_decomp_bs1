"""x87 stack to C locals for BioShock's lifted bodies with a known stack depth.

Ported from the Heroes V port (homm5/tools/x87_locals.py and the body/CFG part
of homm5/tools/call_protocol.py) to this project's generated layout.

The lifted code keeps the x87 stack in a global window (_st[i], fp_push,
fp_pop, fp_xch: android/runtime/recomp_types.h, RECOMP_X87_WINDOW). Every push
and pop moves the window pointer and writes value and integer shadow, and every
_st[i] is a load through it, so x87 arithmetic is a chain of memory operations
(about 15 ARM64 instructions per x86 one in BioShock's hot math).

MSVC code keeps the x87 stack empty at calls, except for the CRT's x87
argument helpers (_CIsqrt, _CIpow, ...), and returns a float in st(0). This
tool computes, per body, the stack depth at every instruction relative to its
entry and the body's x87 interface: c values consumed from its caller's stack
and p values left for it. A body whose depth is consistent, and whose x87 code
only uses the generated forms below, gets one C local per stack slot:

    _st[i]            -> the slot at depth-1-i
    g_st_i64[i]       -> that slot's integer shadow
    fp_push(v)        -> slot(depth) = v, shadow 0
    fp_push_i64(v)    -> slot(depth) = v with the exact shadow
    fp_pop()          -> slot(depth-1)
    fp_xch(i)         -> swap two slots and their shadows

The global window is touched only at the body's x87 boundaries: consumed
values are popped at entry, results pushed before each return, and around a
call the callee's x87 arguments are pushed and its results popped back. The
arithmetic is the same C on the same doubles, so results are identical to the
window model; only the storage changes. Anything else naming the x87 state
(fsave/frstor, fprem, the window itself, bare _st) keeps the body on the
window, as do exception-handling bodies and x87 interfaces with tail exits.

    python tools/x87_locals.py      # writes build/x87/x87.json
"""
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import pickle
import re
import sys
import time

import capstone
from capstone import x86_const as X

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / 'generated'
OUT_DIR = ROOT / 'build/x87'
X87_JSON = OUT_DIR / 'x87.json'


def manifest():
    """The generated modules in modules.inc order, with their source chunks."""
    specs = {row['file']: row for row in json.loads((ROOT / 'config/modules.json').read_text())['modules']}
    included = re.findall(r'^\s*\{\s*"([^"]+)"', (GENERATED / 'modules.inc').read_text(), re.M)
    modules = []
    for name in included:
        spec = dict(specs[name])
        report = json.loads((GENERATED / spec['name'] / 'report.json').read_text())
        spec['sources'] = [GENERATED / spec['name'] / s for s in report['sources']]
        modules.append(spec)
    return modules


FUNC_RE = re.compile(rb'^void (sub_([0-9A-F]{8}))\(void\) \{\r?$', re.M)
END_RE = re.compile(rb'\n\}\r?\n')
# The disassembly text may contain '*' (scaled index: [edx + ecx*4]).
INSN_RE = re.compile(rb'/\* 0x([0-9A-F]{8}): ([^\r\n]*?) \*/')
CALL_LINE_RE = re.compile(rb'RECOMP_CALL\((sub_[0-9A-F]{8}|recomp_\w+)\);')
LABEL_RE = re.compile(rb'^L_([0-9A-F]{8}):', re.M)
# An int3/ud2 the lifter turned into a trap carries no instruction comment.
TRAP_RE = re.compile(rb'^\s*void_lift_trap\(0x([0-9A-F]{8})u')


# --- decoding ---------------------------------------------------------------
class Decoded:
    __slots__ = ('address', 'size', 'mnemonic', 'op_str', 'kind', 'target', 'imm', 'table', 'iat')


def classify(insn):
    d = Decoded()
    d.address, d.size, d.mnemonic, d.op_str = insn.address, insn.size, insn.mnemonic, insn.op_str
    m = insn.mnemonic.split()[-1]
    d.kind, d.target, d.imm, d.table, d.iat = 'op', None, 0, 0, 0
    ops = insn.operands
    if m == 'call':
        d.kind = 'call' if ops and ops[0].type == X.X86_OP_IMM else 'icall'
        d.target = ops[0].imm & 0xFFFFFFFF if d.kind == 'call' else None
    elif m in ('ret', 'retn'):
        d.kind = 'ret'
        d.imm = ops[0].imm if ops and ops[0].type == X.X86_OP_IMM else 0
    elif m == 'jmp':
        d.kind = 'jmp' if ops and ops[0].type == X.X86_OP_IMM else 'ijmp'
        d.target = ops[0].imm & 0xFFFFFFFF if d.kind == 'jmp' else None
    elif insn.group(X.X86_GRP_JUMP):
        d.kind = 'jcc'
        d.target = ops[0].imm & 0xFFFFFFFF if ops and ops[0].type == X.X86_OP_IMM else None
    elif m in ('int3', 'hlt', 'ud2'):
        d.kind = 'stop'
    if m == 'push' and ops and ops[0].type == X.X86_OP_IMM:
        d.imm = ops[0].imm & 0xFFFFFFFF
    # jmp [index*4 + table] (no base): a switch over a table of local targets;
    # 1 for jmp reg (targets unknown, may be local); 0 for other indirect jumps.
    if d.kind == 'ijmp' and ops and ops[0].type == X.X86_OP_MEM and ops[0].mem.index != 0 \
            and ops[0].mem.scale == 4 and ops[0].mem.base == 0:
        d.table = ops[0].mem.disp & 0xFFFFFFFF
    elif d.kind == 'ijmp' and ops and ops[0].type == X.X86_OP_REG:
        d.table = 1
    # call/jmp dword ptr [abs]: an import address table slot.
    if d.kind in ('icall', 'ijmp') and ops and ops[0].type == X.X86_OP_MEM and ops[0].mem.base == 0 \
            and ops[0].mem.index == 0 and ops[0].size == 4:
        d.iat = ops[0].mem.disp & 0xFFFFFFFF
    return d


class Image:
    def __init__(self, path):
        import pefile
        pe = pefile.PE(str(path), fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.data = pe.get_memory_mapped_image()
        pe.close()

    def code(self, va, size=16):
        offset = va - self.base
        return self.data[offset:offset + size]


def load_decoded(module, addresses, cache_dir):
    image_path = GENERATED / 'input' / module['file']
    digest = hashlib.sha256(image_path.read_bytes()).hexdigest()[:16]
    cache = cache_dir / f"decode-{module['name']}-{digest}.pickle"
    decoded = {}
    if cache.exists():
        for address, row in pickle.loads(cache.read_bytes()).items():
            if row is None:
                decoded[address] = None
                continue
            d = Decoded()
            for slot, value in zip(Decoded.__slots__, row):
                setattr(d, slot, value)
            decoded[address] = d
    missing = sorted(a for a in addresses if a not in decoded)
    if missing:
        image = Image(image_path)
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True
        for address in missing:
            got = next(md.disasm(image.code(address), address, 1), None)
            decoded[address] = classify(got) if got else None
        rows = {address: None if d is None else tuple(getattr(d, slot) for slot in Decoded.__slots__)
                for address, d in decoded.items()}
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_bytes(pickle.dumps(rows, protocol=pickle.HIGHEST_PROTOCOL))
    return decoded


# --- bodies -----------------------------------------------------------------
class Function:
    __slots__ = ('name', 'va', 'module', 'source', 'addresses', 'tags', 'callees', 'eh', 'labels', 'switch_after')

    def __init__(self, name, va, module, source):
        self.name, self.va, self.module, self.source = name, va, module, source
        self.addresses, self.tags, self.callees, self.eh, self.labels = [], [], {}, False, set()
        # The generator rewrites a computed jump into `{ _itail_tgt = ...;
        # goto _ljump; }` and drops its comment: remember where it sits, the
        # jump is decoded after it.
        self.switch_after = set()


def lifted_tag(line):
    """How the lifter emitted the instruction on this line."""
    if b'void_lift_trap(' in line:
        return 'trap'
    if b'RECOMP_CALL(' in line:
        return 'call'
    if b'RECOMP_ICALL(' in line:
        return 'icall'
    if b'RECOMP_ITAIL(' in line or b'RECOMP_RET_JUMP(' in line:
        return 'tail'
    if b'goto _ljump' in line or b'goto *_jt' in line:
        return 'switch'
    if b'return;' in line:
        return 'ret'
    return 'op'


def parse_functions(modules):
    functions = {}
    for module in modules:
        for chunk in module['sources']:
            text = chunk.read_bytes()
            for match in FUNC_RE.finditer(text):
                end = END_RE.search(text, match.end())
                body = text[match.end():end.end()]
                f = Function(match[1].decode(), int(match[2], 16), module, chunk.name)
                f.eh = b'RECOMP_EH_ENTER()' in body
                f.labels = {int(x, 16) for x in LABEL_RE.findall(body)}
                seen = set()
                last = label = None
                for line in body.split(b'\n'):
                    insn = INSN_RE.search(line)
                    trap = None if insn else TRAP_RE.match(line)
                    if trap:
                        address = int(trap[1], 16)
                        if address not in seen:
                            seen.add(address)
                            f.addresses.append(address)
                            f.tags.append('trap')
                            last = address
                        label = None
                        continue
                    if not insn:
                        at = LABEL_RE.match(line)
                        if at:
                            label = int(at[1], 16)
                        elif b'_itail_tgt = ' in line and b'goto _ljump' in line and (label is not None or last is not None):
                            f.switch_after.add((len(f.addresses), label, last))
                        continue
                    label = None
                    address = int(insn[1], 16)
                    if address in seen:
                        continue
                    seen.add(address)
                    tag = lifted_tag(line)
                    if tag == 'call' and b'; jmp 0x' in insn[2]:
                        tag = 'pushjmp'
                    f.addresses.append(address)
                    f.tags.append(tag)
                    last = address
                    if tag in ('call', 'pushjmp'):
                        callee = CALL_LINE_RE.search(line)
                        if callee:
                            f.callees[address] = callee[1].decode()
                if f.name in functions:
                    raise ValueError(f'duplicate generated body {f.name}')
                functions[f.name] = f
    return functions


class Body:
    """Compact CFG of one lifted body: instruction i has successors succ[i]
    (indices) and an exit kind exit[i]: None, ('ret',), ('tail', name|None),
    ('stop',). call[i] is ('direct', name) / ('import', name) / ('native',) / None."""
    __slots__ = ('f', 'insns', 'succ', 'exit', 'call')


IMAGES = {}


def image_u32(module, va):
    image = IMAGES.get(module['name'])
    if image is None:
        image = IMAGES[module['name']] = Image(GENERATED / 'input' / module['file'])
    offset = va - image.base
    if offset < 0 or offset + 4 > len(image.data):
        return None
    return int.from_bytes(image.data[offset:offset + 4], 'little')


NORETURN_IMPORTS = {'ExitProcess', 'TerminateProcess', 'ExitThread', 'FatalAppExitA', 'FatalAppExitW',
                    'RaiseException', 'abort', 'exit', '_exit', '_amsg_exit', '_CxxThrowException', 'longjmp',
                    '_cexit', '__report_gsfailure', 'FatalExit'}
# The MSVC CRT's x87 argument helpers (st0, or st1/st0, in; the result in
# st0) and the cdecl math functions (double arguments on the stack, the
# result in st0): (values consumed from the caller's x87 stack, values left).
X87_IMPORTS = {name: (1, 1) for name in ('_CIsqrt', '_CIsin', '_CIcos', '_CItan', '_CIasin', '_CIacos', '_CIatan',
                                         '_CIexp', '_CIlog', '_CIlog10', '_CIsinh', '_CIcosh', '_CItanh')}
X87_IMPORTS.update({name: (2, 1) for name in ('_CIpow', '_CIatan2', '_CIfmod')})
X87_IMPORTS.update({name: (0, 1) for name in ('floor', 'ceil', 'sqrt', 'sin', 'cos', 'tan', 'asin', 'acos', 'atan',
                                              'atan2', 'pow', 'exp', 'log', 'log10', 'fmod', 'fabs', 'ldexp',
                                              'frexp', 'modf', 'atof', 'strtod', 'difftime')})
IAT_NAMES = {}


def iat_names(modules):
    """IAT slot VA -> imported function name, for every module."""
    if not IAT_NAMES:
        import pefile
        for module in modules:
            pe = pefile.PE(str(GENERATED / 'input' / module['file']), fast_load=True)
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
            for entry in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []) or []:
                for imp in entry.imports:
                    if imp.name:
                        IAT_NAMES[imp.address] = imp.name.decode(errors='replace')
            pe.close()
        IAT_NAMES[0] = ''
    return IAT_NAMES


def build_body(f, decoded, names, iat):
    b = Body()
    b.f = f
    n = len(f.addresses)
    index = {a: i for i, a in enumerate(f.addresses)}
    insns = [decoded.get(a) for a in f.addresses]
    succ, exits, calls = [[] for _ in range(n)], [None] * n, [None] * n
    leaders = sorted(index[a] for a in f.labels if a in index)

    def flow(i, address):
        if address in index:
            succ[i].append(index[address])
        elif exits[i] is None:
            exits[i] = ('tail', names.get(address))
        else:
            exits[i] = ('tail', None)

    for i, d in enumerate(insns):
        tag = f.tags[i]
        if d is None or tag == 'trap':
            exits[i] = ('stop',)
            continue
        nxt = d.address + d.size
        if tag == 'pushjmp':
            # push label; jmp func: a call returning to the pushed label.
            calls[i] = ('direct', f.callees.get(d.address))
            flow(i, d.imm)
            continue
        if d.kind == 'ret':
            exits[i] = ('ret',)
        elif d.kind == 'stop':
            exits[i] = ('stop',)
        elif d.kind == 'jmp':
            flow(i, d.target)
        elif d.kind == 'ijmp':
            if d.table == 1:
                succ[i].extend(leaders)
            elif d.table:
                # Entries while they stay inside this body: the index is
                # range-checked before the jump.
                for k in range(1024):
                    target = image_u32(f.module, d.table + 4 * k)
                    if target is None or target not in index:
                        break
                    succ[i].append(index[target])
                if succ[i]:
                    continue
            exits[i] = ('tail', ('import', iat.get(d.iat, '')) if d.iat else None)
        elif d.kind == 'jcc':
            if d.target is not None:
                flow(i, d.target)
            flow(i, nxt)
        elif d.kind in ('call', 'icall'):
            if tag == 'op' and d.kind == 'call' and d.target == (nxt & 0xFFFFFFFF):
                flow(i, nxt & 0xFFFFFFFF)  # Inline CALL-next is a push, with no callee boundary.
                continue
            if tag == 'tail':
                exits[i] = ('tail', None)
                continue
            if tag == 'call':
                calls[i] = ('direct', f.callees.get(d.address))
            elif d.iat and iat.get(d.iat, '') in X87_IMPORTS:
                calls[i] = ('import', iat[d.iat])
            else:
                calls[i] = ('native',)
            if tag != 'call' and d.iat and iat.get(d.iat, '') in NORETURN_IMPORTS:
                exits[i] = ('stop',)
                continue
            flow(i, nxt)
        else:
            if tag == 'tail':
                exits[i] = ('tail', None)
                continue
            flow(i, nxt)
    b.insns, b.succ, b.exit, b.call = insns, succ, exits, calls
    assert all(0 <= s_ < n for row in succ for s_ in row), f.name
    return b


def mark_noreturn(bodies):
    noreturn = set()
    while True:
        grew = False
        for name, b in bodies.items():
            if name in noreturn or not b.insns:
                continue
            entry = b.f.addresses.index(b.f.va) if b.f.va in b.f.addresses else 0
            seen, work, returns = {entry}, [entry], False
            while work and not returns:
                i = work.pop()
                e = b.exit[i]
                c = b.call[i]
                if c is not None and c[0] == 'direct' and c[1] in noreturn:
                    continue
                if e is not None and e[0] in ('ret', 'tail'):
                    returns = True
                    break
                for s_ in b.succ[i]:
                    if s_ not in seen:
                        seen.add(s_)
                        work.append(s_)
            if not returns:
                noreturn.add(name)
                grew = True
        if not grew:
            break
    for b in bodies.values():
        for i, c in enumerate(b.call):
            if c is not None and c[0] == 'direct' and c[1] in noreturn:
                b.succ[i] = []
                b.exit[i] = ('stop',)
    return noreturn


def prepare(out_dir=OUT_DIR):
    """Parsed bodies with recovered computed jumps, decoded instructions and CFGs."""
    start = time.time()
    modules = manifest()
    functions = parse_functions(modules)
    print(f'parsed {len(functions)} bodies in {time.time() - start:.1f}s', flush=True)
    by_module = defaultdict(set)
    for f in functions.values():
        by_module[f.module['name']].update(f.addresses)
    decoded = {}
    for module in modules:
        decoded.update(load_decoded(module, by_module[module['name']], out_dir))
    print(f'decoded {len(decoded)} instructions in {time.time() - start:.1f}s', flush=True)

    def jump_address(entry):
        position, label, last = entry
        if label is not None:
            return label
        d = decoded.get(last)
        return d.address + d.size if d is not None else None
    missing = defaultdict(set)
    for f in functions.values():
        for entry in f.switch_after:
            address = jump_address(entry)
            if address is not None and address not in decoded:
                missing[f.module['name']].add(address)
    for module in modules:
        if missing[module['name']]:
            decoded.update(load_decoded(module, missing[module['name']] | by_module[module['name']], out_dir))
    switches = 0
    for f in functions.values():
        for entry in sorted(f.switch_after, key=lambda e: -e[0]):
            target = jump_address(entry)
            j = decoded.get(target) if target is not None else None
            if j is None or j.kind not in ('ijmp', 'jmp') or target in f.addresses:
                continue
            f.addresses.insert(entry[0], target)
            f.tags.insert(entry[0], 'switch')
            switches += 1
    print(f'recovered {switches} computed jumps', flush=True)
    names = {f.va: f.name for f in functions.values()}
    iat = iat_names(modules)
    bodies = {name: build_body(f, decoded, names, iat) for name, f in functions.items()}
    noreturn = mark_noreturn(bodies)
    print(f'built bodies, {len(noreturn)} noreturn ({time.time() - start:.1f}s)', flush=True)
    return functions, bodies


# --- x87 depth analysis -------------------------------------------------------
PUSH = {'fld', 'fild', 'fbld', 'fldz', 'fld1', 'fldpi', 'fldl2e', 'fldl2t', 'fldlg2', 'fldln2'}
POP1 = {'fstp', 'fistp', 'fisttp', 'fbstp', 'fcomp', 'fucomp', 'ficomp', 'fcompi', 'fucompi', 'fcomip',
        'fucomip', 'faddp', 'fsubp', 'fsubrp', 'fmulp', 'fdivp', 'fdivrp', 'fpatan', 'fyl2x', 'fyl2xp1'}
POP2 = {'fcompp', 'fucompp'}
ZERO = {'fst', 'fist', 'fcom', 'fucom', 'fcomi', 'fucomi', 'ficom', 'fadd', 'fsub', 'fsubr', 'fmul', 'fdiv',
        'fdivr', 'fiadd', 'fisub', 'fisubr', 'fimul', 'fidiv', 'fidivr', 'fchs', 'fabs', 'fsqrt', 'fsin', 'fcos',
        'frndint', 'fscale', 'f2xm1', 'fxam', 'ftst', 'fxch', 'fldcw', 'fnstcw', 'fstcw', 'fnstsw', 'fstsw',
        'fnclex', 'fclex', 'fwait', 'wait', 'fnop', 'ffree', 'fcmovb', 'fcmove', 'fcmovbe', 'fcmovu',
        'fcmovnb', 'fcmovne', 'fcmovnbe', 'fcmovnu'}
READS_ST1 = {'fcompp', 'fucompp', 'fpatan', 'fyl2x', 'fyl2xp1', 'fscale', 'fprem', 'fprem1'}
# Anything else that names the x87 state: opaque (the body keeps the window).
BARRIER = {'fninit', 'finit', 'fnsave', 'fsave', 'frstor', 'fxsave', 'fxrstor', 'fldenv', 'fnstenv', 'fstenv',
           'fincstp', 'fdecstp', 'ffreep', 'fsincos', 'fxtract', 'fprem', 'fprem1'}
# The window model gives emms no effect (the lifter emits a comment).
NO_X87 = {'emms', 'femms'}
ST_RE = re.compile(r'st\((\d)\)')


def x87_effect(d):
    """(delta, highest st index read) or None for a barrier; (0, -1) for non-x87."""
    m = d.mnemonic.split()[-1]
    if m in BARRIER:
        return None
    if m in NO_X87:
        return 0, -1
    if m == 'fptan':
        return 1, 0                                    # st(0) = tan(st(0)), then push 1.0
    if m in PUSH:
        delta = 1
    elif m in POP1:
        delta = -1
    elif m in POP2:
        delta = -2
    elif m in ZERO:
        delta = 0
    elif m.startswith('f') and m not in ('fs', 'fs:'):
        return None                                    # an x87 form this pass does not know
    else:
        return 0, -1
    reads = [int(k) for k in ST_RE.findall(d.op_str)]
    high = max(reads) if reads else -1
    if m in PUSH:
        return delta, high
    if m in ('fldcw', 'fnstcw', 'fstcw', 'fnstsw', 'fstsw', 'fnclex', 'fclex', 'fwait', 'wait', 'fnop', 'ffree'):
        return 0, -1
    high = max(high, 0)
    if m in READS_ST1 or (m in ('faddp', 'fsubp', 'fsubrp', 'fmulp', 'fdivp', 'fdivrp', 'fxch',
                                'fcom', 'fcomp', 'fucom', 'fucomp') and not d.op_str.strip()):
        high = max(high, 1)
    return delta, high


class Summary:
    __slots__ = ('ok', 'known', 'c', 'p', 'depth', 'calls', 'reason', 'maxdepth', 'delta', 'pending')


EXIT = 'exit'


def callee_interface(c, summaries):
    """(c, p) of a call target when it is known, else None."""
    if c[0] == 'import':
        return X87_IMPORTS.get(c[1])
    if c[0] == 'direct' and c[1]:
        callee = summaries.get(c[1])
        if callee is not None and callee.ok and callee.known:
            return callee.c, callee.p
    return None


def call_result(b, i, effects, summaries, limit=8192):
    """Whether call i, made at an empty stack to a callee of unknown interface,
    leaves a float, judged from every path after it (see the Heroes V tool)."""
    if b.exit[i] is not None:
        return 0 if b.exit[i][0] == 'stop' else None
    results, seen, steps = set(), set(), 0
    work = [(t, 0) for t in b.succ[i]]
    while work:
        j, local = work.pop()
        if (j, local) in seen:
            continue
        seen.add((j, local))
        steps += 1
        if steps > limit:
            return None
        c = b.call[j]
        if c is not None:
            known = callee_interface(c, summaries)
            cg = known[0] if known is not None else 0
            if cg == local:
                results.add(0)
            elif cg == local + 1:
                results.add(1)
            else:
                return None
            continue
        delta, high = effects[j]
        if high >= local or local + delta < 0:
            results.add(1)
            continue
        local += delta
        e = b.exit[j]
        if e is not None:
            if e[0] != 'stop':
                results.add(EXIT)
            continue
        for t in b.succ[j]:
            work.append((t, local))
    decided = results - {EXIT}
    if len(decided) > 1:
        return None
    if decided:
        return decided.pop()
    return EXIT if results else 0


def analyse_body(name, b, summaries, assume=0):
    """Depth at every instruction (relative to entry) and the (c, p) interface."""
    s = Summary()
    s.ok, s.known, s.c, s.p, s.depth, s.calls, s.reason, s.maxdepth, s.delta, s.pending = (
        False, True, 0, 0, {}, {}, '', 0, {}, False)
    n = len(b.insns)
    if not n:
        s.reason = 'empty'
        return s
    entry = b.f.addresses.index(b.f.va) if b.f.va in b.f.addresses else 0
    effects = []
    for d in b.insns:
        if d is None:
            effects.append((0, -1))
            continue
        e = x87_effect(d)
        if e is None:
            s.reason = 'barrier ' + d.mnemonic
            return s
        effects.append(e)
    depth = [None] * n
    depth[entry] = 0
    work = [entry]
    need = 0
    net = None
    inferred = False
    while work:
        i = work.pop()
        d0 = depth[i]
        delta, high = effects[i]
        c = b.call[i]
        if c is not None:
            known = callee_interface(c, summaries)
            if known is not None:
                cg, pg = known
                if d0 < cg:
                    s.reason = f'call consumes below entry at {b.f.addresses[i]:08X}'
                    return s
            else:
                if d0 != 0:
                    s.reason = f'call at depth {d0} with unknown x87 interface at {b.f.addresses[i]:08X}'
                    return s
                pg = call_result(b, i, effects, summaries)
                if pg is None:
                    s.reason = f'unknown result at {b.f.addresses[i]:08X}'
                    return s
                if pg == EXIT:
                    pg = assume
                    s.pending = True
                cg = 0
                inferred = True
            after = d0 - cg + pg
            s.calls[b.f.addresses[i]] = (cg, pg)
        else:
            if high >= 0 and d0 - 1 - high < -need:
                need = high + 1 - d0
            after = d0 + delta
            if after < -need:
                need = -after
        e = b.exit[i]
        if e is not None:
            if e[0] == 'ret':
                exit_net = after
            elif e[0] == 'tail':
                target = e[1]
                if isinstance(target, tuple):
                    known = X87_IMPORTS.get(target[1])
                else:
                    summary = summaries.get(target) if target else None
                    known = (summary.c, summary.p) if summary is not None and summary.ok and summary.known else None
                if known is None:
                    s.reason = f'tail with unknown x87 interface at {b.f.addresses[i]:08X}'
                    return s
                if isinstance(target, tuple):
                    # An import thunk: hands the caller's stack to the import.
                    if after != 0:
                        s.reason = f'import tail at depth {after}'
                        return s
                    exit_net = known[1] - known[0]
                    need = max(need, known[0])
                else:
                    if after != 0 or known[0]:
                        s.reason = f'tail at depth {after}'
                        return s
                    exit_net = known[1]
            else:
                exit_net = None
            if exit_net is not None:
                if net is None:
                    net = exit_net
                elif net != exit_net:
                    s.reason = 'inconsistent return depth'
                    return s
        for t in b.succ[i]:
            if depth[t] is None:
                depth[t] = after
                work.append(t)
            elif depth[t] != after:
                s.reason = f'depth conflict at {b.f.addresses[t]:08X}'
                return s
        s.maxdepth = max(s.maxdepth, after, d0)
    if need and inferred:
        s.reason = 'consumes below entry with an inferred call'
        return s
    s.c = need
    s.p = (net if net is not None else 0) + need
    if s.p < 0 or s.p > 2 or s.c > 3 or (s.pending and (s.c or s.p > 1)):
        s.reason = f'unusual interface c={s.c} p={s.p}'
        return s
    s.depth = {b.f.addresses[i]: depth[i] for i in range(n) if depth[i] is not None}
    s.delta = {f'{b.f.addresses[i]:08X}': effects[i][0] for i in range(n)
               if b.call[i] is None and effects[i][0]}
    s.ok = True
    return s


def analyse_full(name, b, summaries):
    s0 = analyse_body(name, b, summaries, 0)
    if not s0.pending:
        return s0
    s1 = analyse_body(name, b, summaries, 1)
    if s0.ok and s1.ok:
        s0.known = False
        return s0
    if s1.ok:
        return s1
    return s0


def analyse(out=X87_JSON):
    start = time.time()
    functions, bodies = prepare(out.parent)
    summaries = {}
    for round_ in range(40):
        changed = 0
        for name, b in bodies.items():
            s = analyse_full(name, b, summaries)
            old = summaries.get(name)
            if (old is None or old.ok != s.ok or (s.ok and ((old.c, old.p, old.known) != (s.c, s.p, s.known)
                                                              or old.calls != s.calls))):
                changed += 1
            summaries[name] = s
        ok = sum(1 for s in summaries.values() if s.ok)
        print(f'x87 round {round_ + 1}: ok={ok} changed={changed} ({time.time() - start:.1f}s)', flush=True)
        if not changed:
            break
    else:
        raise SystemExit('x87 summaries did not converge')
    for name, s in summaries.items():
        if not s.ok:
            continue
        b = bodies[name]
        for i, c in enumerate(b.call):
            if c is None:
                continue
            known = callee_interface(c, summaries)
            if known is not None:
                assert s.calls.get(b.f.addresses[i], known) == known, (name, c)
    reasons = Counter(s.reason.split(' at ')[0].split(' ')[0] for s in summaries.values() if not s.ok)
    print('not ok:', reasons.most_common(12), flush=True)
    interfaces = Counter((s.c, s.p) if s.known else 'unknown' for s in summaries.values() if s.ok)
    print('interfaces:', interfaces.most_common(10), flush=True)
    data = {name: dict(c=s.c, p=s.p, depth={f'{a:08X}': v for a, v in s.depth.items() if v},
                       calls={f'{a:08X}': list(v) for a, v in s.calls.items() if v != (0, 0)},
                       delta=s.delta, **({} if s.known else dict(u=1)))
            for name, s in summaries.items() if s.ok}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(data, separators=(',', ':')) + '\n')
    # Why the others keep the window (for widening the pass where it pays).
    rejected = {name: s.reason for name, s in summaries.items() if not s.ok}
    out.with_name(out.stem + '-rejected.json').write_text(json.dumps(rejected, indent=0, sort_keys=True) + '\n')
    return summaries


# --- source rewrite ----------------------------------------------------------
class Unsupported(Exception):
    pass


FUNCTION_START = re.compile(rb'^void (sub_[0-9A-F]{8})\(void\) \{\r?$', re.M)
FUNCTION_END = re.compile(rb'\n\}\r?\n')
COMMENT_ADDR = re.compile(rb'/\* 0x([0-9A-F]{8}): ')
LABEL_LINE = re.compile(rb'^L_([0-9A-F]{8}):')
TOKEN = re.compile(rb'fp_push_i64\(|fp_push\(|fp_pop\(\)|fp_xch\(|_st\[(\d+)\]|g_st_i64\[(\d+)\]'
                   rb'|\b(?:g_st|_st|g_st_i64|_fp_top|g_fp_top|g_fp_sp|g_st_win|g_st_i64_win|fp_window_\w+|'
                   rb'void_fpu_remainder|void_fpu_state|fp_push_impl|fp_pop_impl)\b')
CALL_START = re.compile(rb'RECOMP_(?:CALL|ICALL)\(')
SKIP_BODY = (b'RECOMP_EH_ENTER', b'void_fpu_remainder', b'void_fpu_state')


def slot(n):
    return b'_x87s%d' % n if n >= 0 else b'_x87m%d' % -n


def islot(n):
    return b'_x87i%d' % n if n >= 0 else b'_x87im%d' % -n


def matching(text, start):
    """Index just past the parenthesis that closes the one before `start`."""
    depth, i = 1, start
    while i < len(text):
        c = text[i]
        if c == 0x28:
            depth += 1
        elif c == 0x29:
            depth -= 1
            if not depth:
                return i + 1
        i += 1
    raise Unsupported('unbalanced')


class Rewriter:
    def __init__(self, low):
        self.low = low          # lowest slot index that exists (-c)
        self.used = set()

    def use(self, n):
        if n < self.low:
            raise Unsupported('slot below the consumed values')
        self.used.add(n)
        return n

    def code(self, text, depth, allow_stack=True):
        out, pos = [], 0
        while True:
            m = TOKEN.search(text, pos)
            if not m:
                break
            out.append(text[pos:m.start()])
            tok = m.group(0)
            if tok in (b'fp_push(', b'fp_push_i64('):
                if not allow_stack:
                    raise Unsupported('push inside an operand')
                end = matching(text, m.end())
                inner, _ = self.code(text[m.end():end - 1], depth, allow_stack=False)
                n = self.use(depth)
                macro = b'X87_SET' if tok == b'fp_push(' else b'X87_SET_I64'
                out.append(b'%s(%s, %s, (%s))' % (macro, slot(n), islot(n), inner))
                depth += 1
                pos = end
            elif tok == b'fp_pop()':
                if not allow_stack:
                    raise Unsupported('pop inside an operand')
                n = self.use(depth - 1)
                out.append(slot(n))
                depth -= 1
                pos = m.end()
            elif tok == b'fp_xch(':
                end = matching(text, m.end())
                k = int(text[m.end():end - 1])
                a, b = self.use(depth - 1), self.use(depth - 1 - k)
                out.append(b'X87_XCH(%s, %s, %s, %s)' % (slot(a), islot(a), slot(b), islot(b)))
                pos = end
            elif m.group(1) is not None:
                out.append(slot(self.use(depth - 1 - int(m.group(1)))))
                pos = m.end()
            elif m.group(2) is not None:
                out.append(islot(self.use(depth - 1 - int(m.group(2)))))
                pos = m.end()
            else:
                raise Unsupported('global x87 state: ' + tok.decode())
        out.append(text[pos:])
        return b''.join(out), depth


def transform_function(name, body, info):
    """Rewrite one body; raise Unsupported to leave it on the window."""
    if any(marker in body for marker in SKIP_BODY):
        raise Unsupported('excluded body')
    if not TOKEN.search(body) and not info.get('calls'):
        return None
    c, p = info['c'], info['p']
    depths = {int(a, 16): v for a, v in info.get('depth', {}).items()}
    calls = {int(a, 16): tuple(v) for a, v in info.get('calls', {}).items()}
    net = p - c
    if (c or p) and (b'RECOMP_ITAIL' in body or b'RECOMP_RET_JUMP' in body or b'goto _ljump' in body):
        raise Unsupported('x87 interface with a tail exit')
    rw = Rewriter(-c)
    lines = body.split(b'\n')
    out = []
    depth = 0
    entered = False
    deltas = {int(a, 16): v for a, v in info.get('delta', {}).items()}
    current, start_depth = None, 0
    for line in lines:
        cm = COMMENT_ADDR.search(line)
        lm = LABEL_LINE.match(line)
        if cm or lm:
            # The text of the previous instruction must move the stack exactly
            # as its decoded instruction does.
            if current is not None and current not in calls and depth - start_depth != deltas.get(current, 0):
                raise Unsupported('instruction text and stack effect differ')
            current = None
        if lm:
            depth = depths.get(int(lm.group(1), 16), 0)
        if cm:
            address = int(cm.group(1), 16)
            depth = depths.get(address, 0)
            current, start_depth = address, depth
        else:
            address = None
        cut = line.find(b'/*')
        code, tail = (line, b'') if cut < 0 else (line[:cut], line[cut:])
        call = CALL_START.search(code)
        if call and address in calls:
            cg, pg = calls[address]
            pre, depth = rw.code(code[:call.start()], depth)
            before = depth
            end = matching(code, call.end())
            stmt_end = end + 1 if code[end:end + 1] == b';' else end
            spill = b''.join(b'X87_GPUSH(%s, %s); ' % (slot(rw.use(k)), islot(k))
                             for k in range(before - cg, before))
            fill = b''.join(b' X87_GPOP(%s, %s);' % (slot(rw.use(k)), islot(k))
                            for k in range(before - cg + pg - 1, before - cg - 1, -1))
            code = pre + spill + code[call.start():stmt_end] + fill + code[stmt_end:]
            depth = before - cg + pg
        else:
            code, depth = rw.code(code, depth)
        line = code + tail
        if b'return;' in line and cm is not None and b': ret' in tail:
            if depth != net:
                raise Unsupported('return depth')
            push = b''.join(b'X87_GPUSH(%s, %s); ' % (slot(rw.use(k)), islot(k)) for k in range(-c, net))
            push += b'X87_CHECK_EXIT(0x%08Xu, %d, %d, %d); ' % (int(name[4:], 16), c, p, int(info.get('u', 0)))
            indent = len(line) - len(line.lstrip())
            line = line[:indent] + push + line[indent:]
        out.append(line)
        if not entered and b'RECOMP_ENTER(' in line:
            entered = True
            pops = b''.join(b'X87_GPOP(%s, %s); ' % (slot(rw.use(k)), islot(k)) for k in range(-1, -c - 1, -1))
            out.append(b'    X87_CHECK_ENTER(); ' + pops)
    if not rw.used:
        return None
    names = sorted(rw.used)
    declare = (b'    double ' + b', '.join(slot(n) + b' = 0' for n in names) + b';' +
               b' int64_t ' + b', '.join(islot(n) + b' = 0' for n in names) + b';')
    text = b'\n'.join(out)
    marker = b'RECOMP_REGS_LOCALS'
    at = text.find(marker)
    if at < 0:
        raise Unsupported('no locals marker')
    at = text.find(b'\n', at)
    return text[:at + 1] + declare + b'\r' * (b'\r\n' in text) + b'\n' + text[at + 1:]


class Plan:
    def __init__(self, path=X87_JSON):
        self.info = json.loads(Path(path).read_text())
        self.stats = Counter()


def apply_x87(text, plan, only=None):
    """Rewrite every body of one generated source the plan allows (or only
    those named in `only`)."""
    parts, pos = [], 0
    for match in FUNCTION_START.finditer(text):
        end = FUNCTION_END.search(text, match.end())
        if not end or match.start() < pos:
            continue
        name = match[1].decode()
        body = text[match.end():end.end()]
        info = plan.info.get(name)
        new = None
        if only is not None and name not in only:
            plan.stats['not selected'] += 1
        elif info is not None:
            try:
                new = transform_function(name, body, info)
                plan.stats['rewritten' if new is not None else 'no x87'] += 1
            except Unsupported as error:
                plan.stats['kept: ' + str(error).split(':')[0]] += 1
                new = None
        else:
            plan.stats['no summary'] += 1
        parts += [text[pos:match.end()], new if new is not None else body]
        pos = end.end()
    return b''.join(parts + [text[pos:]]) if parts else text


if __name__ == '__main__':
    analyse()
