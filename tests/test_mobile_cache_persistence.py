"""Run the production PCS snapshot/atomic writer boundary with real core register saves."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from tests.host_environment import CLANG, environment
from tests.test_mobile_shaders import expanded, function
from tools import port_mobile_shaders as mobile

ROOT = Path(__file__).resolve().parents[1]


class MobileCachePersistenceTest(unittest.TestCase):
    def test_snapshot_state_and_atomic_commit(self):
        files = (ROOT / "android/host/files.c").read_text(encoding="utf-8")
        core = (ROOT / "android/host/core.c").read_text(encoding="utf-8")
        host = (ROOT / "android/host/host.h").read_text(encoding="utf-8")
        runtime = (ROOT / "android/runtime/recomp_types.h").read_text(encoding="utf-8")
        regs = host[host.index("typedef struct void_regs {"):host.index("} void_regs;") + len("} void_regs;")]
        end = runtime.index("} xmm_t;") + len("} xmm_t;")
        xmm = runtime[runtime.rindex("typedef union {", 0, end):end]
        globals_ = core[core.index("VOID_HIDDEN uint32_t g_eax"):core.index("static char *void_bioshock_object_name")]
        production = files[files.index("/* Only this guest writer's CreateFile"):files.index("/* Host side of CreateFile:")]
        predicate = function(files, "static int mobile_shader_cache(")
        save = function(core, "void void_regs_save(") + function(core, "void void_regs_load(")
        original = Path(json.loads((ROOT / "config/inputs.json").read_text())["source_root"])
        original_cache = mobile.checked(original / "Builds/Release/ShaderCache.pcs", mobile.PCS_SHA256)
        seed = mobile.unpack_cache(original_cache)
        rows = [row for row in seed if "ImageSpace.hlsl" not in row[0]][:2]

        def array(name, data):
            return f"static const unsigned char {name}[]={{" + ",".join(str(b) for b in data) + "};\n"

        inputs = "".join(array(f"key{i}", (row[0] + "\0").encode("utf-16le")) +
                         array(f"hash{i}", row[1]) + array(f"code{i}", row[3]) for i, row in enumerate(rows))
        initializers = "\n".join(
            f"row({i},key{i},sizeof key{i},hash{i},code{i},sizeof code{i},{row[2]}u);" for i, row in enumerate(rows))
        program = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#define VOID_HIDDEN
#define VOID_GUEST_BASE 0x2000000000ull
#define GPTR(a) ((void*)(uintptr_t)(VOID_GUEST_BASE+(uint32_t)(a)))
#define MEM32(a) (*(uint32_t*)GPTR(a))
#define MEM16(a) (*(uint16_t*)GPTR(a))
#define RECOMP_X87_WINDOW
#define RECOMP_X87_SLOTS 256
#define ICALL_TRACE_SIZE 128
typedef struct recomp_ehframe recomp_ehframe_t;
/* TYPES */
recomp_ehframe_t *g_eh_frames; uint32_t g_eh_obj,g_eh_info;
#define g_st (*(double (*)[8])(g_st_win+g_fp_sp))
#define g_st_i64 (*(int64_t (*)[8])(g_st_i64_win+g_fp_sp))
/* GLOBALS */
/* CORE SAVE */
static int owned=1,oom,allocs,frees,calls,mode,fail_sync,fail_rename,fail_temp;
static uint32_t allocation,allocated,last_error=1234,source_bytes=0x50000;
static char target[2048],victim[2048],overlay_root[1024]="overlay";
static const char *enabled="1";
static const char *fixture_getenv(const char *name){assert(!strcmp(name,"BIOSHOCK_MOBILE_SHADERS"));return enabled;}
#define getenv fixture_getenv
#define strcasecmp _stricmp
#define strdup _strdup
#define off_t __int64
typedef ptrdiff_t ssize_t;
static int fixture_pread(int fd,void *out,size_t bytes,off_t at){assert(_lseeki64(fd,at,SEEK_SET)==at);return _read(fd,out,(unsigned)bytes);}
static int fixture_sync(int fd){return fail_sync?-1:_commit(fd);}
static int fixture_rename(const char *from,const char *to){if(fail_rename)return -1;return MoveFileExA(from,to,MOVEFILE_REPLACE_EXISTING)?0:-1;}
static int fixture_temp(char *path){
    if(fail_temp){strcpy(path,victim);errno=EACCES;return -1;}
    assert(!_mktemp_s(path,strlen(path)+1));return _open(path,_O_BINARY|_O_CREAT|_O_EXCL|_O_RDWR,_S_IREAD|_S_IWRITE);
}
#define pread fixture_pread
#define fsync fixture_sync
#define rename fixture_rename
#define mkstemp fixture_temp
#define close _close
#define unlink _unlink
#define stat _stat64
#define fstat _fstat64
int void_machine_is_owned(void){return owned;}
int void_vm_readable(uint32_t a,uint32_t n){
    uint64_t end=(uint64_t)a+n;
    return (a>=0x10000&&end<=0x10000+source_bytes)||(allocation&&a>=allocation&&end<=(uint64_t)allocation+allocated);
}
uint32_t void_galloc(uint32_t size){
    ++allocs;if(oom)return 0;assert(!allocation);allocation=0x20000000;allocated=size;
    assert(VirtualAlloc(GPTR(allocation),(size+65535)&~65535u,MEM_COMMIT,PAGE_READWRITE)==GPTR(allocation));return allocation;
}
void void_gfree(uint32_t ptr){assert(ptr==allocation);++frees;assert(VirtualFree(GPTR(ptr),(allocated+65535)&~65535u,MEM_DECOMMIT));allocation=allocated=0;}
uint32_t void_get_last_error(void){return last_error;}
void void_set_last_error(uint32_t value){last_error=value;}
void void_log(const char *format,...){(void)format;}
static char *to_host_ex(const char *name,int flags,int *parent,int *leaf,char *normalized){
    assert(!strcmp(name,"ShaderCache.pcs")&&!flags&&!leaf&&!normalized);*parent=1;return strdup(target);
}
uint32_t void_call_guest_this(uint32_t,uint32_t,int,const uint32_t*);
/* PREDICATE */
/* PRODUCTION */
/* INPUTS */
typedef struct state {void_regs regs;double fp[256];int64_t ints[256];int sp;} state;
static void capture(state *out){memset(out,0,sizeof *out);void_regs_save(&out->regs);memcpy(out->fp,g_st_win,sizeof out->fp);memcpy(out->ints,g_st_i64_win,sizeof out->ints);out->sp=g_fp_sp;}
static void registers(void){
    g_eax=1;g_ecx=2;g_edx=3;g_ebx=4;g_esp=0x50000;g_ebp=6;g_esi=7;g_edi=8;
    g_flag_k=9;g_flag_a=10;g_flag_b=11;g_flag_cf=1;g_fs_base=12;g_cur_func=13;
    g_fp_sp=120;g_fp_top=5;void_fpu_cmp=2;g_fpu_cw=0x37F;g_fp_round_mask=~3ull;g_fp_store_mask=~7ull;
    for(unsigned i=0;i<256;++i){uint64_t bits=0x7FF8000000000123ull+i;memcpy(g_st_win+i,&bits,8);g_st_i64_win[i]=9007199254740993ll+i;}
    memset(g_mm,0x34,sizeof g_mm);memset(g_xmm,0x56,sizeof g_xmm);g_mxcsr=0x1F80;
    g_eh_frames=(void*)(uintptr_t)0x1234;g_eh_obj=14;g_eh_info=15;g_backedges=16;errno=E2BIG;last_error=1234;
}
static void row(unsigned i,const void *key,unsigned keybytes,const void *hash,const void *code,unsigned bytes,uint32_t stamp){
    uint32_t r=0x12000+i*36,k=0x14000+i*4096,v=0x18000+i*16,c=0x20000+i*65536;
    MEM32(r+4)=k;MEM32(r+8)=MEM32(r+12)=keybytes/2;memcpy(GPTR(k),key,keybytes);memcpy(GPTR(r+16),hash,16);
    MEM32(r+32)=v;MEM32(v)=bytes;MEM32(v+4)=stamp;MEM32(v+8)=c;memcpy(GPTR(c),code,bytes);
}
static void initialize(void){
    memset(GPTR(0x10000),0,0x50000);MEM32(0x11000)=0x12000;MEM32(0x11004)=MEM32(0x11008)=2;
    MEM32(0x11014)=0x13000;MEM32(0x11018)=MEM32(0x1101C)=16;MEM32(0x11024)=1;
    const uint16_t name[]={'S','h','a','d','e','r','C','a','c','h','e','.','p','c','s',0};memcpy(GPTR(0x13000),name,sizeof name);
    /* INITIALIZE ROWS */
    owned=1;oom=mode=fail_sync=fail_rename=fail_temp=0;enabled="1";strcpy(overlay_root,"overlay");registers();
}
static void marker(const char *path){FILE *f=fopen(path,"wb");assert(f);assert(fwrite("KEEP",1,4,f)==4);assert(!fclose(f));}
static void unchanged(const char *path){char bytes[5]={0};FILE *f=fopen(path,"rb");assert(f&&fread(bytes,1,5,f)==4&&!memcmp(bytes,"KEEP",4));fclose(f);}
static DWORD WINAPI other_thread(void *argument){
    (void)argument;char *host=strdup(target);mobile_cache_redirect_write(&host,"C:\\Game\\ShaderCache.pcs",1);assert(!strcmp(host,target));free(host);
    void_bioshock_mobile_cache_flush(0x11000);return 0;
}
static void emit(FILE *f,const void *data,size_t size){assert(fwrite(data,1,size,f)==size);}
uint32_t void_call_guest_this(uint32_t va,uint32_t snapshot,int nargs,const uint32_t *args){
    assert(va==0x10AF7060u&&nargs==1&&args[0]==0&&snapshot==allocation);++calls;
    assert(MEM32(snapshot)!=0x12000&&MEM32(snapshot+20)!=0x13000);
    char *path=strdup(target);mobile_cache_redirect_write(&path,"C:\\Game\\ShaderCache.pcs",0);assert(!strcmp(path,target));
    mobile_cache_redirect_write(&path,"C:\\Game\\ShaderCache.pcs10",1);assert(!strcmp(path,target));
    mobile_cache_redirect_write(&path,"C:\\Game\\ShaderCache.pcs",1);assert(strcmp(path,target));
    HANDLE thread=CreateThread(NULL,0,other_thread,NULL,0,NULL);assert(thread);assert(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0);CloseHandle(thread);
    /* The source map is cleared/reused while the writer is parked; the snapshot must survive. */
    memset(GPTR(0x12000),0xDD,72);memset(GPTR(0x14000),0xEE,8192);memset(GPTR(0x18000),0xCC,32);memset(GPTR(0x20000),0xBB,0x20000);
    MEM32(0x11004)=MEM32(0x11024)=0;
    if(mode!=1){
        FILE *f=fopen(path,"wb");assert(f);uint32_t version=mode==3?12:13;emit(f,&version,4);
        for(unsigned i=0;i<MEM32(snapshot+4);++i){
            uint32_t r=MEM32(snapshot)+i*36,n=MEM32(r+8),v=MEM32(r+32);unsigned char compact[5];unsigned len=mobile_cache_compact(n,compact);
            emit(f,compact,len);emit(f,GPTR(MEM32(r+4)),n*2);
            if(mode==5&&i==0){unsigned char wrong[16];memcpy(wrong,GPTR(r+16),16);wrong[0]^=1;emit(f,wrong,16);}else emit(f,GPTR(r+16),16);
            emit(f,GPTR(v),8);
            if(mode==4&&i==0){unsigned char wrong=*(unsigned char*)GPTR(MEM32(v+8))^1;emit(f,&wrong,1);emit(f,GPTR(MEM32(v+8)+1),MEM32(v)-1);}
            else emit(f,GPTR(MEM32(v+8)),MEM32(v)-(mode==2&&i==1?1:0));
        }
        assert(!fclose(f));
    }
    free(path);
    void_regs changed;memset(&changed,0xA7,sizeof changed);changed.fp_top=1;void_regs_load(&changed);
    memset(g_st_win,0x9A,sizeof g_st_win);memset(g_st_i64_win,0x8B,sizeof g_st_i64_win);g_fp_sp=3;
    errno=ERANGE;last_error=777;return 1; /* Original Save returns 1 even on failure. */
}
static void flush(void){state before,after;capture(&before);void_bioshock_mobile_cache_flush(0x11000);capture(&after);assert(!memcmp(&before,&after,sizeof before));assert(errno==E2BIG&&last_error==1234);assert(!allocation&&!mobile_cache_pending[0]&&!mobile_cache_flush_busy);}
int main(int argc,char **argv){
    assert(argc==2||argc==3);strcpy(target,argv[1]);snprintf(victim,sizeof victim,"%s.unrelated",target);marker(target);marker(victim);
    assert(VirtualAlloc(GPTR(0),0x100000000ull,MEM_RESERVE,PAGE_NOACCESS)==GPTR(0));
    assert(VirtualAlloc(GPTR(0x10000),0x50000,MEM_COMMIT,PAGE_READWRITE)==GPTR(0x10000));
    if(argc==3){
        FILE *input=fopen(argv[2],"rb");assert(input&&!fseek(input,0,SEEK_END));long length=ftell(input);assert(length>0&&length<128*1024*1024);rewind(input);
        source_bytes=(uint32_t)length;assert(VirtualAlloc(GPTR(0x10000),(source_bytes+65535)&~65535u,MEM_COMMIT,PAGE_READWRITE)==GPTR(0x10000));
        assert(fread(GPTR(0x10000),1,source_bytes,input)==source_bytes);fclose(input);registers();flush();assert(calls==1&&allocs==1&&frees==1);
        assert(VirtualFree(GPTR(0),0,MEM_RELEASE));puts("PASS production snapshot/atomic verification of complete certified base plus real newly compiled mobile shader while source map clears");return 0;
    }
    for(unsigned test=0;test<17;++test){initialize();switch(test){
        case 0:enabled=NULL;break;case 1:enabled="0";break;case 2:enabled="true";break;case 3:overlay_root[0]=0;break;
        case 4:owned=0;break;case 5:MEM32(0x11020)=1;break;case 6:MEM32(0x11018)=15;break;
        case 7:MEM16(0x13000)='X';break;case 8:MEM32(0x11024)=0;break;case 9:MEM32(0x11004)=0;break;
        case 10:MEM32(0x12008)=4096;break;case 11:MEM32(0x18000)=0xFFFFFFFCu;break;case 12:oom=1;break;
        case 13:MEM16(0x14002)=0;break;case 14:MEM32(0x11004)=100001;break;case 15:MEM32(0x18008)=0xFFFFFFF0u;break;
        case 16:MEM32(0x11018)=MEM32(0x1101C)=18;MEM16(0x1301E)='1';MEM16(0x13020)='0';MEM16(0x13022)=0;break;}
        int oldcalls=calls;flush();assert(calls==oldcalls);unchanged(target);
    }
    initialize();assert(!void_bioshock_mobile_cache(0xFFFFFFF0u));
    char *absent=NULL;mobile_cache_redirect_write(&absent,NULL,1);assert(!absent);
    MEM16(0x13000)='s';assert(void_bioshock_mobile_cache(0x11000));
    for(unsigned test=0;test<8;++test){initialize();if(test<5)mode=(int)test+1;else if(test==5)fail_sync=1;else if(test==6)fail_rename=1;else fail_temp=1;
        flush();unchanged(target);unchanged(victim);
    }
    initialize();int oldallocs=allocs;mobile_cache_flush_busy=1;void_bioshock_mobile_cache_flush(0x11000);assert(allocs==oldallocs);mobile_cache_flush_busy=0;unchanged(target);
    initialize();flush();assert(calls==8&&allocs==frees+1);unchanged(victim);
    assert(VirtualFree(GPTR(0),0,MEM_RELEASE));
    puts("PASS production snapshot, source-clear during writer, all core registers/x87/EH/lazy flags, TLS redirection, nonblocking busy/OOM/invalid guards, atomic old-cache preservation on failed/truncated/corrupt/fsync/rename/temp paths");
}
'''
        program = (program.replace("/* TYPES */", xmm + "\n" + regs)
                   .replace("/* GLOBALS */", globals_).replace("/* CORE SAVE */", save)
                   .replace("/* PREDICATE */", predicate).replace("/* PRODUCTION */", production)
                   .replace("/* INPUTS */", inputs).replace("/* INITIALIZE ROWS */", initializers))
        with tempfile.TemporaryDirectory(prefix="bioshock-mobile-persist-", dir=ROOT / "build") as temporary:
            folder = Path(temporary).resolve()
            source, obj, binary, output = folder / "probe.c", folder / "probe.obj", folder / "probe.exe", folder / "cache.pcs"
            source.write_text(program, encoding="utf-8")
            env, linker = environment("x64")
            logs = []
            for command in ([str(CLANG), "/nologo", "/O2", "/W3", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                             "/clang:--target=x86_64-pc-windows-msvc", "/c", str(source), "/Fo" + str(obj)],
                            [str(linker), "/nologo", str(obj), "kernel32.lib", "/OUT:" + str(binary)],
                            [str(binary), str(output)]):
                result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=30)
                logs.append(result.stdout + result.stderr)
                self.assertEqual(result.returncode, 0, logs[-1])
            actual = mobile.unpack_cache(output.read_bytes())
            self.assertEqual(actual, rows)
            sources = {name: source.decode("utf-8") for name, _, _, source in mobile.unpack(
                mobile.checked(original / "Builds/Release/shaders.spk", mobile.SPK_SHA256))}
            full, counts = mobile.filter_cache(original_cache, sources)
            self.assertEqual(counts["kept"], 15535)
            adapted = {name: source.decode("utf-8") for name, _, _, source in mobile.unpack(mobile.adapt(
                mobile.checked(original / "Builds/Release/shaders.spk", mobile.SPK_SHA256),
                {name: mobile.checked(ROOT / "reference/mobile-shaders" / name, digest)
                 for name, digest in mobile.REFERENCE_SHA256.items()}))}
            source_text = expanded("ImageSpace.hlsl", adapted).replace(
                "LITERAL(bool, SRGBWrite, 0);", "LITERAL(bool, SRGBWrite, 1);").encode("utf-8")
            requests = folder / "requests"
            requests.mkdir()
            request = requests / "new.req"
            request.write_bytes(f"function=ToneMapPixelShader\nprofile=ps_3_0\nflags=0\ninclude=no\nmacros=0\nsource={len(source_text)}\n".encode() + source_text)
            result = subprocess.run([str(ROOT / "build/shader_compiler.exe"), str(request),
                                     str(original / "Builds/Release/d3dx9_33.dll")], capture_output=True,
                                    text=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            new_code = (folder / "new.bin").read_bytes()
            mobile.ctab_names(new_code)
            # Fixture identity only, never shipped as a cache asset. The payload
            # is an actual original-D3DX compilation of the adapted shader.
            encoded = "ImageSpace.hlslToneMapPixelShader\0".encode("utf-16le")
            count = len(encoded) // 2
            compact = bytearray([(count & 63) | (64 if count > 63 else 0)])
            count >>= 6
            while count:
                compact.append((count & 127) | (128 if count > 127 else 0))
                count >>= 7
            new_record = (compact + encoded + hashlib.sha256(source_text).digest()[:16] +
                          struct.pack("<II", len(new_code), 0x7FFFFFFE) + new_code)
            expected_full = full + new_record
            base_rows = mobile.unpack_cache(expected_full)
            # Build an actual engine memory layout from every certified record;
            # the production helper, not a Python serializer, performs the copy/verification.
            blob = bytearray(0x2000 + 36 * len(base_rows))
            struct.pack_into("<10I", blob, 0x1000, 0x12000, len(base_rows), len(base_rows), 0, 8,
                             0x11040, 16, 16, 0, 1)
            blob[0x1040:0x1060] = "ShaderCache.pcs\0".encode("utf-16le")
            for index, (name, key, stamp, code, _) in enumerate(base_rows):
                encoded = (name + "\0").encode("utf-16le")
                key_at = 0x10000 + len(blob)
                blob.extend(encoded)
                blob.extend(bytes((-len(blob)) % 4))
                value_at = 0x10000 + len(blob)
                blob.extend(struct.pack("<3I", len(code), stamp, value_at + 12))
                blob.extend(code)
                struct.pack_into("<9I", blob, 0x2000 + 36 * index, 0, key_at, len(encoded) // 2,
                                 len(encoded) // 2, *struct.unpack("<4I", key), value_at)
            memory = folder / "full-base.bin"
            memory.write_bytes(blob)
            full_output = folder / "full-cache.pcs"
            result = subprocess.run([str(binary), str(full_output), str(memory)], cwd=ROOT, env=env,
                                    capture_output=True, text=True, timeout=30)
            logs.append(result.stdout + result.stderr)
            self.assertEqual(result.returncode, 0, logs[-1])
            self.assertEqual(full_output.read_bytes(), expected_full)
            self.assertFalse(list(folder.glob("*.pending-*")))
            report = ROOT / "work/mobile-cache-persistence-281"
            report.mkdir(parents=True, exist_ok=True)
            (report / "native-fixture.log").write_text("\n".join(logs), encoding="utf-8")
            (report / "native-fixture.json").write_text(json.dumps({
                "status": "PASS", "records": len(actual), "bytes": output.stat().st_size,
                "sha256": hashlib.sha256(output.read_bytes()).hexdigest(), "device_executed": False,
                "full_base_records": 15535, "full_base_bytes": len(full),
                "full_base_sha256": hashlib.sha256(full).hexdigest(),
                "grown_records": len(base_rows), "grown_bytes": len(expected_full),
                "grown_sha256": hashlib.sha256(full_output.read_bytes()).hexdigest(),
                "new_mobile_bytecode_sha256": hashlib.sha256(new_code).hexdigest(),
                "scope": "Actual files.c helper and core save/load; guest serializer callback mocked; separate original PE oracle verifies serializer format",
                "sources": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in (
                    "android/host/files.c", "android/host/core.c", "tests/test_mobile_cache_persistence.py")}}, indent=2) + "\n")


if __name__ == "__main__":
    unittest.main()
