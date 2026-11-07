#include "Container/geometry/OpenVdbTransportImageScene.h"
#include "Container/renderer/effects/OpenVdbTransportGpuReference.h"
#include "OpenVdbTransportImageInternal.h"

namespace container::geometry::vdb {
bool TransportImageScene::hasNativeSources() const noexcept {
  for (const auto &s : native_)
    if (s)
      return true;
  return false;
}
std::shared_ptr<const TransportImageScene>
LoadTransportImageScene(const TransportImageSceneDescription &description) {
  if (!openVdbVolumeAvailable())
    throw std::runtime_error(
        "OpenVDB image scene loading is unavailable; configure with -DENABLE_OPENVDB=ON.");
  using namespace image_detail;
  if (description.volumes.size() > 4 || description.lights.size() > 32 ||
      description.occluders.size() > 4096 || !description.maxInputBytes ||
      !description.maxDensityValues)
    throw std::invalid_argument(
        "transport image scene source/light/triangle/budget limits exceeded");
  auto out = std::make_shared<TransportImageScene>();
  out->scene_.lights = description.lights;
  out->scene_.occluders = description.occluders;
  out->scene_.environment = description.environment;
  uint64_t retained = add(sceneBytes(out->scene_), Fixed), values = 0;
  budget(retained, description.maxInputBytes, "transport image scene metadata exceeds byte budget");
  for (size_t i = 0; i < description.volumes.size(); ++i) {
    const auto &source = description.volumes[i];
    auto options = source.volume;
    if (!options.temperatureGridName.empty())
      throw std::invalid_argument("transport image physical model does not implement temperature "
                                  "emission; use RGB emission");
    options.maxInputBytes =
        std::min<uint64_t>(options.maxInputBytes, description.maxInputBytes - retained);
    OpenVdbVolume dense;
    const OpenVdbVolume *volume;
    if (options.sparse) {
      out->native_[i] = renderer::LoadTransportGpuNativeSource(source.input, options);
      volume = &out->native_[i]->volume();
    } else {
      dense = loadOpenVdbVolume(source.input, options);
      volume = &dense;
    }
    const auto &v = *volume;
    const uint64_t payload = mul(add(add(v.density.size(), v.temperature.size()),
                                     add(v.sparseDensity.size(), v.sparseTemperature.size())),
                                 4);
    const uint64_t bytes =
        add(payload, add(sizeof(OpenVdbVolume),
                         add(v.gridName.size(), source.input.native().size() *
                                                    sizeof(std::filesystem::path::value_type))));
    budget(add(retained, bytes), description.maxInputBytes,
           "all retained transport image sources exceed byte budget");
    if (!options.sparse)
      values = add(values, v.density.size());
    if (values > description.maxDensityValues)
      throw std::invalid_argument("transport image dense source values exceed aggregate budget");
    TransportMedium medium;
    medium.dimensions = v.dimensions;
    medium.density.clear();
    for (int k = 0; k < 3; ++k) {
      medium.boundsMin[k] = v.boundsMin[k];
      medium.boundsMax[k] = v.boundsMax[k];
    }
    medium.extinction = source.material.extinction;
    medium.albedo = source.material.albedo;
    medium.anisotropy = source.material.anisotropy;
    medium.emission = source.material.emission;
    if (!options.sparse)
      medium.density = std::move(dense.density);
    out->scene_.media.push_back(std::move(medium));
    out->info_.push_back({source.input,
                          v.gridName,
                          options.sparse,
                          v.dimensions,
                          {v.boundsMin[0], v.boundsMin[1], v.boundsMin[2]},
                          {v.boundsMax[0], v.boundsMax[1], v.boundsMax[2]},
                          bytes,
                          v.sourceActiveVoxels});
    retained =
        add(retained, add(bytes, sizeof(TransportMedium) + sizeof(TransportImageSourceInfo)));
    budget(retained, description.maxInputBytes,
           "transport image source diagnostics exceed byte budget");
  }
  // Reuse reference validation without tracing: an order0, one-sample vacuum
  // view is NOT a substitute for validating the actual loaded material. These
  // simple coefficient/scene checks deliberately do not issue any transport.
  const auto color = [](const TransportVector &v, double limit) {
    for (double x : v)
      if (!std::isfinite(x) || x < 0 || x > limit)
        throw std::invalid_argument("transport image scene color/coefficient out of range");
  };
  color(out->scene_.environment, 1e6);
  for (const auto &m : out->scene_.media) {
    color(m.albedo, 1);
    color(m.emission, 1e6);
    if (!std::isfinite(m.extinction) || m.extinction < 0 || m.extinction > 1e6 ||
        !std::isfinite(m.anisotropy) || std::abs(m.anisotropy) > .95)
      throw std::invalid_argument("transport image scene extinction/HG out of range");
  }
  for (const auto &light : out->scene_.lights) {
    color(light.value, 1e6);
    for (double x : light.vector)
      if (!std::isfinite(x) || std::abs(x) > 1e12)
        throw std::invalid_argument("transport image light coordinates out of range");
    if (light.type != TransportLightType::Directional && light.type != TransportLightType::Point)
      throw std::invalid_argument("transport image light type unsupported");
    if (light.type == TransportLightType::Directional)
      (void)unit(light.vector);
  }
  for (const auto &t : out->scene_.occluders)
    for (const auto &p : {t.a, t.b, t.c})
      for (double x : p)
        if (!std::isfinite(x) || std::abs(x) > 1e12)
          throw std::invalid_argument("transport image triangle coordinates out of range");
  return out;
}
} // namespace container::geometry::vdb
