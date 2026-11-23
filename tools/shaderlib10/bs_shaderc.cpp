/* Build-time translation of BioShock's Direct3D 10 shaders for the native renderer (android/render10).
 *
 *     bs_shaderc INPUT OUTPUT.jsonl
 *
 * INPUT is written by tools/build_shaderlib10.py: records of {u32 kind, u32 size, DXBC[size]} with the
 * size padded to 4 bytes.  Every DXBC container goes through DXVK 2.4.1's DXBC compiler (src/dxbc,
 * src/spirv; zlib licence), unchanged, with the options of DXVK's D3D11 device on Android.  The compiler
 * emits SPIR-V 1.6 with the Vulkan memory model and demote-to-helper, which Vulkan 1.1 drivers do not
 * accept, so SPIRV-Cross (Apache 2.0) turns that module into two texts:
 *
 *   vk  GLSL 4.50 for Vulkan, DXVK's binding numbers in set 0; build_shaderlib10.py compiles it with
 *       glslc of the NDK for Vulkan 1.1 (SPIR-V 1.3);
 *   es  GLSL ES 3.20: separate textures and samplers combined (the runtime binds the texture of slot
 *       t and the sampler state of slot s to the unit of each pair), uniform blocks and units with
 *       explicit bindings, clip space converted for OpenGL (Y flipped, depth -w..w).
 *
 * One JSON object per shader is written to OUTPUT; the Python side packs the library. */
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "dxbc/dxbc_module.h"
#include "dxbc/dxbc_util.h"
#include "spirv_glsl.hpp"

namespace rdvk {
static unsigned g_warnings = 0;
void log(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    ++g_warnings;
}
}  // namespace rdvk

namespace {

/* Binding layout of DXVK's compiler (dxbc_util.h): per stage 16 constant buffers, 16 samplers and
 * 128 shader resources; stage offset = 160 x DxbcProgramType (pixel 0, vertex 1, geometry 2). */
constexpr uint32_t kStageBindings = 160, kSamplerBase = 16, kResourceBase = 32;
/* OpenGL ES binding points of the es text: uniform blocks VS 0..15, PS 16..31; texture units VS 16..31,
 * PS 0..15 (GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS >= 96 in OpenGL ES 3.2). */
constexpr uint32_t kEsUboVs = 0, kEsUboPs = 16, kEsUnitVs = 16, kEsUnitPs = 0;

uint32_t u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

uint32_t program_type(const uint8_t* b, size_t size) {
    uint32_t chunks = size >= 32 ? u32(b + 28) : 0;
    for (uint32_t c = 0; c < chunks && 32 + 4 * c + 4 <= size; ++c) {
        uint32_t at = u32(b + 32 + 4 * c);
        if (at + 12 > size) continue;
        uint32_t fourcc = u32(b + at);
        if (fourcc == 0x58454853u || fourcc == 0x52444853u) return u32(b + at + 8) >> 16;   /* SHEX / SHDR */
    }
    return ~0u;
}

uint64_t fnv1a64(const uint8_t* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}

dxvk::DxbcOptions compiler_options() {
    dxvk::DxbcOptions o;
    o.useDepthClipWorkaround = false;
    o.supportsTypedUavLoadR32 = true;
    o.supportsRawAccessChains = false;
    o.zeroInitWorkgroupMemory = true;
    o.invariantPosition = true;
    o.forceVolatileTgsmAccess = false;
    o.disableMsaa = false;
    o.forceSampleRateShading = false;
    o.enableSampleShadingInterlock = false;
    o.minSsboAlignment = ~0u;           /* raw and structured buffers stay texel buffers */
    return o;
}

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
            else out += char(c);
        }
    }
    return out + "\"";
}

std::string hex(const uint8_t* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) { out += digits[data[i] >> 4]; out += digits[data[i] & 15]; }
    return out;
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size()) text.replace(at, from.size(), to);
}

/* Lines SPIRV-Cross adds for SPIR-V 1.6 features that Vulkan 1.1 and OpenGL ES drivers lack. */
std::string common_fixups(std::string text) {
    std::istringstream in(text);
    std::string line, out;
    while (std::getline(in, line)) {
        if (line.find("#extension GL_EXT_demote_to_helper_invocation") != std::string::npos) continue;
        out += line;
        out += '\n';
    }
    replace_all(out, "demote;", "discard;");
    replace_all(out, "helperInvocationEXT()", "gl_HelperInvocation");
    return out;
}

struct Combined { uint32_t unit; int texture; int sampler; std::string name; };

struct Result {
    std::string vk, es, error;
    std::vector<Combined> combined;
    std::map<uint32_t, uint32_t> ubos;      /* constant buffer slot -> GL binding */
    std::map<uint32_t, std::string> qualifiers;   /* PS input location -> interpolation qualifiers */
    bool push_constants = false;
    bool clip_distance = false;
};

