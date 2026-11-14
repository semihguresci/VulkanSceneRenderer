#pragma once

#include "Container/renderer/effects/DisplayGrade.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace container::renderer {

inline constexpr size_t kMaxRenderSetups = 128;
inline constexpr size_t kMaxRenderSetupNameBytes = 128;
inline constexpr size_t kMaxRenderSetupVisibility = 500'000;

struct RenderSetupSource {
  // Lowercase SHA-256 of the primary glTF or GLB bytes. Separate buffer
  // and texture files do not affect the source-node identity.
  std::string digest;
  uint64_t byteSize{};
  uint32_t sceneIndex{};
  bool operator==(const RenderSetupSource&) const = default;
};

struct RenderSetupVisibility {
  uint32_t sourceNode{};
  bool visible{};
  bool operator==(const RenderSetupVisibility&) const = default;
};

struct RenderSetupEntry {
  std::string name;
  std::optional<uint32_t> cameraSourceNode; // empty uses the base viewport camera
  std::optional<uint32_t> materialVariant; // empty uses glTF default materials
  std::vector<RenderSetupVisibility> visibility;
  std::filesystem::path hdrPath; // empty uses the base environment
  DisplayGrade grade;
};

struct RenderSetupDocument {
  RenderSetupSource source;
  std::vector<RenderSetupEntry> setups;
};

struct RenderSetupNode {
  uint32_t sourceNode{};
  bool camera{};
};

[[nodiscard]] RenderSetupSource FingerprintRenderSetupSource(const std::filesystem::path&,
                                                             uint32_t sceneIndex);
[[nodiscard]] RenderSetupDocument LoadRenderSetupDocument(const std::filesystem::path&);
void SaveRenderSetupDocument(const std::filesystem::path&, const RenderSetupDocument&);
void ValidateRenderSetupSource(const RenderSetupSource& saved, const RenderSetupSource& active);
void ValidateRenderSetupDocument(const RenderSetupDocument&);
void ValidateRenderSetupEntry(const RenderSetupEntry&, const RenderSetupSource& savedSource,
                              const RenderSetupSource& activeSource,
                              std::span<const RenderSetupNode> activeNodes, size_t variantCount);

} // namespace container::renderer
