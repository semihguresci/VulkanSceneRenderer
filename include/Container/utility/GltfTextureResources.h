#pragma once

#include <Container/geometry/GltfImageSources.h>
#include <Container/utility/MaterialSamplerKey.h>
#include <Container/utility/TextureManager.h>

#include <functional>
#include <limits>

namespace container::material {

struct GltfTextureResources {
  std::vector<uint32_t> color;
  std::vector<uint32_t> data;
};
struct PreparedGltfTextureImage {
  TextureImageData image;
  std::string contentKey;
};
struct PreparedGltfTextureBinding {
  size_t textureIndex{}, imageIndex{};
  bool isSrgb{};
  MaterialSamplerKey sampler;
};
struct PreparedGltfTextures {
  size_t textureCount{};
  std::vector<PreparedGltfTextureImage> images;
  std::vector<PreparedGltfTextureBinding> bindings;
};

// Decode/validate every selected image role on detached CPU storage, before
// retiring a live scene. This includes materials available only in variants.
// Output bytes are bounded across the complete preparation, not per image.
[[nodiscard]] PreparedGltfTextures PrepareGltfTextures(
    const tinygltf::Model& model, const TextureDecodeOptions& deviceProfile,
    uint64_t maxPreparedBytes = 512ull * 1024 * 1024,
    bool enableExperimentalMaterials = false);

// Exact number of distinct image/role/sampler resources the prepared producer
// will publish into a fresh table. Validates every binding before any upload.
[[nodiscard]] size_t CountPreparedGltfTextureResources(const PreparedGltfTextures&);

// Different sampler policies can share one uploaded image/view. Content keys
// include format, color-space role, swizzle and the complete authored mip chain.
[[nodiscard]] GltfTextureResources UploadPreparedGltfTextures(
    const PreparedGltfTextures& prepared, TextureManager& manager,
    const std::function<TextureResource(const std::string&, const TextureImageData&)>& uploader);

} // namespace container::material
