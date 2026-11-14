#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace container::scene {
class SceneGraph;
}

namespace container::renderer {

class SceneController;

// Readback layout from the scene data capture shader. Object ID zero is background.
struct SceneDataCapturePixel {
  float depth;
  float normalX;
  float normalY;
  float normalZ;
  uint32_t objectId;
};
static_assert(sizeof(SceneDataCapturePixel) == 20);
static_assert(offsetof(SceneDataCapturePixel, objectId) == 16);

struct EncodedSceneDataCapture {
  struct FreeExr {
    void operator()(unsigned char* bytes) const noexcept;
  };

  std::unique_ptr<unsigned char, FreeExr> exr;
  size_t exrSize{0};
  std::string lookupJson;

  [[nodiscard]] std::string_view exrBytes() const noexcept {
    return exr ? std::string_view{reinterpret_cast<const char*>(exr.get()), exrSize}
               : std::string_view{};
  }
};

// Encode a top-left origin capture and map every visible scene ID from this frame.
[[nodiscard]] EncodedSceneDataCapture encodeSceneDataCapture(
    std::span<const SceneDataCapturePixel> pixels, uint32_t width, uint32_t height,
    const SceneController& controller, const scene::SceneGraph& graph);

[[nodiscard]] std::filesystem::path sceneDataExrPath(std::filesystem::path image);
[[nodiscard]] std::filesystem::path sceneDataLookupPath(std::filesystem::path image);

}  // namespace container::renderer
