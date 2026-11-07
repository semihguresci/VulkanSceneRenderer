#pragma once
#include "Container/geometry/OpenVdbTransportImage.h"
#include <memory>
namespace container::renderer {
class TransportGpuNativeSource;
}
namespace container::geometry::vdb {
struct TransportImageMaterial {
  double extinction{1};
  TransportVector albedo{1, 1, 1};
  double anisotropy{};
  TransportVector emission{};
};
struct TransportImageVolumeSource {
  std::filesystem::path input;
  OpenVdbVolumeOptions volume;
  TransportImageMaterial material;
};
struct TransportImageSceneDescription {
  std::vector<TransportImageVolumeSource> volumes;
  std::vector<TransportLight> lights;
  std::vector<TransportTriangle> occluders;
  TransportVector environment{};
  uint64_t maxInputBytes{512ull * 1024 * 1024}, maxDensityValues{16'000'000};
};
struct TransportImageSourceInfo {
  std::filesystem::path input;
  std::string gridName;
  bool nativeSparse{};
  std::array<uint32_t, 3> dimensions{};
  TransportVector boundsMin{}, boundsMax{};
  uint64_t retainedBytes{}, sourceActiveVoxels{};
};
class TransportImageScene {
public:
  [[nodiscard]] const TransportScene &scene() const noexcept { return scene_; }
  [[nodiscard]] const std::vector<TransportImageSourceInfo> &sources() const noexcept {
    return info_;
  }
  [[nodiscard]] const auto &nativeSources() const noexcept { return native_; }
  [[nodiscard]] bool hasNativeSources() const noexcept;

private:
  TransportScene scene_;
  std::vector<TransportImageSourceInfo> info_;
  std::array<std::shared_ptr<const renderer::TransportGpuNativeSource>, 4> native_{};
  friend std::shared_ptr<const TransportImageScene>
  LoadTransportImageScene(const TransportImageSceneDescription &);
};
// Up to4 fog sources,32 directional/point lights,4096 black triangles.
// Dense preview import uses the existing loader's bounded world-aligned
// resampling and density clamp[0,1e6]. Native sparse sources preserve trusted
// Float32 NanoVDB topology/maps through the factory, not arbitrary binary data.
// Source options.sparse chooses the representation. CPU rendering supports
// dense only; native sources require the GPU route. Temperature selection is
// rejected because this physical model has independent RGB emission only.
// All retained source payloads and one current loader workspace share the
// source byte cap. Inputs remain untouched. OpenVDB-disabled fails first.
[[nodiscard]] std::shared_ptr<const TransportImageScene>
LoadTransportImageScene(const TransportImageSceneDescription &);
} // namespace container::geometry::vdb
