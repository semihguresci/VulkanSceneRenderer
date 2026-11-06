#pragma once

#include <nlohmann/json_fwd.hpp>
#include <tiny_gltf.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace container::geometry::gltf {

inline constexpr char kGltfXmpExtension[] = "KHR_xmp_json_ld";

class GltfXmpImport {
 public:
  // Retain packet JSON before TinyGLTF drops empty values or narrows integers.
  // The parser receives only an empty extension shell and checked references.
  explicit GltfXmpImport(nlohmann::json& manifest);
  [[nodiscard]] bool rewroteManifest() const { return rewritten_; }
  [[nodiscard]] size_t retainedBytes() const { return rootExtensionsJson_.size(); }
  [[nodiscard]] const std::vector<std::string>& warnings() const { return warnings_; }
  void retain(tinygltf::Model& model) &&;

 private:
  std::string rootExtensionsJson_;
  std::vector<std::string> warnings_;
  bool rewritten_{};
};

// Parse the retained root extension only when an inspector is open.
[[nodiscard]] std::optional<nlohmann::json> ReadGltfXmpPackage(const tinygltf::Model& model);

struct GltfXmpPacketView {
  const nlohmann::json* packet{};
  std::string_view message;
};

[[nodiscard]] GltfXmpPacketView ResolveGltfXmpPacket(
    const nlohmann::json& package, const tinygltf::ExtensionMap& extensions);

}  // namespace container::geometry::gltf
