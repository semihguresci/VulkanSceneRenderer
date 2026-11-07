#pragma once

#include <Container/utility/GltfMaterialConversion.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace container::material {

struct RuntimeMaterialSourceTextures {
  std::array<int32_t, size_t(GltfMaterialTextureSlot::Count)> indices = [] {
    std::array<int32_t, size_t(GltfMaterialTextureSlot::Count)> result;
    result.fill(-1);return result;
  }();
  int32_t& operator[](size_t index) noexcept {return indices[index];}
  const int32_t& operator[](size_t index) const noexcept {return indices[index];}
  constexpr size_t size() const noexcept {return indices.size();}
  bool operator==(const RuntimeMaterialSourceTextures&) const = default;
};

// Reads only known, explicit source texture references in conversion precedence
// order. No resource lookup/reverse mapping or material/model/table copy occurs.
// All retained metadata is bounded before traversal; all known fallback indices
// are validated even when a higher-precedence binding wins.
[[nodiscard]] RuntimeMaterialSourceTextures ReadGltfMaterialSourceTextures(
    const tinygltf::Material&, size_t sourceTextureCount,
    const GltfMaterialConversionLimits& limits = {});

// Bit-exact individual numeric fields, flags, texture bindings and transforms.
// Ignores ABI padding and never allocates. Positive/negative zero differ.
[[nodiscard]] bool RuntimeMaterialsEqual(const Material&, const Material&) noexcept;

struct RuntimeMaterialAuthoringView {
  GltfMaterialConversionContext context;
  const tinygltf::Material* retained{};
};
struct RuntimeMaterialAuthoringLimits {
  GltfMaterialConversionLimits conversion{};
  // Conservative selected-record output plus conversion/validation coexistence.
  // Does not include externally owned source/resource tables or allocator data.
  uint64_t maxOutputBytes{64ull * 1024 * 1024};
};
class RuntimeMaterialAuthoringResult {
 public:
  explicit operator bool() const noexcept {return bytes_!=0;}
  [[nodiscard]] const tinygltf::Material& authored() const noexcept {return authored_;}
  [[nodiscard]] const tinygltf::Material& evaluated() const noexcept {return evaluated_;}
  [[nodiscard]] std::span<const std::string> extensionsToDeclare() const noexcept {return declarations_;}
  [[nodiscard]] uint64_t ownedBytes() const noexcept {return bytes_;}
 private:
  tinygltf::Material authored_,evaluated_;
  std::vector<std::string> declarations_;
  uint64_t bytes_{};
  friend RuntimeMaterialAuthoringResult MaterializeRuntimeMaterial(const Material&,
      const RuntimeMaterialSourceTextures&, const RuntimeMaterialSourceTextures&,
      RuntimeMaterialAuthoringView, RuntimeMaterialAuthoringView,
      const RuntimeMaterialAuthoringLimits&);
};

// Source identities are supplied separately for archive/authored and live/
// evaluated resources. Exact forward conversion must reproduce every runtime
// field and all 23 bindings; unsupported lossless representations reject with
// a named field rather than approximating or inferring source identities.
// Retained source records keep opaque metadata. SG requires an exact retained
// definition because its import approximation has no general inverse.
[[nodiscard]] RuntimeMaterialAuthoringResult MaterializeRuntimeMaterial(
    const Material&, const RuntimeMaterialSourceTextures& authoredTextures,
    const RuntimeMaterialSourceTextures& evaluatedTextures,
    RuntimeMaterialAuthoringView authored, RuntimeMaterialAuthoringView evaluated,
    const RuntimeMaterialAuthoringLimits& limits = {});

} // namespace container::material
