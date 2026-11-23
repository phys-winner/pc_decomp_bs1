"""Bounded BioShock metadata, external BCn reconstruction and atomic UTX3 cook."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("bioshock_textures",ROOT/"tools/bioshock_textures.py")
t = importlib.util.module_from_spec(spec)
spec.loader.exec_module(t)


def compact(value):
    negative = value < 0
    value = abs(value)
    out = bytearray([(value & 63) | (128 if negative else 0)])
    value >>= 6
    if value:
        out[0] |= 64
    while value:
        item = value & 127
        value >>= 7
        out.append(item | (128 if value else 0))
    return bytes(out)


def string(value):
    return compact(len(value)+1)+(value+"\0").encode("utf-16-le")


def texture_blob(fmt=3,missing=True):
    out = struct.pack("<II",4,3)+compact(1)+bytes(4)+bytes([1,fmt])+compact(0)+bytes(4)
    out += struct.pack("<IIQ",4,1,4096)+compact(2)
    for dim,data in ((8,b"" if missing else b"A"*32),(4,b"B"*8)):
        out += struct.pack("<II",4,2)+bytes(12)+compact(len(data))+data
        out += struct.pack("<II2B",dim,dim,dim.bit_length()-1,dim.bit_length()-1)
    return out


def package_blob(payload,compressed=False):
    names = ["None","Format","Texture","Core","Class","TestTexture"]
    names_data = b"".join(string(n)+bytes(8) for n in names)
    fname = lambda i:compact(i)+bytes(4)
    imports = fname(3)+fname(4)+bytes(4)+fname(2)
    exports = compact(-1)+compact(0)+bytes(8)+fname(5)+bytes(8)+compact(len(payload))+compact(64)+bytes(4)
    header = bytearray(64)
    struct.pack_into("<III",header,0,0x9e2a83c1,0x38008d,0x360001 if compressed else 1)
    struct.pack_into("<6I",header,12,len(names),64+len(payload),1,64+len(payload)+len(names_data)+len(imports),1,64+len(payload)+len(names_data))
    tail = payload+names_data+imports+exports
    if compressed:
        block = zlib.compress(tail)
        return bytes(header)+struct.pack("<3I",1,72,len(block))+block
    return bytes(header)+tail


def fixture(root,compressed=False):
    content = root/"Content"
    (content/"Maps").mkdir(parents=True)
    (content/"BulkContent").mkdir()
    (content/"Maps/fixture.bsm").write_bytes(package_blob(texture_blob(),compressed))
    (content/"BulkContent/test.blk").write_bytes(bytes(32)+b"A"*32)
    catalog = b"\0"+bytes(12)+compact(1)+bytes(8)+string("test.blk")+compact(1)
    catalog += string("TestTexture")+string("Package")+struct.pack("<5I",0,32,32,32,0)
    (content/"BulkContent/Catalog.bdc").write_bytes(catalog)
    return content


class FakeCodec:
    def __init__(self,path):
        pass

    def encode(self,blob,family):
        # Two ASTC mips: four 16-byte blocks and one 16-byte block.
        assert blob[:4] == b"DDS " and blob[128:] == b"A"*32+b"B"*8
        return b"C"*80,0x93b0

    def key(self,fourcc,w,h,data):
        return (w << 32) | zlib.crc32(data)


class Textures(unittest.TestCase):
    def test_signed_compact_and_bounds(self):
        for v in (0,1,63,64,128,0x7fffffff,-1,-1024):
            self.assertEqual(t.Reader(compact(v)).compact(),v)
        for bad in (b"",b"\x40",b"\x7f\xff\xff\xff\xff\x7f"):
            with self.assertRaises(ValueError):
                t.Reader(bad).compact()

    def test_compressed_and_raw_packages_external_padding(self):
        for compressed in (False,True):
            with tempfile.TemporaryDirectory() as d:
                root = Path(d)
                content = fixture(root,compressed)
                p,_ = t.package(content/"Maps/fixture.bsm")
                rec = t.texture(p,p["exports"][0])
                self.assertEqual(rec["bulk_bytes"],4096)
                bulk = t.Bulk(content)
                try:
                    self.assertEqual(list(bulk.chains(p,rec)),[[b"A"*32,b"B"*8]])
                finally:
                    bulk.close()
                broken = bytearray((content/"Maps/fixture.bsm").read_bytes())
                broken[20:24] = bytes(4)
                (content/"Maps/fixture.bsm").write_bytes(broken)
                with self.assertRaises(ValueError):
                    t.package(content/"Maps/fixture.bsm")

    def test_reject_corrupt_mip_and_bulk_ranges(self):
        with tempfile.TemporaryDirectory() as d:
            content = fixture(Path(d))
            p,_ = t.package(content/"Maps/fixture.bsm")
            e = p["exports"][0]
            p["data"] = bytearray(p["data"])
            p["data"][e["offset"]+e["size"]-2] = 7
            with self.assertRaisesRegex(ValueError,"dimensions"):
                t.texture(p,e)
            cat = bytearray((content/"BulkContent/Catalog.bdc").read_bytes())
            struct.pack_into("<I",cat,len(cat)-16,100000)
            (content/"BulkContent/Catalog.bdc").write_bytes(cat)
            with self.assertRaisesRegex(ValueError,"bulk range"):
                t.Bulk(content)

    def test_atomic_pack_all_mips_raw_and_encoder_failure(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            game = base/"input"
            fixture(game,True)
            codec = base/"codec.dll"
            codec.write_bytes(b"fixture")
            output = base/"output/port-textures.tvt"
            with patch.object(t,"Codec",FakeCodec):
                report = t.cook(game,output,codec,[],2,60,2)
            self.assertEqual(report["entries"],2)
            blob = output.read_bytes()
            header = t.HEADER.unpack_from(blob)
            self.assertEqual(header[:4],(0x33585455,2,2,2))
            self.assertEqual(blob[t.HEADER.size:t.HEADER.size+40],b"A"*32+b"B"*8)
            entries = [t.ENTRY.unpack_from(blob,header[4]+i*t.ENTRY.size) for i in range(2)]
            self.assertLess(entries[0][0],entries[1][0])
            self.assertEqual(sorted(e[8] for e in entries),[1,2])
            self.assertEqual(t.verify(output,FakeCodec(codec)),2)
            bad = bytearray(blob)
            struct.pack_into("<Q",bad,header[4]+8,len(blob)+100)
            broken = base/"broken.tvt"
            broken.write_bytes(bad)
            with self.assertRaisesRegex(ValueError,"outside file"):
                t.verify(broken,FakeCodec(codec))
            with patch.object(t,"Codec",FakeCodec),patch.object(FakeCodec,"encode",side_effect=RuntimeError("broken encoder")):
                with self.assertRaisesRegex(RuntimeError,"broken encoder"):
                    t.cook(game,output,codec,[],2,60,2)
            self.assertEqual(output.read_bytes(),blob)
            self.assertFalse(output.with_suffix(".tvt.part").exists())
            partial = output.with_suffix(".tvt.part")
            partial.write_bytes(b"another writer")
            with patch.object(t,"Codec",FakeCodec):
                with self.assertRaises(FileExistsError):
                    t.cook(game,output,codec,[],2,60,2)
            self.assertEqual(partial.read_bytes(),b"another writer")

    def test_original_directory_output_forbidden(self):
        with tempfile.TemporaryDirectory() as d:
            game = Path(d)
            with self.assertRaisesRegex(ValueError,"outside original"):
                t.cook(game,game/"Content/port-textures.tvt",game/"none.dll",[],1,60,2)


if __name__ == "__main__":
    unittest.main()
