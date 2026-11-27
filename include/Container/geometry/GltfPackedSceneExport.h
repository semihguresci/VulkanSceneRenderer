#pragma once

#include <Container/geometry/GltfLegacyMaterialSerialization.h>

#include <tiny_gltf.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace container::geometry::gltf {

struct GltfLinkedCompositionInfo;
struct GltfLinkedPlacementEdit;
struct GltfLinkedHybridCarrier;

struct GltfExportResourceIndexMaps {
  std::vector<int32_t> images, textures, samplers;
};

// Writes a detached, already packed model. Callers may add current light or
// camera definitions and node associations before saving. Buffer views must
// reference the model's single in-memory buffer. External resources must
// already be packed, and the input model is read-only.
void writeExportModel(const tinygltf::Model& model,
                      const std::filesystem::path& destination,
                      const GltfLegacyMaterialSerializationLimits& legacyLimits = {});

// Atomic companion-manifest save. Authored external files remain read-only.
void writeLinkedSceneManifest(const GltfLinkedCompositionInfo&,
    std::span<const GltfLinkedPlacementEdit>, size_t selectedScene,
    const std::filesystem::path& destination);

struct GltfLinkedHybridExport {
  const GltfLinkedCompositionInfo& composition;
  std::span<const GltfLinkedHybridCarrier> carriers; // output IDs before node compaction
  std::span<const uint8_t> prunedGraphNodes;
  std::span<const size_t> linkedContexts;
  std::span<const int32_t> sourceMaterialToExport, retiredMaterials, animatedLinkedMaterials;
  const GltfExportResourceIndexMaps& resourceIndexMaps;
};

void writeLinkedHybridScene(tinygltf::Model model, const GltfLinkedHybridExport&,
                            const std::filesystem::path& destination);

}  // namespace container::geometry::gltf
