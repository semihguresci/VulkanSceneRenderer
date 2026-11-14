#include "Container/renderer/core/SceneDataCapture.h"

#include "Container/renderer/scene/SceneController.h"
#include "Container/utility/SceneGraph.h"

#include <nlohmann/json.hpp>
#include <tinyexr.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace container::renderer {

namespace {

[[nodiscard]] nlohmann::json sourceIndex(int32_t index) {
  return index >= 0 ? nlohmann::json(index) : nlohmann::json(nullptr);
}

[[nodiscard]] nlohmann::json objectIdentity(
    uint32_t objectId, const SceneController& controller, const scene::SceneGraph& graph) {
  const auto graphNode = controller.nodeIndexForObject(objectId - 1);
  const auto* node = graph.getNode(graphNode);
  if (!node || node->retired) {
    throw std::runtime_error("scene data capture could not resolve object ID " + std::to_string(objectId));
  }
  const auto* owner = graph.getNode(node->meshOwnerGraphNode);
  const auto* sourceNode = owner ? owner : node;
  return {{"id", objectId},
          {"graphNode", graphNode},
          {"sourceNode", sourceIndex(sourceNode->sourceNodeIndex)},
          {"sourceMesh", sourceIndex(node->sourceMeshIndex)},
          {"sourcePrimitive", sourceIndex(node->sourcePrimitiveIndex)},
          {"sourceInstance", sourceIndex(node->sourceInstanceIndex)},
          {"name", sourceNode->name.empty() ? node->name : sourceNode->name}};
}

}  // namespace

void EncodedSceneDataCapture::FreeExr::operator()(unsigned char* bytes) const noexcept {
  std::free(bytes);
}

EncodedSceneDataCapture encodeSceneDataCapture(
    std::span<const SceneDataCapturePixel> pixels, uint32_t width, uint32_t height,
    const SceneController& controller, const scene::SceneGraph& graph) {
  // TinyEXR uses signed scanline byte counts and image dimensions.
  constexpr auto maxInt = static_cast<uint32_t>(std::numeric_limits<int>::max());
  if (width == 0 || height == 0 || width > maxInt / sizeof(SceneDataCapturePixel) || height > maxInt ||
      static_cast<uint64_t>(width) * height != pixels.size()) {
    throw std::invalid_argument("invalid scene data capture dimensions");
  }

  std::vector<float> normalX(pixels.size());
  std::vector<float> normalY(pixels.size());
  std::vector<float> normalZ(pixels.size());
  std::vector<uint32_t> objectIds(pixels.size());
  std::vector<float> depth(pixels.size());
  std::vector<bool> visibleObjects(controller.objectData().size());
  for (size_t index = 0; index < pixels.size(); ++index) {
    const auto& pixel = pixels[index];
    if (pixel.objectId == 0) continue;
    if (pixel.objectId > visibleObjects.size() || !(pixel.depth > 0.0f) || !std::isfinite(pixel.depth) ||
        !std::isfinite(pixel.normalX) || !std::isfinite(pixel.normalY) || !std::isfinite(pixel.normalZ)) {
      throw std::runtime_error("invalid surface in scene data capture");
    }
    normalX[index] = pixel.normalX;
    normalY[index] = pixel.normalY;
    normalZ[index] = pixel.normalZ;
    objectIds[index] = pixel.objectId;
    depth[index] = pixel.depth;
    visibleObjects[pixel.objectId - 1] = true;
  }

  nlohmann::json objects = nlohmann::json::array();
  for (size_t objectIndex = 0; objectIndex < visibleObjects.size(); ++objectIndex) {
    if (visibleObjects[objectIndex]) {
      objects.push_back(objectIdentity(static_cast<uint32_t>(objectIndex + 1), controller, graph));
    }
  }
  const nlohmann::json lookup{{"schemaVersion", 1},
                              {"width", width},
                              {"height", height},
                              {"backgroundId", 0},
                              {"depthUnits", "metres"},
                              {"normalSpace", "world"},
                              {"objects", std::move(objects)}};

  // EXR scanline planes follow the lexicographically ordered channel list.
  constexpr std::array<const char*, 5> names{"N.X", "N.Y", "N.Z", "ObjectID", "Z"};
  std::array<EXRChannelInfo, names.size()> channels{};
  std::array<int, names.size()> types{
      TINYEXR_PIXELTYPE_FLOAT, TINYEXR_PIXELTYPE_FLOAT, TINYEXR_PIXELTYPE_FLOAT,
      TINYEXR_PIXELTYPE_UINT, TINYEXR_PIXELTYPE_FLOAT};
  std::array<unsigned char*, names.size()> images{
      reinterpret_cast<unsigned char*>(normalX.data()),
      reinterpret_cast<unsigned char*>(normalY.data()),
      reinterpret_cast<unsigned char*>(normalZ.data()),
      reinterpret_cast<unsigned char*>(objectIds.data()),
      reinterpret_cast<unsigned char*>(depth.data())};
  for (size_t index = 0; index < names.size(); ++index) {
    std::memcpy(channels[index].name, names[index], std::strlen(names[index]) + 1);
  }

  EXRHeader header;
  InitEXRHeader(&header);
  header.num_channels = static_cast<int>(channels.size());
  header.channels = channels.data();
  header.pixel_types = types.data();
  header.requested_pixel_types = types.data();
  header.compression_type = TINYEXR_COMPRESSIONTYPE_NONE;

  EXRImage image;
  InitEXRImage(&image);
  image.width = static_cast<int>(width);
  image.height = static_cast<int>(height);
  image.num_channels = header.num_channels;
  image.images = images.data();

  unsigned char* bytes = nullptr;
  const char* error = nullptr;
  const size_t size = SaveEXRImageToMemory(&image, &header, &bytes, &error);
  EncodedSceneDataCapture output;
  output.exr.reset(bytes);
  const std::unique_ptr<const char, decltype(&FreeEXRErrorMessage)> message(error, &FreeEXRErrorMessage);
  if (!output.exr || size == 0) {
    throw std::runtime_error(std::string("failed to encode scene data EXR: ") +
                             (message ? message.get() : "unknown encoder error"));
  }
  output.exrSize = size;
  output.lookupJson = lookup.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';
  return output;
}

std::filesystem::path sceneDataExrPath(std::filesystem::path image) {
  image.replace_extension(".data.exr");
  return image;
}

std::filesystem::path sceneDataLookupPath(std::filesystem::path image) {
  image.replace_extension(".data.json");
  return image;
}

}  // namespace container::renderer
