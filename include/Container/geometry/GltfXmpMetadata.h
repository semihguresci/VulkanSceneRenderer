#pragma once

#include <nlohmann/json_fwd.hpp>
#include <tiny_gltf.h>

#include <cstddef>
#include <cstdint>
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

struct GltfXmpComposition {
  std::string rootExtensionsJson;
  size_t destinationPackets{}, incomingPackets{};
};

// Keep source packet order and preserve packet JSON without TinyGLTF narrowing.
[[nodiscard]] GltfXmpComposition ComposeGltfXmpPackets(
    const tinygltf::Model& destination, const tinygltf::Model& incoming, uint32_t maxMetadataDepth);

// Remap one attachment in its original packet domain. Invalid indices remain
// invalid even when another source appends packets at the same indices.
[[nodiscard]] bool RemapGltfXmpAttachment(tinygltf::ExtensionMap&, size_t sourcePackets, size_t offset);

}  // namespace container::geometry::gltf
