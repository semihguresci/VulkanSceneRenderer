#pragma once
#include "Container/geometry/OpenVdbTransportImage.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace container::geometry::vdb::image_detail {
constexpr uint32_t BlockPixels = 64;
constexpr uint64_t Fixed = 1024 * 1024;
inline uint64_t add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX - a)
    throw std::invalid_argument("transport image integer overflow");
  return a + b;
}
inline uint64_t mul(uint64_t a, uint64_t b) {
  if (a && b > UINT64_MAX / a)
    throw std::invalid_argument("transport image integer overflow");
  return a * b;
}
inline void budget(uint64_t bytes, uint64_t maximum, const char *why) {
  if (bytes > maximum)
    throw std::invalid_argument(why);
}
inline double dot(const TransportVector &a, const TransportVector &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline TransportVector cross(const TransportVector &a, const TransportVector &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline TransportVector unit(TransportVector v) {
  double n = std::hypot(v[0], v[1], v[2]);
  if (!std::isfinite(n) || n == 0)
    throw std::invalid_argument("transport image camera basis is degenerate");
  for (auto &x : v)
    x /= n;
  return v;
}
inline uint64_t mix(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}
inline uint64_t sceneBytes(const TransportScene &s) {
  uint64_t n = sizeof(s);
  n = add(n, mul(s.media.size(), sizeof(TransportMedium)));
  for (const auto &m : s.media)
    n = add(n, mul(m.density.size(), sizeof(float)));
  n = add(n, mul(s.lights.size(), sizeof(TransportLight)));
  return add(n, mul(s.occluders.size(), sizeof(TransportTriangle)));
}
struct Camera {
  TransportPinholeCamera c;
  TransportVector forward, right, up;
  double tanHalf{}, aspect{};
  Camera(const TransportPinholeCamera &camera, uint32_t width, uint32_t height) : c(camera) {
    for (int k = 0; k < 3; ++k)
      for (double x : {c.eye[k], c.target[k], c.up[k]})
        if (!std::isfinite(x) || std::abs(x) > 1e12)
          throw std::invalid_argument(
              "transport image camera coordinates must be finite and at most1e12");
    if (!std::isfinite(c.verticalFovDegrees) || c.verticalFovDegrees < .1 ||
        c.verticalFovDegrees > 150)
      throw std::invalid_argument("transport image FOV must be within[0.1,150] degrees");
    forward = unit({c.target[0] - c.eye[0], c.target[1] - c.eye[1], c.target[2] - c.eye[2]});
    const auto hint = unit(c.up);
    const auto r = cross(forward, hint);
    if (std::hypot(r[0], r[1], r[2]) < 1e-8)
      throw std::invalid_argument("transport image up is parallel to view direction");
    right = unit(r);
    up = unit(cross(right, forward));
    aspect = double(width) / height;
    tanHalf = std::tan(c.verticalFovDegrees * std::numbers::pi / 360);
  }
  TransportRay ray(uint64_t index, uint32_t width, uint32_t height) const {
    const double x = (2 * (double(index % width) + .5) / width - 1) * aspect * tanHalf;
    const double y = (1 - 2 * (double(index / width) + .5) / height) * tanHalf;
    return {c.eye,
            unit({forward[0] + x * right[0] + y * up[0], forward[1] + x * right[1] + y * up[1],
                  forward[2] + x * right[2] + y * up[2]})};
  }
};
inline void counter(uint64_t &a, uint64_t b) { a = add(a, b); }
template <class Trace>
TransportImageResult render(const TransportScene &scene, const TransportPinholeCamera &camera,
                            const TransportImageOptions &o, uint64_t extraSceneBytes,
                            uint64_t nativeWords, bool gpu, Trace &&trace) {
  const auto &p = o.transport;
  if (o.width < 1 || o.height < 1 || o.width > 2048 || o.height > 2048 || o.maxPixels < 1 ||
      o.maxPixels > 262144 || o.maxPaths < 1 || o.maxPaths > 16'000'000 || o.maxImageBytes < 1 ||
      o.maxImageBytes > 4ull * 1024 * 1024 * 1024 || p.samplesPerRay < 1 ||
      p.samplesPerRay > 8192 || p.maxWork < 1 || p.maxWork > 1'000'000'000 || p.maxInputBytes < 1)
    throw std::invalid_argument("transport image dimensions/sampling/budgets out of range");
  const uint64_t count = mul(o.width, o.height), paths = mul(count, p.samplesPerRay);
  if (count > o.maxPixels || paths > o.maxPaths)
    throw std::invalid_argument("transport image pixel/path budget exceeded");
  const Camera view(camera, o.width, o.height);
  const uint64_t source = add(sceneBytes(scene), extraSceneBytes),
                 storage = mul(count, sizeof(TransportRayEstimate));
  const uint64_t retained = add(add(source, storage), Fixed);
  budget(add(retained, mul(BlockPixels, sizeof(TransportRay) + sizeof(TransportRayEstimate))),
         o.maxImageBytes, "transport image retained scene/result workspace exceeds byte budget");
  uint64_t prep = add(add(nativeWords, mul(scene.media.size(), 32)),
                      add(mul(scene.lights.size(), 8), mul(scene.occluders.size(), 12)));
  for (const auto &m : scene.media)
    prep = add(prep, m.density.size());
  prep = add(prep, BlockPixels * 12);
  const uint64_t blocks = (count + BlockPixels - 1) / BlockPixels;
  const uint64_t preparation = mul(blocks, prep);
  if (add(preparation, paths) > p.maxWork)
    throw std::invalid_argument("transport image preparation/path work exceeds aggregate budget");
  TransportImageResult out;
  out.width = o.width;
  out.height = o.height;
  out.samplesPerPixel = p.samplesPerRay;
  out.maxScatteringEvents = p.maxScatteringEvents;
  out.seed = p.seed;
  out.camera = camera;
  out.gpu = gpu;
  out.retainedSceneBytes = source;
  out.maxImageBytes = o.maxImageBytes;
  out.pixels.resize(size_t(count));
  out.diagnostics.preparationWork = preparation;
  std::vector<std::pair<double, uint64_t>> phases(size_t(blocks), {0, 0});
  std::vector<TransportRay> rays;
  rays.reserve(BlockPixels);
  for (uint64_t ordinal = 0; ordinal < blocks; ++ordinal) {
    const uint64_t block = o.reverseBlocks ? blocks - 1 - ordinal : ordinal,
                   first = block * BlockPixels;
    rays.clear();
    for (uint64_t id = first; id < std::min(first + BlockPixels, count); ++id)
      rays.push_back(view.ray(id, o.width, o.height));
    auto options = p;
    options.seed = mix(p.seed ^ mix(block));
    options.maxWork = p.maxWork - preparation - out.diagnostics.work;
    options.maxInputBytes = std::min<uint64_t>(p.maxInputBytes, o.maxImageBytes - storage - Fixed);
    auto result = trace(rays, options);
    if (result.rays.size() != rays.size() || result.paths != mul(rays.size(), p.samplesPerRay))
      throw std::runtime_error("transport image backend returned inconsistent path/result counts");
    counter(out.diagnostics.work, result.work);
    if (add(preparation, out.diagnostics.work) > p.maxWork)
      throw std::runtime_error("transport image backend exceeded aggregate work");
    for (size_t i = 0; i < rays.size(); ++i)
      out.pixels[size_t(first) + i] = result.rays[i];
    phases[size_t(block)] = {result.meanScatteringCosine, result.phaseSamples};
    counter(out.diagnostics.paths, result.paths);
    counter(out.diagnostics.scatteringEvents, result.scatteringEvents);
    counter(out.diagnostics.nullCollisions, result.nullCollisions);
    counter(out.diagnostics.shadowProposals, result.shadowProposals);
    counter(out.diagnostics.phaseSamples, result.phaseSamples);
    counter(out.diagnostics.truncatedPaths, result.truncatedPaths);
    counter(out.diagnostics.rouletteTerminations, result.rouletteTerminations);
    if constexpr (requires { result.shadowRouletteTerminations; })
      counter(out.diagnostics.shadowRouletteTerminations, result.shadowRouletteTerminations);
  }
  uint64_t phasesSeen = 0;
  for (const auto &[mean, count] : phases)
    if (count) {
      const double n = double(phasesSeen), m = double(count);
      out.diagnostics.meanScatteringCosine +=
          (mean - out.diagnostics.meanScatteringCosine) * (m / (n + m));
      counter(phasesSeen, count);
    }
  return out;
}
} // namespace container::geometry::vdb::image_detail
