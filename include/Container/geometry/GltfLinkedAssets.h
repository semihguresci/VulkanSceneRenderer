#pragma once

#include <nlohmann/json_fwd.hpp>
#include <tiny_gltf.h>

#include <filesystem>
#include <functional>
#include <map>
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

struct GltfLinkedReadContext {
  std::filesystem::path root, source;
  std::map<std::filesystem::path, std::filesystem::path> aliases;

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
    const std::filesystem::path&, const GltfLinkedDocumentReader&);

} // namespace container::geometry::gltf
