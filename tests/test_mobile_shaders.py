"""Check the mobile shader archive and compile its real PC entry points with D3DX33."""
import ast
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import port_mobile_shaders as mobile
from tests.host_environment import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]


def expanded(name, sources, mappings=None):
    return re.sub(r'^\s*#include\s+(?:"([^"]+)"|<([^>]+)>)[^\r\n]*$',
                  lambda match: expanded(match[1] or mappings[match[2]], sources, mappings),
                  sources[name], flags=re.M)


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class MobileShadersTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = Path(json.loads((ROOT / "config/inputs.json").read_text())["source_root"])
        cls.data = mobile.checked(cls.original / "Builds/Release/shaders.spk", mobile.SPK_SHA256)
        cls.references = {name: mobile.checked(ROOT / "reference/mobile-shaders" / name, digest)
                          for name, digest in mobile.REFERENCE_SHA256.items()}
        cls.adapted = mobile.adapt(cls.data, cls.references)
        cls.sources = {name: source.decode("utf-8") for name, _, _, source in mobile.unpack(cls.data)}
        cls.cache_data = mobile.checked(cls.original / "Builds/Release/ShaderCache.pcs", mobile.PCS_SHA256)
        cls.cache_rows = mobile.unpack_cache(cls.cache_data)
        cls.mobile_cache, cls.cache_counts = mobile.filter_cache(cls.cache_data, cls.sources)

    def test_checked_archive_and_unchanged_materials(self):
        original = mobile.unpack(self.data)
        self.assertEqual(mobile.pack(original), self.data)
        changed = []
        for before, after in zip(original, mobile.unpack(self.adapted)):
            self.assertEqual(before[:2], after[:2])
            if before != after:
                changed.append(before[0])
                self.assertEqual(after[2], 0x7FFFFFFE)
        self.assertEqual(changed, ["ImageSpace.hlsl", "ScreenTransfer.hlsl"])
        self.assertEqual(mobile.adapt(self.data, self.references), self.adapted)
        for invalid in (b"", self.data[:-1], self.data + b"x", bytes(8) + self.data[8:]):
            with self.assertRaises(ValueError):
                mobile.unpack(invalid)
        with self.assertRaises(ValueError):
            mobile.replace_once("a a", "a", "b")

    def test_real_hlsl_entry_points(self):
        compiler = ROOT / "build/shader_compiler.exe"
        self.assertTrue(compiler.is_file(), "Build the existing shader compiler first")
        sources = {name: source.decode("utf-8") for name, _, _, source in mobile.unpack(self.adapted)}
        source = expanded("ImageSpace.hlsl", sources)
        original_source = expanded("ImageSpace.hlsl", self.sources)
        cases = [("ToneMapVertexShader", "vs_3_0", ""), ("ToneMapPixelShader", "ps_3_0", "")]
        for count in (1, 4, 8, 16):
            for transfer in (0, 1, 2, 3) + ((4,) if count >= 5 else ()):
                cases.append(("BlurPixelShader", "ps_3_0", f"NUM_SAMPLES={count}\nTRANSFER_TYPE={transfer}\n"))
            cases.append(("BlurVertexShader", "vs_3_0", f"NUM_SAMPLES={count}\n"))
        fog_source = expanded("ScreenTransfer.hlsl", sources) + r'''
half4 MobileFogPixelProbe(float4 screenData : TEXCOORD0) : COLOR0 {
    PixelData pixel = (PixelData)0;
    pixel.screenData = screenData;
    return computeFog(pixel);
}
half4 MobileFogDepthProbe(float4 screenData : TEXCOORD0) : COLOR0 {
    PixelData pixel = (PixelData)0;
    float sceneW = GetSceneW(screenData);
    pixel.screenData = float4(screenData.xy / screenData.w * sceneW, screenData.z, sceneW);
    return computeFog(pixel);
}
void MobileFogVertexProbe(float4 screenData : POSITION, out float4 position : POSITION, out float4 fog : COLOR0) {
    VertexData vertex = (VertexData)0;
    PixelData pixel = GeneratePixelData(screenData, float3(0,0,1), vertex);
    position = screenData;
    fog = computeFog(pixel);
}
'''
        cases += [("MobileFogPixelProbe", "ps_3_0", ""), ("MobileFogVertexProbe", "vs_3_0", ""),
                  ("MobileFogDepthProbe", "ps_3_0", ""), ("MobileFogDepthProbe", "ps_3_0", "D3DDRV10=1\n")]
        with tempfile.TemporaryDirectory(prefix="mobile-shaders-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            requests = folder / "requests"
            requests.mkdir()
            for index, (function, profile, macros) in enumerate(cases):
                request = requests / f"{index:03d}.req"
                definitions = "".join("#define " + macro.replace("=", " ", 1) + "\n" for macro in macros.splitlines())
                text = fog_source if function.startswith("MobileFog") else source
                if function == "ToneMapPixelShader":
                    for literal in ("UseAutoExposure", "ApplyBloom", "SRGBWrite", "UseToneControlMap"):
                        text = text.replace(f"LITERAL(bool, {literal}, 0);", f"LITERAL(bool, {literal}, 1);")
                data = (definitions + text).encode("utf-8")
                header = f"function={function}\nprofile={profile}\nflags=0\ninclude=no\nmacros=0\nsource={len(data)}\n"
                request.write_bytes(header.encode("ascii") + data)
                result = subprocess.run([str(compiler), str(request), str(self.original / "Builds/Release/d3dx9_33.dll")],
                                        capture_output=True, text=True, timeout=60)
                error = folder / f"{index:03d}.err"
                self.assertEqual(result.returncode, 0, f"{function}/{macros}: " + (error.read_text(errors="replace") if error.exists() else result.stderr))
                binary = (folder / f"{index:03d}.bin").read_bytes()
                self.assertIn(struct.unpack_from("<I", binary)[0] >> 16, (0xFFFF, 0xFFFE))
                if function == "BlurPixelShader" and "TRANSFER_TYPE=1\n" in macros:
                    self.assertTrue({"s_buffer", "SampleWeights", "PWLConstants", "PWLThreshold"}
                                    <= mobile.ctab_names(binary), "Bloom must retain its PC bindings")
                if function.startswith("Blur") and "TRANSFER_TYPE=1\n" not in macros:
                    original = (definitions + original_source).encode("utf-8")
                    request = requests / f"original-{index:03d}.req"
                    request.write_bytes(f"function={function}\nprofile={profile}\nflags=0\ninclude=no\nmacros=0\nsource={len(original)}\n".encode("ascii") + original)
                    result = subprocess.run([str(compiler), str(request), str(self.original / "Builds/Release/d3dx9_33.dll")],
                                            capture_output=True, text=True, timeout=60)
                    error = folder / f"original-{index:03d}.err"
                    self.assertEqual(result.returncode, 0, error.read_text(errors="replace") if error.exists() else result.stderr)
                    self.assertEqual(binary, (folder / f"original-{index:03d}.bin").read_bytes(),
                                     f"Non-PWL shader changed: {function}/{macros}")
                if function.startswith("MobileFog"):
                    self.assertNotIn(b"screenDataToCamera\0", binary, "Axial fog must not bind the radial matrix")
                    self.assertIn(b"fogColor\0", binary)
                    self.assertIn(b"fogTransform\0", binary)
                    if function == "MobileFogDepthProbe":
                        self.assertIn(b"s_sceneDepth\0", binary, "The original depth sampler must remain active")
        print(f"PASS mobile shader adaptation: {len(cases)} D3DX33 entry/transfer variants; 18 non-PWL/vertex variants byte-identical to original")

    def test_mobile_math_and_binding_contract(self):
        source = next(row[3].decode("utf-8") for row in mobile.unpack(self.adapted) if row[0] == "ImageSpace.hlsl")
        self.assertIn("pixel.rgb = sqrt(pixel.rgb)", source)
        self.assertIn("max(sample.r, max(sample.g, sample.b))", function(source, "half4 BlurTransfer("))
        self.assertNotIn("ComputePWL", function(source, "half4 BlurPixelShader("))
        self.assertEqual(function(source, "half ComputePWL("), function(self.sources["ImageSpace.hlsl"], "half ComputePWL("))
        for register in ("sampler s_buffer;", "SampleWeights[NUM_SAMPLES]", "SampleOffsets[(NUM_SAMPLES + 1) / 2]", "float4 OffsetUVs[(NUM_SAMPLES + 1) / 2] : TEXCOORD0"):
            self.assertIn(register, source)
        # PC weights and the transfer stage remain on every mobile filter tap, including alpha.
        for index in range(4):
            self.assertIn(f"BlurTransfer(tex2D(s_buffer,Interpolants.OffsetUVs[{index // 2}].{'xy' if index % 2 == 0 else 'wz'})) * SampleWeights[{index}]", source)
        for value in (0, 0.001, 0.1, 0.5, 1):
            gamma = value ** 0.5
            self.assertTrue(0 <= gamma <= 1)
            self.assertAlmostEqual(gamma * gamma, value)

    def test_bloom_thresholds_real_source_taps_and_preserves_alpha_weights(self):
        source = next(row[3].decode("utf-8") for row in mobile.unpack(self.adapted) if row[0] == "ImageSpace.hlsl")
        # Execute the production HLSL scalar operations as float C++; only vector syntax is adapted.
        actual = "\n".join(function(source, signature) for signature in ("half ComputePWL(", "half4 BlurTransfer("))
        actual = re.sub(r"\bsample\.([rgb])\b", r"sample.rgb.\1", actual)
        program = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
static float max(float a,float b){return a>b?a:b;}
typedef float half;
struct half3 {float r,g,b;void operator*=(float scale){r*=scale;g*=scale;b*=scale;}};
struct half4 {half3 rgb;float a;};
struct float4 {float x,y,z,w;};
static float4 PWLConstants={0,0,5,-10};
static half PWLThreshold=2;
#define BT_LuminancePWL 1
#define BT_VarianceShadowGather 2
#define BT_ReplicateR 3
#define TRANSFER_TYPE BT_LuminancePWL
/* ACTUAL */
static half4 gather(const half4 *samples,const half4 *weights,bool perTap){
    half4 sum={{0,0,0},0};
    for(int i=0;i<4;++i){
        half4 value=perTap?BlurTransfer(samples[i]):samples[i];
        sum.rgb.r+=value.rgb.r*weights[i].rgb.r;
        sum.rgb.g+=value.rgb.g*weights[i].rgb.g;
        sum.rgb.b+=value.rgb.b*weights[i].rgb.b;
        sum.a+=value.a*weights[i].a;
    }
    return perTap?sum:BlurTransfer(sum);
}
static void close(float a,float b){assert(_finitef(a)&&fabsf(a-b)<0.000001f);}
int main(){
    const half4 equal[4]={{{.25f,.25f,.25f},.25f},{{.25f,.25f,.25f},.25f},{{.25f,.25f,.25f},.25f},{{.25f,.25f,.25f},.25f}};
    const half4 unequal[4]={{{.1f,.4f,.2f},.4f},{{.2f,.3f,.1f},.3f},{{.3f,.2f,.4f},.2f},{{.4f,.1f,.3f},.1f}};
    // The original engine at bloom config +80=1 uploads the curve (2,0),(2.5,2.5).
    half4 samples[4]={{{4,4,4},.2f},{{0,0,0},.4f},{{0,0,0},.6f},{{0,0,0},.8f}};
    half4 result=gather(samples,equal,true),old=gather(samples,equal,false);
    close(result.rgb.r,2.5f);close(result.rgb.g,2.5f);close(result.rgb.b,2.5f);close(old.rgb.r,0);close(result.a,.5f);close(old.a,.5f);
    result=gather(samples,unequal,true);
    close(result.rgb.r,1);close(result.rgb.g,4);close(result.rgb.b,2);close(result.a,.4f);
    samples[0].rgb={4,0,0};samples[1].rgb={0,4,0};samples[2].rgb={0,0,4};
    result=gather(samples,equal,true);old=gather(samples,equal,false);
    close(result.rgb.r,2.5f);close(result.rgb.g,2.5f);close(result.rgb.b,2.5f);close(old.rgb.r,0);close(old.rgb.g,0);close(old.rgb.b,0);close(result.a,.5f);
    for(auto &sample:samples)sample={{3,3,3},.75f};
    result=gather(samples,unequal,true);old=gather(samples,unequal,false);
    close(result.rgb.r,5);close(result.rgb.g,old.rgb.g);close(result.rgb.b,old.rgb.b);close(result.a,.75f);
    const float dark[]={0,0.00000001f,0.00001f,0.00002f};
    for(float value:dark){half4 input={{value,value*.5f,0},.375f};half4 output=BlurTransfer(input);close(output.rgb.r,0);close(output.rgb.g,0);close(output.rgb.b,0);close(output.a,input.a);}
    // An identity PC curve must preserve even sub-epsilon color and arbitrary alpha.
    PWLConstants={1,0,1,0};
    for(float value:dark){half4 input={{value,value*.5f,0},-.25f};half4 output=BlurTransfer(input);close(output.rgb.r,input.rgb.r);close(output.rgb.g,input.rgb.g);close(output.a,input.a);}
    puts("PASS actual bloom transfer: threshold before gather, per-tap maxRGB, original RGBA weights/alpha, uniform controls, finite black/epsilon");
}
'''
        with tempfile.TemporaryDirectory(prefix="mobile-bloom-math-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            probe, obj, binary = folder / "probe.cpp", folder / "probe.obj", folder / "probe.exe"
            probe.write_text(program.replace("/* ACTUAL */", actual), encoding="utf-8")
            env, linker = environment("x64")
            for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c++17",
                             "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                            [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)], [str(binary)]):
                result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_mobile_cache_preserves_only_certified_records(self):
        self.assertEqual(len(self.cache_rows), 16385)
        self.assertEqual(struct.pack("<I", 13) + b"".join(row[4] for row in self.cache_rows), self.cache_data)
        self.assertEqual(self.cache_counts, {"kept": 15535, "excluded": {
            "ImageSpace": 220, "fogTransform": 630, "invalid_or_unknown": 0}})
        self.assertEqual(len(self.mobile_cache), 26718019)
        self.assertEqual(hashlib.sha256(self.mobile_cache).hexdigest(), mobile.MOBILE_PCS_SHA256)
        originals = {(row[0], row[1]): row for row in self.cache_rows}
        kept = mobile.unpack_cache(self.mobile_cache)
        self.assertEqual(len(kept), 15535)
        for row in kept:
            self.assertEqual(row, originals[row[0], row[1]], "Every retained record must be byte-identical")
            self.assertNotIn("ImageSpace.hlsl", row[0])
            self.assertNotIn("fogTransform", mobile.ctab_names(row[3]))

    def test_mobile_cache_rejects_unknown_or_malformed_entries(self):
        valid = next(row for row in self.cache_rows if not row[0].startswith("ImageSpace.hlsl")
                     and "fogTransform" not in mobile.ctab_names(row[3]))

        def container(name, code):
            text = (name + "\0").encode("utf-16le")
            self.assertLess(len(text) // 2, 64)
            return struct.pack("<I", 13) + bytes([len(text) // 2]) + text + valid[1] + struct.pack("<II", len(code), valid[2]) + code

        # Mutate actual CTAB offsets, including a constant alias: the filter must never use substring guesses.
        original = valid[3]
        base = 12
        info = base + struct.unpack_from("<I", original, base + 16)[0]
        name = base + struct.unpack_from("<I", original, info)[0]
        bad_offset = bytearray(original)
        struct.pack_into("<I", bad_offset, info, 0xFFFFFFFF)
        unknown_alias = bytearray(original)
        unknown_alias[name] = ord("$")
        duplicate_end = (2 + ((struct.unpack_from("<I", original, 4)[0] >> 16) & 32767)) * 4
        duplicate = original[:-4] + original[4:duplicate_end] + original[-4:]
        no_table = struct.pack("<2I", struct.unpack_from("<I", original)[0], 0xFFFF)
        for code in (bytes(bad_offset), bytes(unknown_alias), duplicate, no_table, original[:-4]):
            result, counts = mobile.filter_cache(container(valid[0], code), self.sources)
            self.assertEqual(result, struct.pack("<I", 13))
            self.assertEqual(counts["excluded"]["invalid_or_unknown"], 1)
        _, counts = mobile.filter_cache(container("Unknown.hlslPSMain", original), self.sources)
        self.assertEqual(counts["excluded"]["invalid_or_unknown"], 1)
        one = container(valid[0], original)
        for malformed in (b"", one[:-1], one + one[4:], struct.pack("<I", 13) + b"\x40"):
            with self.assertRaises(ValueError):
                mobile.unpack_cache(malformed)

    def test_apk_stages_the_pinned_mobile_cache(self):
        tree = ast.parse((ROOT / "tools/build_apk.py").read_text(encoding="utf-8"))
        stage = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "stage_mobile_shaders")
        namespace = {"build_mobile_shaders": mobile.build, "build_mobile_shader_cache": mobile.build_cache}
        exec(compile(ast.Module(body=[stage], type_ignores=[]), "build_apk.py", "exec"), namespace)
        with tempfile.TemporaryDirectory(prefix="mobile-cache-assets-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            shaders, cache = namespace["stage_mobile_shaders"](folder)
            self.assertEqual((folder / "mobile-shaders.spk").read_bytes(), self.adapted)
            self.assertEqual((folder / "mobile-shader-cache.pcs").read_bytes(), self.mobile_cache)
            self.assertEqual(cache["mobile_spk_sha256"], shaders["sha256"])
            self.assertEqual(cache["sha256"], mobile.MOBILE_PCS_SHA256)
            self.assertEqual(cache["cache_directory"], "ShaderCache-mobile-v3")
            self.assertEqual(shaders["cache_directory"], cache["cache_directory"])
            self.assertFalse(cache["hardware_read_verified"])
            rejected = folder / "must-not-exist.pcs"
            for constant in ("ENGINE_SHA256", "D3DX_SHA256", "PCS_SHA256", "SPK_SHA256", "MOBILE_SPK_SHA256"):
                with patch.object(mobile, constant, "0" * 64), self.assertRaises(ValueError):
                    mobile.build_cache(rejected, self.adapted)
                self.assertFalse(rejected.exists())
            with self.assertRaises(ValueError):
                mobile.build_cache(rejected, self.adapted[:-1] + b"x")
            self.assertFalse(rejected.exists())
        main = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "main")
        self.assertTrue(any(isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                            and node.func.id == "stage_mobile_shaders" for node in ast.walk(main)))
        self.assertTrue(any(isinstance(node, ast.Dict) and any(
            isinstance(key, ast.Constant) and key.value == "mobile_shader_cache"
            and isinstance(value, ast.Name) and value.id == "mobile_shader_cache"
            for key, value in zip(node.keys, node.values)) for node in ast.walk(main)))

    def test_cache_certificate_matches_real_d3dx_dead_and_live_fog(self):
        after = {name: source.decode("utf-8") for name, _, _, source in mobile.unpack(self.adapted)}
        cases = [("Base.hlsl", "MaterialFactory_NULL.hlsl", "PSMain", "ps_3_0", False),
                 ("Base.hlsl", "MaterialFactory_BitmapMaterial.hlsl", "PSMain", "ps_3_0", False),
                 ("Base.hlsl", "MaterialFactory_FogBlend.hlsl", "PSMain", "ps_3_0", True),
                 ("DepthShadows.hlsl", "MaterialFactory_NULL.hlsl", "ShadowDepthTestVS", "vs_3_0", False),
                 ("ScreenTransfer.hlsl", "MaterialFactory_NULL.hlsl", "CacheFogVertexProbe", "vs_3_0", True)]
        with tempfile.TemporaryDirectory(prefix="mobile-cache-d3dx-", dir=ROOT / "build") as temporary:
            folder = Path(temporary).resolve()
            requests = folder / "requests"
            requests.mkdir()
            for index, (root, material, entry, target, live) in enumerate(cases):
                binaries = []
                for revision, sources in enumerate((self.sources, after)):
                    text = expanded(root, sources, {"MATERIALFACTORY": material, "VERTEXFACTORY": "VertexFactory_NULL.hlsl"})
                    if entry == "CacheFogVertexProbe":
                        text += "\nvoid CacheFogVertexProbe(float4 s:POSITION,out float4 p:POSITION,out float4 f:COLOR0){PixelData d=(PixelData)0;d.screenData=s;p=s;f=computeFog(d);}\n"
                    source = text.encode("utf-8")
                    name = f"{index}-{revision}"
                    request = requests / (name + ".req")
                    request.write_bytes(f"function={entry}\nprofile={target}\nflags=0\ninclude=no\nmacros=0\nsource={len(source)}\n".encode() + source)
                    result = subprocess.run([str(ROOT / "build/shader_compiler.exe"), str(request),
                                             str(self.original / "Builds/Release/d3dx9_33.dll")],
                                            capture_output=True, text=True, timeout=45)
                    error = folder / (name + ".err")
                    self.assertEqual(result.returncode, 0, error.read_text(errors="replace") if error.exists() else result.stderr)
                    code = (folder / (name + ".bin")).read_bytes()
                    self.assertEqual("fogTransform" in mobile.ctab_names(code), live)
                    binaries.append(code)
                self.assertEqual(binaries[0] == binaries[1], not live, (root, material, entry))
                if entry == "ShadowDepthTestVS":
                    def instructions(code):
                        words = struct.unpack(f"<{len(code) // 4}I", code)
                        tokens, position = [words[0]], 1
                        while position < len(words):
                            token = words[position]
                            if token & 65535 == 0xFFFE:
                                position += 1 + ((token >> 16) & 32767)
                            else:
                                count = 1 + ((token >> 24) & 15)
                                tokens.extend(words[position:position + count])
                                position += count
                        return tokens

                    # EffectCompiler stored extra CTAB metadata. Its actual cached instructions agree.
                    self.assertTrue(any(row[0] == "DepthShadows.hlslShadowDepthTestVS"
                                        and instructions(row[3]) == instructions(binaries[0])
                                        and mobile.ctab_names(row[3]) == mobile.ctab_names(binaries[0])
                                        for row in self.cache_rows))

    def test_original_effect_compiler_cannot_alias_fog_with_script_fields(self):
        # The engine aborts literal specialization when a parent/member lookup returns NULL.
        # ScriptSide's dynamic parents only request these two fixed member names.
        program = r'''
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef HRESULT (WINAPI *create_t)(const char*,UINT,const void*,void*,DWORD,void**,void**);
typedef void* (WINAPI *name_t)(void*,void*,const char*);
typedef HRESULT (WINAPI *literal_t)(void*,void*,BOOL);
typedef HRESULT (WINAPI *value_t)(void*,void*,const void*,UINT);
typedef ULONG (WINAPI *release_t)(void*);
int main(int argc,char**argv){
    assert(argc==2);
    HMODULE module=LoadLibraryA(argv[1]);assert(module);
    create_t create=(create_t)GetProcAddress(module,"D3DXCreateEffectCompiler");assert(create);
    const char *source="float3 fogTransform; float4 Main(float4 x:TEXCOORD0):COLOR0{return float4(x.xyz*fogTransform,1);}";
    void *compiler=NULL,*errors=NULL;
    HRESULT hr=create(source,(UINT)strlen(source),NULL,NULL,0,&compiler,&errors);
    assert(SUCCEEDED(hr)&&compiler);
    void **vtable=*(void***)compiler;
    name_t parameter=(name_t)vtable[9];
    void *fog=parameter(compiler,NULL,"fogTransform");assert(fog);
    assert(parameter(compiler,fog,"samplerSlot")==NULL);
    assert(parameter(compiler,fog,"channelID")==NULL);
    const float values[3]={-0.01f,0.4f,0.9f};
    assert(SUCCEEDED(((value_t)vtable[20])(compiler,fog,values,sizeof values)));
    assert(SUCCEEDED(((literal_t)vtable[57])(compiler,fog,TRUE)));
    assert(parameter(compiler,fog,"samplerSlot")==NULL);
    assert(parameter(compiler,fog,"channelID")==NULL);
    ((release_t)vtable[2])(compiler);
    if(errors)((release_t)(*(void***)errors)[2])(errors);
    FreeLibrary(module);
    puts("PASS original D3DX33: ScriptSide child names cannot specialize builtin float3 fogTransform");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="mobile-cache-literal-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            source, obj, binary = folder / "probe.c", folder / "probe.obj", folder / "probe.exe"
            source.write_text(program, encoding="utf-8")
            env, linker = environment("x86")
            for command in ([str(CLANG), "/nologo", "/O2", "/W3", "/std:c11",
                             "/clang:--target=i686-pc-windows-msvc", "/c", str(source), "/Fo" + str(obj)],
                            [str(linker), "/nologo", str(obj), "kernel32.lib", "/MACHINE:X86", "/OUT:" + str(binary)],
                            [str(binary), str(self.original / "Builds/Release/d3dx9_33.dll")]):
                result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_mobile_fog_equation_and_original_binding_contract(self):
        original = {name: source.decode("utf-8") for name, _, _, source in mobile.unpack(self.data)}
        adapted = {name: source.decode("utf-8") for name, _, _, source in mobile.unpack(self.adapted)}
        self.assertEqual(adapted["ScreenTransfer.hlsl"], original["ScreenTransfer.hlsl"].replace(
            "static const bool UseRadialFog = 1;", "static const bool UseRadialFog = 0;"))
        reference = self.references["FogApplyPixelShader.msf"].decode("utf-8")
        self.assertIn("highp float distance = screenData.w;", reference)
        self.assertIn("distance = pixel.screenData.w;", adapted["ScreenTransfer.hlsl"])

        def equation(source, signature):
            body = function(source, signature)
            body = body[body.index("float expPower"):]
            body = re.sub(r"//[^\n]*", "", body)
            body = re.sub(r"\b(?:highp|mediump|lowp)\b", "", body)
            body = re.sub(r"\b(?:half4|vec4)\b", "float4", body)
            body = re.sub(r"\bhalf\b", "float", body)
            body = re.sub(r"\s+", "", body)
            return body.replace("clamp(exp(expPower),0.0,1.0)", "saturate(exp(expPower))")

        self.assertEqual(equation(reference, "highp vec4 computeFog("),
                         equation(adapted["ScreenTransfer.hlsl"], "half4 computeFog("))
        for signature in ("float GetSceneW(", "float GetSceneWFromUV(", "half finalTransfer_alpha("):
            self.assertEqual(function(original["ScreenTransfer.hlsl"], signature),
                             function(adapted["ScreenTransfer.hlsl"], signature))
        self.assertEqual(original["MaterialFactory_FogApply.hlsl"], adapted["MaterialFactory_FogApply.hlsl"])
        self.assertEqual(original["MaterialFactory_FogBlend.hlsl"], adapted["MaterialFactory_FogBlend.hlsl"])

    def test_mobile_cache_startup_revision(self):
        source = (ROOT / "android/host/android_main.c").read_text(encoding="utf-8")
        start = source.index("    {\n        /* GLES program binaries")
        block = source[start:source.index("    }\n    if (turnip)", start) + len("    }")]
        program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static const char *enabled;
static char data_dir[]="/profile",selected_cache[1100];
static unsigned cache_sets,directory_calls;
static int directory_error,mixed_case;
static const char *fixture_getenv(const char *name){assert(!strcmp(name,"BIOSHOCK_MOBILE_SHADERS"));return enabled;}
int void_files_cache_directory(const char *profile,const char *name,char *path,size_t cap){
    assert(!strcmp(profile,"/profile"));++directory_calls;
    assert(!strcmp(name,"ShaderCache-mobile-v3")||!strcmp(name,"ShaderCache"));
    snprintf(path,cap,"%s/%s",profile,mixed_case?"sHaDeRcAcHe-MoBiLe-v3":name);
    return !directory_error||directory_error==EEXIST;
}
static void void_files_set_cache(const char *path){++cache_sets;snprintf(selected_cache,sizeof selected_cache,"%s",path);}
#define getenv fixture_getenv
static void configure(void){
/* ACTUAL */
}
int main(void){
    const char *off[]={NULL,"0","true","2"};
    for(unsigned i=0;i<4;++i){enabled=off[i];configure();assert(!strcmp(selected_cache,"/profile/ShaderCache"));}
    enabled="1";configure();assert(cache_sets==5&&!strcmp(selected_cache,"/profile/ShaderCache-mobile-v3"));
    directory_error=EEXIST;mixed_case=1;configure();assert(cache_sets==6&&!strcmp(selected_cache,"/profile/sHaDeRcAcHe-MoBiLe-v3"));
    directory_error=EACCES;configure();assert(cache_sets==6&&directory_calls==7);
    puts("PASS production cache startup: explicit profile resolver runs before mount initialization, normal/mobile v3 selection, actual mixed-case directory retained, failed resolution does not enable cache");
}
'''
        with tempfile.TemporaryDirectory(prefix="mobile-cache-startup-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            probe = folder / "probe.c"
            probe.write_text(program.replace("/* ACTUAL */", block), encoding="utf-8")
            env, linker = environment("x64")
            obj, binary = folder / "probe.obj", folder / "probe.exe"
            for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/WX", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                             "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                            [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)], [str(binary)]):
                result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_overlay_and_cache_resolution(self):
        source = (ROOT / "android/host/files.c").read_text(encoding="utf-8")
        actual = "\n".join(function(source, signature) for signature in (
            "static int mobile_shader_cache(", "static int resolve_below(",
            "int void_files_cache_directory(", "static char *to_host_ex("))
        program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define DT_DIR 4
#define DT_REG 8
#define DT_UNKNOWN 0
#define RESOLVE_OVERLAY 1
#define RESOLVE_CACHED_ONLY 2
#define S_ISDIR(mode) (((mode)&_S_IFMT)==_S_IFDIR)
typedef struct { const char *win; char host[1024]; } mount_t;
static mount_t mounts[]={{"C:\\Games\\BioShock","/game"}};
static int mount_count=1;
static char game_root[1024]="/game", data_root[1024]="/profile", overlay_root[1024]="/mobile";
static const char *enabled="1";
static char *fixture_getenv(const char *name) { assert(!strcmp(name,"BIOSHOCK_MOBILE_SHADERS")); return (char *)enabled; }
#define getenv fixture_getenv
static int normalize(const char *path,char *out,size_t cap) { if(!path)return 0; snprintf(out,cap,"%s",path); return 1; }
static int cacheable(const char *path) { return strncmp(path,"/profile",8)!=0; }
static void void_log(const char *text,...) { (void)text; }
/* Case-sensitive filesystem primitive boundaries, independent of cache policy.
 * The production component walk and both callers above execute unchanged. */
typedef struct {char path[512];unsigned char kind;const char *bytes;} node;
static node nodes[24]={
    {"/profile/sHaDeRcAcHe-MoBiLe-v3",DT_DIR,NULL},
    {"/profile/sHaDeRcAcHe-MoBiLe-v3/sHaDeRcAcHe.PcS",DT_REG,"warm-progress"},
    {"/profile/sHaDeRcAcHe",DT_DIR,NULL},
    {"/profile/NotADirectory",DT_REG,"keep"},
    {"/game/Builds",DT_DIR,NULL},{"/game/Builds/Release",DT_DIR,NULL},
    {"/mobile/Builds",DT_DIR,NULL},{"/mobile/Builds/Release",DT_DIR,NULL},
    {"/mobile/Builds/Release/shaders.spk",DT_REG,"mobile-shaders"},
    {"/profile/ShaderCache-mobile-v2",DT_DIR,NULL},
    {"/profile/ShaderCache-mobile-v2/ShaderCache.pcs",DT_REG,"old-v2-progress"}
};
static unsigned node_count=11;
static int node_index(const char *path){for(unsigned i=0;i<node_count;++i)if(!strcmp(path,nodes[i].path))return (int)i;return -1;}
static int fixture_stat(const char *path,struct stat *st) {
    int index=node_index(path);if(index<0){errno=ENOENT;return -1;}
    memset(st,0,sizeof *st);st->st_mode=nodes[index].kind==DT_DIR?_S_IFDIR:_S_IFREG;return 0;
}
static int fixture_mkdir(const char *path,unsigned mode){
    assert(mode==0775);
    if(node_index(path)>=0){errno=EEXIST;return -1;}
    assert(node_count<24);snprintf(nodes[node_count].path,sizeof nodes[node_count].path,"%s",path);
    nodes[node_count++].kind=DT_DIR;return 0;
}
#define stat(path,st) fixture_stat(path,st)
#define mkdir(path,mode) fixture_mkdir(path,mode)
static int resolve_component(char *base,size_t cap,const char *name,unsigned char *kind,int cached){
    if(cached&&!cacheable(base))return -1;
    size_t length=strlen(base);
    for(unsigned i=0;i<node_count;++i){
        if(strncmp(nodes[i].path,base,length)||nodes[i].path[length]!='/')continue;
        const char *tail=nodes[i].path+length+1;
        if(strchr(tail,'/')||strcasecmp(tail,name))continue;
        snprintf(base+length,cap-length,"/%s",tail);if(kind)*kind=nodes[i].kind;return 1;
    }
    snprintf(base+length,cap-length,"/%s",name);if(kind)*kind=DT_UNKNOWN;return 0;
}
/* ACTUAL */
int main(void) {
    int parent=0,leaf=0;char *path;char directory[1100];
    unsigned before=node_count;
    assert(void_files_cache_directory("/profile","ShaderCache-mobile-v3",directory,sizeof directory));
    assert(!strcmp(directory,"/profile/sHaDeRcAcHe-MoBiLe-v3")&&node_count==before);
    assert(void_files_cache_directory("/profile","ShaderCache",directory,sizeof directory));
    assert(!strcmp(directory,"/profile/sHaDeRcAcHe")&&node_count==before);
    assert(!void_files_cache_directory("/profile","NotADirectory",directory,sizeof directory));
    assert(!strcmp(nodes[3].bytes,"keep"));
    path=to_host_ex("C:\\Games\\BioShock\\Builds\\Release\\shaders.spk",RESOLVE_OVERLAY,&parent,&leaf,NULL);
    assert(path && !strcmp(path,"/mobile/Builds/Release/shaders.spk") && parent && leaf);free(path);
    const char *names[]={"ShaderCache.pcs","shadercache.PCS","ShaderCache.pcs10"};
    for(unsigned i=0;i<3;++i) {
        char win[256];snprintf(win,sizeof win,"C:\\Games\\BioShock\\Builds\\Release\\%s",names[i]);
        path=to_host_ex(win,0,&parent,&leaf,NULL);
        assert(path && !strcmp(path,i==2?"/profile/sHaDeRcAcHe-MoBiLe-v3/ShaderCache.pcs10":"/profile/sHaDeRcAcHe-MoBiLe-v3/sHaDeRcAcHe.PcS") && parent && leaf==(i!=2));free(path);
    }
    path=to_host_ex("C:\\Games\\BioShock\\Builds\\Release\\ShaderCache.pcs",RESOLVE_OVERLAY,&parent,&leaf,NULL);
    assert(path && leaf&&!strcmp(nodes[node_index(path)].bytes,"warm-progress")&&node_count==before);free(path);
    assert(!strcmp(nodes[10].path,"/profile/ShaderCache-mobile-v2/ShaderCache.pcs")&&!strcmp(nodes[10].bytes,"old-v2-progress"));
    assert(!to_host_ex("C:\\Games\\BioShock\\Builds\\Release\\ShaderCache.pcs",RESOLVE_CACHED_ONLY,NULL,NULL,NULL));
    assert(!mobile_shader_cache("C:\\Games\\BioShock\\Builds\\Release\\Other.pcs"));
    enabled="0";
    path=to_host_ex("C:\\Games\\BioShock\\Builds\\Release\\ShaderCache.pcs",RESOLVE_OVERLAY,&parent,&leaf,NULL);
    assert(path && !strcmp(path,"/game/Builds/Release/ShaderCache.pcs"));free(path);
    assert(!to_host_ex(NULL,0,NULL,NULL,NULL));
    assert(void_files_cache_directory("/fresh","ShaderCache-mobile-v3",directory,sizeof directory));
    assert(!strcmp(directory,"/fresh/ShaderCache-mobile-v3")&&node_count==before+1);
    snprintf(data_root,sizeof data_root,"/fresh");enabled="1";
    path=to_host_ex("C:\\Games\\BioShock\\Builds\\Release\\ShaderCache.pcs",0,&parent,&leaf,NULL);
    assert(path&&!strcmp(path,"/fresh/ShaderCache-mobile-v3/ShaderCache.pcs")&&parent&&!leaf);free(path);
    puts("PASS production mobile SPK override, real component walk for mixed-case warm PCS and program directory, no duplicate/renamed/deleted progress, fresh path and disabled original path");
}
'''
        with tempfile.TemporaryDirectory(prefix="mobile-shader-files-", dir=ROOT / "build") as temporary:
            folder = Path(temporary)
            probe = folder / "probe.c"
            probe.write_text(program.replace("/* ACTUAL */", actual), encoding="utf-8")
            env, linker = environment("x64")
            obj, binary = folder / "probe.obj", folder / "probe.exe"
            for command in ([str(CLANG), "/nologo", "/O2", "/W4", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS", "/clang:--target=x86_64-pc-windows-msvc", "/c", str(probe), "/Fo" + str(obj)],
                            [str(linker), "/nologo", str(obj), "/OUT:" + str(binary)], [str(binary)]):
                result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
