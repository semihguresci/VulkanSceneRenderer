#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tinygltf { class Model; class Value; }

namespace container::geometry::gltf {

inline constexpr char kGltfIesExtension[] = "EXT_lights_ies";
inline constexpr char kIesMimeType[] = "application/x-ies-lm-63";
inline constexpr size_t kIesMaximumProfileBytes = 2 * 1024 * 1024;
inline constexpr size_t kIesMaximumProfiles = 64;

enum class IesHorizontalSymmetry : uint32_t { Axial, Quadrant, Half, OppositeHalf, Full };

struct IesProfile {
  std::vector<float> verticalDegrees;
  std::vector<float> horizontalDegrees;
  std::vector<float> candela;
  IesHorizontalSymmetry symmetry{};

  [[nodiscard]] float sample(float vertical, float horizontal) const;
  [[nodiscard]] std::vector<float> packedForGpu() const;
};

struct GltfIesBinding {
  size_t profile{};
  float multiplier{1.0f};
  float color[3]{1.0f, 1.0f, 1.0f};
};

[[nodiscard]] IesProfile ParseIesProfile(std::string_view text);
[[nodiscard]] GltfIesBinding ParseGltfIesBinding(const tinygltf::Value& value, size_t profileCount);
[[nodiscard]] std::vector<std::string> PrepareGltfIesProfiles(tinygltf::Model& model,
    const std::filesystem::path& directory, bool allowExternal, uintmax_t& remainingInputBytes);
[[nodiscard]] const std::string* GltfIesEmbeddedProfileUri(const tinygltf::Model& model, size_t index);
[[nodiscard]] std::string DecodeGltfIesProfileUri(std::string_view uri);

} // namespace container::geometry::gltf
