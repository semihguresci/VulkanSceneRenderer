#pragma once

#include <nlohmann/json_fwd.hpp>
#include <tiny_gltf.h>

#include <cstdint>
#include <vector>

namespace container::geometry::gltf {

enum class GltfNodeFlag : uint8_t {
  Visibility = 1,
  Selectability = 2,
  Hoverability = 4,
};

struct GltfNodeFlags {
  bool visible{true};
  bool selectable{true};
  bool hoverable{true};
  // Preserve absent, empty/defaulted and explicitly authored extension values
  // as distinct source states. Values above are local, never inherited.
  uint8_t authoredExtensions{};
  uint8_t authoredProperties{};

  [[nodiscard]] bool hasExtension(GltfNodeFlag flag) const noexcept {
    return (authoredExtensions & static_cast<uint8_t>(flag)) != 0;
  }
  [[nodiscard]] bool hasProperty(GltfNodeFlag flag) const noexcept {
    return (authoredProperties & static_cast<uint8_t>(flag)) != 0;
  }
  friend bool operator==(const GltfNodeFlags&, const GltfNodeFlags&) = default;
};

struct GltfNodeFlagLimits {
  uint64_t maxNodes{1'000'000};
  uint64_t maxRetainedBytes{16ull * 1024 * 1024};
};
struct GltfNodeFlagSummary {
  uint64_t nodes{}, authoredNodes{}, authoredExtensions{}, retainedBytes{};
  friend bool operator==(const GltfNodeFlagSummary&, const GltfNodeFlagSummary&) = default;
};
struct GltfNodeFlagData {
  // Dense source-node order, including nodes inactive in the current scene.
  std::vector<GltfNodeFlags> nodes;
  GltfNodeFlagSummary summary;
};

// Strict, read-only metadata gate before TinyGLTF resource callbacks. Each
// extension is an object, its optional named property must be a JSON boolean,
// and an absent property defaults true. Authored extensions must be declared
// in extensionsUsed. Unknown properties/extras remain opaque and untouched.
[[nodiscard]] GltfNodeFlagSummary ValidateGltfNodeFlagsJson(
    const nlohmann::json& source, const GltfNodeFlagLimits& limits = {});

// Parses the same local semantics from a detached model without mutating it.
// At runtime each flag inherits independently by AND through the hierarchy;
// visibility does not change camera availability or the other two flags.
// Synthetic primitive/instance children start with defaults and inherit the
// source group's flags. Effective values must not replace these local values.
[[nodiscard]] GltfNodeFlagData ParseGltfNodeFlags(
    const tinygltf::Model& source, const GltfNodeFlagLimits& limits = {});

} // namespace container::geometry::gltf
