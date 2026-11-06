#pragma once

#include <nlohmann/json_fwd.hpp>
#include <tiny_gltf.h>

#include <filesystem>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
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
[[nodiscard]] GltfLinkedManifest PrepareGltfLinkedManifest(nlohmann::json&, bool enabled);

struct GltfLinkedFileContents {
  uint64_t size{}, hash{};
  bool operator==(const GltfLinkedFileContents&) const = default;
};

// Captured only by scene imports. Canonical resolution and encoded content
// witnesses prevent a linked save from silently changing its source inputs.
struct GltfLinkedCompositionInfo {
  std::filesystem::path source;
  size_t primaryNodes{};
  std::vector<int> placements;
  std::map<std::filesystem::path, std::filesystem::path> resolutions;
  std::map<std::filesystem::path, GltfLinkedFileContents> inputs;

  void observe(const std::filesystem::path&, std::span<const unsigned char>);
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

struct GltfLinkedReadContext {
  std::filesystem::path root, source;
  std::map<std::filesystem::path, std::filesystem::path> aliases;
  GltfLinkedCompositionInfo* composition{};

  [[nodiscard]] std::filesystem::path resource(std::string_view uri) const;
};

struct GltfLinkedDocument {
  tinygltf::Model model;
  GltfLinkedManifest manifest;
};

using GltfLinkedDocumentReader = std::function<GltfLinkedDocument(const GltfLinkedReadContext&)>;

// One detached import. Resolved source resources are reused while each
// placement gets its own node identities. All resolved reads stay in root.
[[nodiscard]] tinygltf::Model LoadGltfLinkedComposition(
    const std::filesystem::path&, const GltfLinkedDocumentReader&,
    GltfLinkedCompositionInfo* = nullptr);

} // namespace container::geometry::gltf
