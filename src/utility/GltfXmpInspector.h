#pragma once

#include <Container/utility/ResourceCatalogControls.h>

#include <nlohmann/json.hpp>
#include <tiny_gltf.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace container::scene {
class SceneGraph;
}

namespace container::ui::detail {

class GltfXmpInspector {
 public:
  void setSource(const tinygltf::Model*, uint64_t graphIdentity, uint64_t generation);
  void drawAsset();
  void drawScene(int selectedScene);
  void drawSelection(const scene::SceneGraph&, uint32_t selectedNode, bool hasBimSelection);
  void drawSurfaceMaterial(int sourceMaterial);
  void drawCatalogMaterial(int sourceMaterial);
  void drawAnimation(int selectedClip);
  void drawImage(const std::optional<ResourceCatalogTarget>&, std::span<const tinygltf::Image> archiveImages);

 private:
  enum class Scope { Asset, Scene, Node, Mesh, SurfaceMaterial, CatalogMaterial, Animation, Image, Count };
  struct RawPreview {
    const nlohmann::json* packet{};
    std::string text;
    bool shortened{};
  };

  void draw(Scope, const tinygltf::ExtensionMap&);

  const tinygltf::Model* model_{};
  uint64_t graphIdentity_{}, generation_{};
  std::optional<nlohmann::json> package_;
  std::array<RawPreview, static_cast<size_t>(Scope::Count)> previews_;
};

} // namespace container::ui::detail
