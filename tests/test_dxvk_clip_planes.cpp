/* Generate and validate the production clip fallback with DXVK's SPIR-V builder. */
#include <cassert>
#include <fstream>
#include <string>
#include "spirv/spirv_module.h"
#include "spirv/spirv_module.cpp"
#include "spirv/spirv_code_buffer.cpp"

namespace dxvk {
namespace caps { constexpr uint32_t MaxClipPlanes = 6; }
enum class DxsoUsage : uint32_t { Position = 0 };
struct DxsoSemantic {
  DxsoUsage usage; uint32_t usageIndex;
  bool operator==(const DxsoSemantic& other) const { return usage == other.usage && usageIndex == other.usageIndex; }
};
static mutex g_linkerSlotMutex;
static uint32_t g_linkerSlotCount;
static std::array<DxsoSemantic, 32> g_linkerSlots;
#include "clip_linker.inc"
#include "clip_planes.inc"
}

static void generate(const std::string& path, bool vertex) {
  using namespace dxvk;
  SpirvModule m(spvVersion(1, 3));
  m.enableCapability(spv::CapabilityShader);
  m.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  uint32_t main = m.allocateId(), type = m.defFloatType(32), vec4 = m.defVectorType(type, 4);
  uint32_t out = m.newVar(m.defPointerType(vec4, spv::StorageClassOutput), spv::StorageClassOutput);
  if (vertex) m.decorateBuiltIn(out, spv::BuiltInPosition);
  else {
    m.decorateLocation(out, 0);
    m.setExecutionMode(main, spv::ExecutionModeOriginUpperLeft);
    m.enableExtension("SPV_EXT_demote_to_helper_invocation");
    m.enableCapability(spv::CapabilityDemoteToHelperInvocationEXT);
  }
  m.functionBegin(m.defVoidType(), main, m.defFunctionType(m.defVoidType(), 0, nullptr), spv::FunctionControlMaskNone);
  m.opLabel(m.allocateId());
  uint32_t mask = 0;
  if (vertex) {
    uint32_t count = m.constu32(caps::MaxClipPlanes);
    uint32_t clip = m.newVar(m.defPointerType(m.defArrayType(type, count), spv::StorageClassOutput), spv::StorageClassOutput);
    m.decorateLocation(clip, RegisterClipDistanceSlots());
    for (uint32_t i = 0; i < caps::MaxClipPlanes; ++i) {
      uint32_t index = m.constu32(i);
      m.opStore(m.opAccessChain(m.defPointerType(type, spv::StorageClassOutput), clip, 1, &index), m.constf32(float(i)));
    }
  } else {
    EmitEmulatedClipTest(m, mask);
    assert(mask == (63u << RegisterClipDistanceSlots()));
  }
  m.opStore(out, m.constvec4f32(0.0f, 0.0f, 0.0f, 1.0f));
  m.opReturn(); m.functionEnd();
  m.addEntryPoint(main, vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment, "main");
  assert(!m.hasCapability(spv::CapabilityClipDistance));
  std::ofstream stream(path, std::ios::binary); assert(stream); m.compile().store(stream);
}

int main(int argc, char** argv) {
  using namespace dxvk;
  assert(argc == 3);
  uint32_t first = uint32_t(std::stoul(argv[2])); assert(first <= 26);
  g_linkerSlotCount = first;
  for (uint32_t i = 0; i < first; ++i) g_linkerSlots[i] = DxsoSemantic{DxsoUsage::Position, i};
  assert(RegisterClipDistanceSlots() == first && RegisterClipDistanceSlots() == first);
  assert(g_linkerSlotCount == first + 6);
  for (uint32_t i = first; g_linkerSlotCount < 32; ++i)
    assert(RegisterLinkerSlot(DxsoSemantic{DxsoUsage::Position, i}) >= first + 6);
  bool bounded = false;
  try { (void)RegisterLinkerSlot(DxsoSemantic{DxsoUsage::Position, 999}); }
  catch (const DxvkError&) { bounded = true; }
  assert(bounded);
  generate(std::string(argv[1]) + "/clip.vert.spv", true);
  generate(std::string(argv[1]) + "/clip.frag.spv", false);
  return 0;
}