Result cross(const std::vector<uint32_t>& spirv, uint32_t type) {
    Result r;
    bool pixel = type == 0;
    uint32_t stage_offset = kStageBindings * type;
    {
        spirv_cross::CompilerGLSL glsl(spirv);
        spirv_cross::CompilerGLSL::Options opt;
        opt.version = 450;
        opt.es = false;
        opt.vulkan_semantics = true;
        opt.emit_line_directives = false;
        glsl.set_common_options(opt);
        r.vk = common_fixups(glsl.compile());
        /* DXVK writes SV_Position through an output register copied to the builtin, so the builtin
         * itself is not invariant: a depth pre-pass and the EQUAL-tested passes after it then get
         * different depths from different vertex shaders.  Direct3D computes them identically. */
        if (!pixel && r.vk.find("invariant gl_Position;") == std::string::npos) {
            size_t at = 0, next;
            while ((next = r.vk.find('\n', at)) != std::string::npos &&
                   (r.vk.compare(at, 8, "#version") == 0 || r.vk.compare(at, 10, "#extension") == 0))
                at = next + 1;
            r.vk.insert(at, "invariant gl_Position;\n");
        }
    }
    spirv_cross::CompilerGLSL es(spirv);
    spirv_cross::CompilerGLSL::Options opt;
    opt.version = 320;
    opt.es = true;
    opt.vulkan_semantics = false;
    opt.enable_420pack_extension = false;
    opt.emit_line_directives = false;
    opt.vertex.flip_vert_y = !pixel;
    opt.vertex.fixup_clipspace = !pixel;
    opt.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
    opt.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
    es.set_common_options(opt);
    uint32_t dummy = es.build_dummy_sampler_for_combined_images();
    es.build_combined_image_samplers();
    uint32_t unit = pixel ? kEsUnitPs : kEsUnitVs;
    for (const spirv_cross::CombinedImageSampler& c : es.get_combined_image_samplers()) {
        int texture = int(es.get_decoration(c.image_id, spv::DecorationBinding)) - int(stage_offset + kResourceBase);
        int sampler = c.sampler_id == dummy ? -1
            : int(es.get_decoration(c.sampler_id, spv::DecorationBinding)) - int(stage_offset + kSamplerBase);
        /* Uniforms of one name are one uniform across the stages of a GLSL program: the stages' own
         * t0/s0 differ, so the name carries the stage. */
        char name[48];
        snprintf(name, sizeof name, pixel ? "bs_ps_t%d_s%d" : "bs_vs_t%d_s%d", texture, sampler);
        es.set_name(c.combined_id, name);
        es.set_decoration(c.combined_id, spv::DecorationBinding, unit);
        es.unset_decoration(c.combined_id, spv::DecorationDescriptorSet);
        r.combined.push_back(Combined{unit, texture, sampler, name});
        ++unit;
    }
    spirv_cross::ShaderResources res = es.get_shader_resources();
    for (const spirv_cross::Resource& ubo : res.uniform_buffers) {
        uint32_t slot = es.get_decoration(ubo.id, spv::DecorationBinding) - stage_offset;
        uint32_t binding = (pixel ? kEsUboPs : kEsUboVs) + slot;
        es.set_decoration(ubo.id, spv::DecorationBinding, binding);
        es.unset_decoration(ubo.id, spv::DecorationDescriptorSet);
        /* Blocks of one name are one interface across the stages of a GLSL program; the stages' own
         * cb0 differ, so the name carries the stage. */
        char name[32];
        snprintf(name, sizeof name, pixel ? "bs_ps_cb%u" : "bs_vs_cb%u", slot);
        es.set_name(ubo.base_type_id, name);
        r.ubos[slot] = binding;
    }
    r.push_constants = !res.push_constant_buffers.empty();
    for (const spirv_cross::Resource& pc : res.push_constant_buffers) es.set_name(pc.id, "bs_push");
    std::string text = common_fixups(es.compile());
    r.clip_distance = text.find("gl_ClipDistance") != std::string::npos;
    /* Interpolation qualifiers: OpenGL ES requires those of a vertex output and the fragment input it
     * feeds to match, and has no noperspective.  The pixel shader's qualifiers are recorded per
     * location; the vertex shader's outputs get a placeholder that the runtime fills for the pixel
     * shader of each program. */
    std::istringstream in(text);
    std::string line, out;
    static const std::regex decl("^layout\\(location = (\\d+)\\) ((?:(?:flat|centroid|noperspective|sample|smooth) )*)(in|out) (.*)$");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_match(line, m, decl)) {
            std::string quals = m[2].str();
            replace_all(quals, "noperspective ", "");
            replace_all(quals, "smooth ", "");
            uint32_t location = uint32_t(std::stoul(m[1].str()));
            if (pixel && m[3].str() == "in") {
                r.qualifiers[location] = quals;
                line = "layout(location = " + m[1].str() + ") " + quals + "in " + m[4].str();
            } else if (!pixel && m[3].str() == "out") {
                /* Integer outputs must be flat on their own; the placeholder then stays empty. */
                line = "layout(location = " + m[1].str() + ") " + quals + "/*@Q" + m[1].str() + "@*/out " + m[4].str();
            }
        }
        out += line;
        out += '\n';
    }
    r.es = out;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: bs_shaderc INPUT OUTPUT.jsonl\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> input;
    uint8_t chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;) input.insert(input.end(), chunk, chunk + n);
    fclose(f);
    FILE* o = fopen(argv[2], "wb");
    if (!o) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    unsigned done = 0, failed = 0, stages[3] = {};
    for (size_t at = 0; at + 8 <= input.size();) {
        uint32_t kind = u32(&input[at]);
        uint32_t size = u32(&input[at + 4]);
        const uint8_t* b = &input[at + 8];
        at += 8 + ((size + 3) & ~3u);
        if (at > input.size() + 3 || size < 32 || memcmp(b, "DXBC", 4)) { fprintf(stderr, "bad input record\n"); return 1; }
        uint32_t type = program_type(b, size);
        if (type > 2) { fprintf(stderr, "DXBC with program type %u\n", type); return 1; }
        uint64_t hash = fnv1a64(b, size);
        char name[48];
        snprintf(name, sizeof(name), "%s_%016llx", type == 0 ? "PS" : type == 1 ? "VS" : "GS", (unsigned long long)hash);
        std::string record = "{\"hash\": \"" + std::string(name + 3) + "\", \"size\": " + std::to_string(size) +
            ", \"kind\": " + std::to_string(kind) + ", \"type\": " + std::to_string(type) +
            ", \"checksum\": \"" + hex(b + 4, 16) + "\"";
        try {
            dxvk::DxbcReader reader(reinterpret_cast<const char*>(b), size);
            dxvk::DxbcModule module(reader);
            dxvk::DxbcModuleInfo info{};
            info.options = compiler_options();
            dxvk::DxbcTessInfo tess{64.0f};
            info.tess = &tess;
            info.xfb = nullptr;
            dxvk::Rc<dxvk::DxvkShader> compiled = module.compile(info, name);
            const dxvk::DxvkShaderCreateInfo& ci = compiled->info();
            dxvk::SpirvCodeBuffer code = compiled->code();
            std::vector<uint32_t> spirv(code.data(), code.data() + code.dwords());
            Result r = cross(spirv, type);
            record += ", \"input_mask\": " + std::to_string(ci.inputMask) + ", \"output_mask\": " + std::to_string(ci.outputMask);
            record += ", \"push_constants\": " + std::string(r.push_constants ? "true" : "false");
            record += ", \"clip_distance\": " + std::string(r.clip_distance ? "true" : "false");
            record += ", \"icb\": \"" + hex(reinterpret_cast<const uint8_t*>(ci.uniformData), ci.uniformSize) + "\"";
            record += ", \"bindings\": [";
            bool first = true;
            for (const dxvk::DxvkBindingInfo& bi : compiled->bindings()) {
                record += std::string(first ? "" : ", ") + "[" + std::to_string(uint32_t(bi.descriptorType)) + ", " +
                    std::to_string(bi.resourceBinding) + ", " + std::to_string(uint32_t(bi.viewType)) + "]";
                first = false;
            }
            record += "], \"combined\": [";
            first = true;
            for (const Combined& c : r.combined) {
                record += std::string(first ? "" : ", ") + "[" + std::to_string(c.unit) + ", " + std::to_string(c.texture) +
                    ", " + std::to_string(c.sampler) + "]";
                first = false;
            }
            record += "], \"ubos\": [";
            first = true;
            for (auto& u : r.ubos) {
                record += std::string(first ? "" : ", ") + "[" + std::to_string(u.first) + ", " + std::to_string(u.second) + "]";
                first = false;
            }
            record += "], \"qualifiers\": {";
            first = true;
            for (auto& q : r.qualifiers) {
                record += std::string(first ? "" : ", ") + "\"" + std::to_string(q.first) + "\": " + json_string(q.second);
                first = false;
            }
            record += "}, \"vk\": " + json_string(r.vk) + ", \"es\": " + json_string(r.es) + "}\n";
            ++stages[type];
            ++done;
        } catch (const dxvk::DxvkError& error) {
            record += ", \"error\": " + json_string(error.message()) + "}\n";
            ++failed;
        } catch (const spirv_cross::CompilerError& error) {
            record += ", \"error\": " + json_string(std::string("SPIRV-Cross: ") + error.what()) + "}\n";
            ++failed;
        }
        fwrite(record.data(), 1, record.size(), o);
    }
    if (fclose(o)) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    printf("{\"shaders\": %u, \"failed\": %u, \"compiler_messages\": %u, \"PS\": %u, \"VS\": %u, \"GS\": %u}\n",
           done, failed, rdvk::g_warnings, stages[0], stages[1], stages[2]);
    return 0;
}
