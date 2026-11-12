#pragma once

#include <nlohmann/json_fwd.hpp>
#include <tiny_gltf.h>

#include <filesystem>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace container::geometry::gltf {

struct GltfLinkedFile {
  std::filesystem::path uri;
  std::string mimeType;
  std::map<std::filesystem::path, size_t> aliases;
};

struct GltfLinkedManifest {
  bool normalized{};
  std::vector<GltfLinkedFile> files;
  // Source node index to file index, with -1 for ordinary nodes.
  std::vector<int> nodeFiles;
};

// Parse only the pinned 2.1 draft URI composition fields. Normalize the
// validated manifest to 2.0 for the ordinary parser and flattening pipeline.
[[nodiscard]] GltfLinkedManifest PrepareGltfLinkedManifest(nlohmann::json&, bool enabled,
                                                           std::stop_token stop = {});

struct GltfLinkedFileContents {
  uint64_t size{}, hash{};
  bool operator==(const GltfLinkedFileContents&) const = default;
};

// Indices refer to the flattened model returned by the same load.
struct GltfLinkedIndexRange {
  int32_t first{};
  size_t count{};

  [[nodiscard]] bool contains(int32_t index) const noexcept {
    return index >= first && size_t(index - first) < count;
  }
};

struct GltfLinkedReadWitnesses {
  std::set<std::filesystem::path> inputs, resolutions;
};

struct GltfLinkedSourceOwnership {
  std::filesystem::path source;
  std::map<std::filesystem::path, std::filesystem::path> aliases;
  GltfLinkedIndexRange buffers, bufferViews, accessors, images, samplers, textures;
  GltfLinkedIndexRange materials, meshes, cameras, lights, animations, variants, xmpPackets;
  std::map<std::string, GltfLinkedIndexRange> extensionLights;
  GltfLinkedReadWitnesses reads;
};

struct GltfLinkedPlacementOwnership {
  int32_t carrierNode{-1};
  // An outer placement owns a nested link's carrier, but the nested
  // placement owns the payload attached to that carrier.
  int32_t parentPlacement{-1};
  size_t sourceContext{};
  size_t manifestFile{};
  GltfLinkedIndexRange skins;
  size_t bindingWitness{};
};

// Captured only by scene imports. Canonical resolution and encoded content
// witnesses prevent a linked save from silently changing its source inputs.
struct GltfLinkedCompositionInfo {
  std::filesystem::path source;
  size_t primaryNodes{};
  bool primaryAnimated{};
  bool primaryBinary{};
  std::vector<int> placements;
  std::map<std::filesystem::path, std::filesystem::path> resolutions;
  std::map<std::filesystem::path, GltfLinkedFileContents> inputs;
  // Trusted, import-local ownership. The provenance extras written to
  // portable files are independent and never populate these tables.
  std::vector<GltfLinkedSourceOwnership> sourceContexts;
  std::vector<GltfLinkedPlacementOwnership> placementRecords;
  std::vector<int32_t> nodePlacements; // Source node -> concrete placement, or -1.
  std::vector<std::set<std::filesystem::path>> bindingResolutions;
  uint64_t dependencyWitnessBytes{};

  void observe(const std::filesystem::path&, std::span<const unsigned char>, std::stop_token stop = {});
  void recordWitness(std::set<std::filesystem::path>&, const std::filesystem::path&);
};

struct GltfLinkedPlacementEdit {
  int node{};
  std::optional<std::string> name;
  std::optional<std::array<double, 16>> matrix;
};

// Reject destinations that resolve to an imported file, including physical
// aliases and dependency paths that have changed since the import.
void ValidateGltfLinkedExportDestination(const GltfLinkedCompositionInfo&, const std::filesystem::path&);

// Preserve the URI and alias tables of a static draft manifest. The caller
// proves these are the only scene edits. The destination stays beside the
// original manifest so its nested files and aliases resolve identically.
[[nodiscard]] nlohmann::json BuildGltfLinkedManifestForSave(const GltfLinkedCompositionInfo&,
    std::span<const GltfLinkedPlacementEdit>, size_t selectedScene,
    const std::filesystem::path& destination);

struct GltfLinkedHybridCarrier {
  int32_t sourceNode{-1}, outputNode{-1};
};

// Return a detached JSON patch for surviving primary carriers. Only their
// resolved dependency closures must still exist. The primary manifest and all
// originally imported paths remain protected from replacement.
[[nodiscard]] nlohmann::json BuildGltfLinkedHybridManifest(const GltfLinkedCompositionInfo&,
    std::span<const GltfLinkedHybridCarrier>, const std::filesystem::path& destination);
void ApplyGltfLinkedHybridManifest(nlohmann::json& document, const nlohmann::json& patch);

struct GltfLinkedReadContext {
  std::filesystem::path root, source;
  std::map<std::filesystem::path, std::filesystem::path> aliases;
  GltfLinkedCompositionInfo* composition{};
  bool child{};
  GltfLinkedReadWitnesses* witnesses{};
  std::stop_token stop;

  [[nodiscard]] std::filesystem::path resource(std::string_view uri) const;
  void observe(const std::filesystem::path&, std::span<const unsigned char>) const;
};

struct GltfLinkedDocument {
  tinygltf::Model model;
  GltfLinkedManifest manifest;
  bool binary{};
};

using GltfLinkedDocumentReader = std::function<GltfLinkedDocument(const GltfLinkedReadContext&)>;

// One detached import. Resolved source resources are reused while each
// placement gets its own node identities. All resolved reads stay in root.
[[nodiscard]] tinygltf::Model LoadGltfLinkedComposition(
    const std::filesystem::path&, const GltfLinkedDocumentReader&,
    GltfLinkedCompositionInfo* = nullptr, std::stop_token stop = {});

} // namespace container::geometry::gltf
