"""Read BioShock PC v141 textures and cook an optional UT3 UTX3 v2 cache.

Original packages and BLKs are opened read-only. The output must be outside the
input game directory. The converter retains every original BCn mip in the pack;
cache misses, unsupported formats and writes can use the ordinary D3D path.
"""
from concurrent.futures import ThreadPoolExecutor
import argparse
from collections import Counter
import ctypes as C
import hashlib
import json
import mmap
import os
from pathlib import Path
import struct
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct("<4I4Q6I")
ENTRY = struct.Struct("<3Q3I4HI")
FOURCC = {3:b"DXT1", 7:b"DXT3", 8:b"DXT5", 12:b"DXT5"}
MAX_PACKAGE = 256 << 20


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


class Reader:
    def __init__(self, data, position=0):
        self.data, self.position = data, position

    def take(self, size):
        require(0 <= size <= len(self.data)-self.position, "truncated package field")
        value = self.data[self.position:self.position+size]
        self.position += size
        return value

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def compact(self):
        value = self.take(1)[0]
        negative, result, continuation, shift = value & 128, value & 63, value & 64, 6
        while continuation:
            require(shift <= 27, "oversized compact index")
            value = self.take(1)[0]
            result |= (value & 127) << shift
            shift += 7
            continuation = value & 128
        require(result <= 0x7fffffff, "compact index overflow")
        return -result if negative else result

    def string(self):
        count = self.compact()
        require(0 <= count <= 4096, "invalid BioShock string length")
        if not count:
            return ""
        data = self.take(count*2)
        require(data[-2:] == b"\0\0", "unterminated BioShock string")
        return data[:-2].decode("utf-16-le")


def package(path):
    raw = path.read_bytes()
    require(len(raw) >= 72 and struct.unpack_from("<II", raw) == (0x9e2a83c1, 0x38008d), "not a BioShock PC v141 package")
    if struct.unpack_from("<I",raw,8)[0] == 1:
        require(len(raw) <= MAX_PACKAGE,"package exceeds 256 MiB")
        data = raw
    else:
        count, start = struct.unpack_from("<II", raw, 64)
        require(0 < count < 10000 and start == 68+count*4 and start <= len(raw), "invalid package compression index")
        offsets = struct.unpack_from(f"<{count}I", raw, 68)
        r, data = Reader(raw, start), bytearray(raw[:64])
        for offset in offsets:
            require(offset == r.position, "incorrect compressed block offset")
            size = r.u32()
            require(0 < size <= 1 << 20, "oversized compressed block")
            decoder = zlib.decompressobj()
            block = decoder.decompress(r.take(size), 32769)
            require(len(block) <= 32768 and decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail, "invalid compressed package block")
            require(len(data)+len(block) <= MAX_PACKAGE, "package exceeds 256 MiB")
            data.extend(block)
        require(r.position == len(raw), "trailing compressed package bytes")
    nc, no, ec, eo, ic, io = struct.unpack_from("<6I", data, 12)
    require(0 < nc <= 1000000 and 0 < ec <= 1000000 and 0 < ic <= 1000000, "package table count exceeds bound")
    require(64 < no < io < eo < len(data), "invalid package table offsets")
    r, names = Reader(data, no), []
    for _ in range(nc):
        names.append(r.string())
        r.take(8)
    require(r.position == io, "name table size mismatch")
    def fname():
        index, number = r.compact(), r.u32()
        require(0 <= index < len(names) and number <= 1000000, "invalid package name reference")
        return names[index]+(str(number-1) if number else "")
    imports = []
    for _ in range(ic):
        class_package, cls = fname(), fname()
        outer = struct.unpack("<i", r.take(4))[0]
        imports.append(dict(name=fname(), cls=cls, outer=outer))
    require(r.position == eo, "import table size mismatch")
    exports = []
    for _ in range(ec):
        cls, parent = r.compact(), r.compact()
        outer, unknown = struct.unpack("<2i", r.take(8))
        name, flags = fname(), r.take(8)
        size = r.compact()
        offset = r.compact() if size else 0
        r.take(4)
        require(-ic <= cls <= ec and -ic <= outer <= ec, "invalid export object reference")
        require(0 <= size <= no and (not size or 64 <= offset < no and offset+size <= no), "export outside serialized payload")
        exports.append(dict(name=name, class_ref=cls, offset=offset, size=size))
    for export in exports:
        cls = export["class_ref"]
        export["class"] = imports[-cls-1]["name"] if cls < 0 else exports[cls-1]["name"] if cls else "Class"
    return dict(data=data, names=names, exports=exports), hashlib.sha256(raw).hexdigest()


def texture(p, export):
    raw = p["data"][export["offset"]:export["offset"]+export["size"]]
    r, props = Reader(raw), {}
    require((r.u32(), r.u32()) == (4, 3), "unsupported texture property serialization")
    for _ in range(4096):
        index, number = r.compact(), r.u32()
        require(0 <= index < len(p["names"]) and number == 0, "invalid texture property name")
        name = p["names"][index]
        if name == "None":
            break
        info = r.take(1)[0]
        kind, code = info & 15, info >> 4 & 7
        require(1 <= kind <= 15, "invalid texture property type")
        if kind == 10:
            structure, extra = r.compact(), r.u32()
            require(0 <= structure < len(p["names"]) and extra == 0, "invalid property structure")
        size = [1,2,4,12,16][code] if code < 5 else r.take(1)[0] if code == 5 else struct.unpack("<H",r.take(2))[0] if code == 6 else r.u32()
        element = r.compact() if info & 128 and kind != 3 else 0
        props[name,element] = (kind,r.take(size))
    else:
        raise ValueError("texture property count exceeds bound")
    version = (r.u32(),r.u32())
    require(version in ((4,0),(4,1)), "unsupported texture bulk serialization")
    bulk_bytes = struct.unpack("<Q",r.take(8))[0] if version[1] else 0
    count = r.compact()
    require(0 <= count <= 16, "texture mip count exceeds bound")
    mips = []
    for _ in range(count):
        marker, version = r.u32(),r.u32()
        require(marker == 4 and version in (0,1,2), "unsupported texture mip serialization")
        if version == 1:
            r.take(12)
            r.take(r.compact())
        r.take(12)
        size = r.compact()
        require(0 <= size <= 64 << 20, "oversized texture mip")
        at = export["offset"]+r.position
        r.take(size)
        w,h = r.u32(),r.u32()
        ub,vb = r.take(2)
        require(0 < w <= 16384 and 0 < h <= 16384 and ub < 15 and vb < 15 and 1 << ub == w and 1 << vb == h, "invalid mip dimensions")
        mips.append(dict(width=w,height=h,bytes=size,offset=at))
    require(r.position == len(raw), "unparsed texture data")
    fmt = props.get(("Format",0))
    require(fmt is None or fmt[0] in (1,2), "invalid texture pixel format property")
    return dict(name=export["name"], format=int.from_bytes(fmt[1],"little") if fmt else None, bulk_bytes=bulk_bytes, mips=mips)


class Bulk:
    def __init__(self, content):
        self.streams, self.maps, self.items = [],{},{}
        try:
            self._open(content)
        except BaseException:
            self.close()
            raise

    def _open(self, content):
        self.paths = [content/"BulkContent/Catalog.bdc"]
        r = Reader(self.paths[0].read_bytes())
        require(r.take(1) == b"\0", "non-PC bulk catalog")
        r.take(12)
        count = r.compact()
        require(0 < count <= 1024, "bulk catalog file count exceeds bound")
        for _ in range(count):
            r.take(8)
            name = r.string()
            require(Path(name).name == name and name.lower().endswith(".blk"), "invalid bulk file name")
            stream = (content/"BulkContent"/name).open("rb")
            self.streams.append(stream)
            self.paths.append(Path(stream.name))
            mapping = mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ)
            self.maps[name] = mapping
            records = r.compact()
            require(0 <= records <= 1000000, "bulk record count exceeds bound")
            for _ in range(records):
                obj, package_name = r.string(),r.string()
                zero,offset,size,duplicate,index = struct.unpack("<5I",r.take(20))
                require(zero == 0 and size == duplicate and offset <= len(mapping) and size <= len(mapping)-offset, "invalid bulk range")
                self.items.setdefault(obj,[]).append((name,offset,size))
        require(r.position == len(r.data), "trailing bulk catalog bytes")

    def close(self):
        for mapping in self.maps.values():
            mapping.close()
        for stream in self.streams:
            stream.close()

    def chains(self,p,rec):
        mips = rec["mips"]
        sizes = [((m["width"]+3)//4)*((m["height"]+3)//4)*(8 if rec["format"] == 3 else 16) for m in mips]
        embedded = []
        for m,size in zip(mips,sizes):
            require(m["bytes"] in (0,size), "mip storage does not match BCn dimensions")
            embedded.append(bytes(p["data"][m["offset"]:m["offset"]+size]) if m["bytes"] else None)
        if all(x is not None for x in embedded):
            yield embedded
            return
        required = sum(sizes[:max(i+1 for i,x in enumerate(embedded) if x is None)])
        prefixes = {sum(sizes[:i]) for i in range(1,len(sizes)+1)}
        found = set()
        for filename,offset,size in self.items.get(rec["name"],[]):
            # CachedBulkDataSize may include platform allocation padding. Actual
            # dimensions determine exact stored mip boundaries, including DXT5N.
            if size < required or size not in prefixes:
                continue
            raw = self.maps[filename][offset:offset+size]
            levels,at,mismatch = [],0,False
            for source,n in zip(embedded,sizes):
                if at+n <= size:
                    value = raw[at:at+n]
                    mismatch |= source is not None and source != value
                else:
                    require(source is not None,"external texture is missing a lower mip")
                    value = source
                levels.append(value)
                at += n
            if mismatch:
                continue
            digest = hashlib.sha256(b"".join(levels)).digest()
            if digest not in found:
                found.add(digest)
                yield levels
        require(found, "no unambiguous bulk mip source for "+rec["name"])


def dds(rec,levels):
    result = bytearray(128)
    result[:4] = b"DDS "
    struct.pack_into("<7I",result,4,124,0xa1007,rec["mips"][0]["height"],rec["mips"][0]["width"],len(levels[0]),0,len(levels))
    struct.pack_into("<II4s",result,76,32,4,FOURCC[rec["format"]])
    struct.pack_into("<I",result,108,0x401008)
    return bytes(result)+b"".join(levels)


class Codec:
    def __init__(self,path):
        self.lib = C.CDLL(str(path))
        self.lib.texconv_convert_dds.argtypes = [C.c_void_p,C.c_uint64,C.c_int,C.POINTER(C.c_void_p),C.POINTER(C.c_uint64),C.POINTER(C.c_uint32),C.POINTER(C.c_uint32)]
        self.lib.texconv_free.argtypes = [C.c_void_p]
        self.lib.texconv_key.argtypes = [C.c_void_p,C.c_uint64]
        self.lib.texconv_key.restype = C.c_uint64

    def encode(self,blob,family):
        ptr,size,fmt,flags = C.c_void_p(),C.c_uint64(),C.c_uint32(),C.c_uint32()
        result = self.lib.texconv_convert_dds(blob,len(blob),family,C.byref(ptr),C.byref(size),C.byref(fmt),C.byref(flags))
        require(result == 0 and ptr.value,"texture encoder rejected reconstructed DDS")
        try:
            return C.string_at(ptr,size.value),fmt.value
        finally:
            self.lib.texconv_free(ptr)

    def key(self,fourcc,w,h,data):
        meta = (fourcc << 32) ^ (w << 16) ^ h
        return self.lib.texconv_key(data,len(data)) ^ (meta*0x9e3779b185ebca87 & 0xffffffffffffffff)


def file_sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream,"sha256").hexdigest()


def verify(path,codec):
    """Validate every index span, full mip suffix size and source-content key."""
    with path.open("rb") as stream, mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ) as data:
        require(len(data) >= HEADER.size,"truncated UTX3 header")
        header = HEADER.unpack_from(data)
        magic,version,family,count,index_at,payload_bytes = header[:6]
        require(magic == 0x33585455 and version == 2 and family in (1,2),"unsupported UTX3 header")
        require(0 < count <= 2000000 and HEADER.size <= index_at <= len(data),"invalid UTX3 index")
        require(payload_bytes == index_at-HEADER.size and len(data)-index_at == count*ENTRY.size,"UTX3 file size mismatch")
        previous = -1
        for i in range(count):
            key,raw_at,data_at,raw_bytes,data_bytes,fmt,w,h,levels,dxt,reserved = ENTRY.unpack_from(data,index_at+i*ENTRY.size)
            require(key > previous,"UTX3 keys are not strictly sorted")
            require(w and h and w <= 16384 and h <= 16384 and 0 < levels <= 15 and dxt in (1,3,5) and reserved == 0,"invalid UTX3 texture metadata")
            require((family == 2 and fmt == 0x93b0) or (family == 1 and fmt in (0x9274,0x9278)),"UTX3 family/GL format mismatch")
            raw_total,data_total,ww,hh = 0,0,w,h
            for _ in range(levels):
                blocks = ((ww+3)//4)*((hh+3)//4)
                raw_total += blocks*(8 if dxt == 1 else 16)
                data_total += blocks*(8 if fmt == 0x9274 else 16)
                ww,hh = max(1,ww//2),max(1,hh//2)
            require(raw_bytes == raw_total and data_bytes == data_total,"UTX3 mip suffix size mismatch")
            require(HEADER.size <= raw_at <= index_at and raw_bytes <= index_at-raw_at and HEADER.size <= data_at <= index_at and data_bytes <= index_at-data_at,"UTX3 payload outside file")
            top_bytes = ((w+3)//4)*((h+3)//4)*(8 if dxt == 1 else 16)
            fourcc = int.from_bytes(f"DXT{dxt}".encode("ascii"),"little")
            require(codec.key(fourcc,w,h,data[raw_at:raw_at+top_bytes]) == key,"UTX3 original source key mismatch")
            previous = key
    return count


def cook(game,output,codec_path,maps,threads,quality,family):
    game,output = game.resolve(),output.resolve()
    require(not output.is_relative_to(game),"cache output must be outside original input directory")
    content = game/"Content"
    sources = sorted((content/"Maps").glob("*.bsm"))+sorted((content/"System").glob("*.u"))+sorted((game/"Builds/Release").glob("*.U"))
    if maps:
        wanted = {x.casefold() for x in maps}
        sources = [p for p in sources if p.stem.casefold() in wanted or p.name.casefold() in wanted]
        require(len(sources) == len(wanted),"selected map was not found uniquely")
    require(sources,"no BioShock packages found")
    os.environ["VOID_TEXCONV_ASTC_QUALITY"] = str(quality)
    codec,bulk = Codec(codec_path.resolve()),Bulk(content)
    input_hashes = {str(p.relative_to(game)):file_sha(p) for p in sources+bulk.paths}
    output.parent.mkdir(parents=True,exist_ok=True)
    temporary = output.with_suffix(output.suffix+".part")
    stats,seen,index,conflicts = Counter(),set(),{},set()
    start = time.monotonic()
    try:
        # Exclusive creation rejects a simultaneous cook or a prior incomplete
        # output until it has been reviewed. Existing complete packs stay intact.
        with temporary.open("xb") as stream, ThreadPoolExecutor(max_workers=threads) as pool:
            stream.write(bytes(HEADER.size))
            pending = []
            def append(task):
                rec,levels,future = task
                encoded,fmt = future.result()
                raw = b"".join(levels)
                raw_at = stream.tell()
                stream.write(raw)
                data_at = stream.tell()
                stream.write(encoded)
                raw_left,data_left = len(raw),len(encoded)
                fourcc = int.from_bytes(FOURCC[rec["format"]],"little")
                for level,(m,source) in enumerate(zip(rec["mips"],levels)):
                    w,h = m["width"],m["height"]
                    key = codec.key(fourcc,w,h,source)
                    # Different lower chains sharing the same top mip cannot be
                    # distinguished by the runtime key. Skip that key safely.
                    suffix = hashlib.sha256(raw[len(raw)-raw_left:]).digest()
                    entry = (key,raw_at,data_at,raw_left,data_left,fmt,w,h,len(levels)-level,int(chr(fourcc >> 24)),0)
                    previous = index.get(key)
                    if key not in conflicts:
                        if previous is None:
                            index[key] = (entry,suffix)
                        elif previous[1] != suffix:
                            # The same level heads chains with different lower levels: a one-level entry
                            # (the native renderer, android/render10/r10_pack.c, looks levels up one by one),
                            # as the on-device converter android/texconv/bioshock_tex.cpp writes it.
                            one = list(previous[0])
                            pw, ph = one[6], one[7]
                            one[3] = ((pw+3)//4)*((ph+3)//4)*(8 if fourcc == 0x31545844 else 16)
                            one[4] = ((pw+3)//4)*((ph+3)//4)*(8 if one[5] == 0x9274 else 16)
                            one[8] = 1
                            index[key] = (tuple(one),previous[1])
                            conflicts.add(key)
                    n = ((w+3)//4)*((h+3)//4)*(8 if fmt == 0x9274 else 16)
                    raw_at += len(source)
                    raw_left -= len(source)
                    data_at += n
                    data_left -= n
                require(data_left == 0,"encoded mip chain size mismatch")
                stats["unique_chains"] += 1
                stats["raw_bytes"] += len(raw)
                stats["encoded_bytes"] += len(encoded)
            for path in sources:
                p,digest = package(path)
                require(digest == input_hashes[str(path.relative_to(game))],"input package changed before cooking")
                before = stats["textures"]
                for export in p["exports"]:
                    if export["class"] != "Texture":
                        continue
                    rec = texture(p,export)
                    stats["textures"] += 1
                    if rec["format"] not in FOURCC or not rec["mips"]:
                        stats["unsupported"] += 1
                        continue
                    stats["bc_textures"] += 1
                    for levels in bulk.chains(p,rec):
                        blob = dds(rec,levels)
                        key = hashlib.sha256(blob).digest()
                        if key in seen:
                            continue
                        seen.add(key)
                        pending.append((rec,levels,pool.submit(codec.encode,blob,family)))
                        if len(pending) >= threads*2:
                            append(pending.pop(0))
                print(path.name,stats["textures"]-before,"textures",stats["unique_chains"],"unique",round(time.monotonic()-start,1),"seconds",flush=True)
            for task in pending:
                append(task)
            require(index,"no BCn texture cache entries")
            index_at = stream.tell()
            for key in sorted(index):
                stream.write(ENTRY.pack(*index[key][0]))
            stream.seek(0)
            stream.write(HEADER.pack(0x33585455,2,family,len(index),index_at,index_at-HEADER.size,stats["raw_bytes"],0,
                                     stats["unique_chains"],stats["unsupported"],0,0,0,0))
            stream.flush()
            os.fsync(stream.fileno())
        require(verify(temporary,codec) == len(index),"UTX3 verification entry count mismatch")
        for name,digest in input_hashes.items():
            require(file_sha(game/name) == digest,"original input changed during cook: "+name)
        os.replace(temporary,output)
        report = dict(format="UTX3 v2",family=family,astc_quality=quality,seconds=time.monotonic()-start,
            output=output.name,bytes=output.stat().st_size,sha256=file_sha(output),entries=len(index),conflicting_keys=len(conflicts),
            stats=dict(stats),inputs_sha256=input_hashes,codec_sha256=file_sha(codec_path))
        output.with_suffix(output.suffix+".json").write_text(json.dumps(report,indent=2)+"\n")
        print("PASS",json.dumps({k:v for k,v in report.items() if k not in ("inputs_sha256","stats")}),flush=True)
        return report
    except BaseException:
        # Never remove another process's partial output if exclusive creation failed.
        if "stream" in locals() and stream.closed:
            temporary.unlink(missing_ok=True)
        raise
    finally:
        bulk.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-root",type=Path,required=True)
    parser.add_argument("--output",type=Path,default=ROOT/"build/texture-cache/Content/port-textures.tvt")
    parser.add_argument("--codec",type=Path,default=ROOT/"build/texture-codec/texconv.dll")
    parser.add_argument("--map",action="append",default=[])
    parser.add_argument("--threads",type=int,choices=range(1,9),default=4)
    parser.add_argument("--quality",type=float,default=60)
    parser.add_argument("--family",choices=("astc","etc2"),default="astc")
    args = parser.parse_args()
    require(0 <= args.quality <= 100,"ASTC quality must be between 0 and 100")
    cook(args.game_root,args.output,args.codec,args.map,args.threads,args.quality,2 if args.family == "astc" else 1)


if __name__ == "__main__":
    main()
