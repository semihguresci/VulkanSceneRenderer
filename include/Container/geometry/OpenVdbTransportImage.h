#pragma once
#include "Container/geometry/OpenVdbTransportReference.h"
#include <filesystem>

namespace container::geometry::vdb {
struct TransportPinholeCamera {
  TransportVector eye{0, 0, 3}, target{0, 0, 0}, up{0, 1, 0};
  double verticalFovDegrees{45}; // [0.1,150], rectilinear pinhole, no lens/near/far plane.
};
struct TransportImageOptions {
  uint32_t width{64}, height{64}; // Each [1,2048], row0 top, X fastest.
  TransportReferenceOptions transport = [] {
    TransportReferenceOptions o;
    o.samplesPerRay = 256;
    return o;
  }();
  uint64_t maxPixels{262144}, maxPaths{16'000'000};
  uint64_t maxImageBytes{512ull * 1024 * 1024};
  // Scheduling only. Canonical64-pixel RNG blocks stay unchanged. Reversing
  // their visit order is useful for verifying deterministic tiled processing.
  bool reverseBlocks{false};
};
struct TransportImageDiagnostics {
  uint64_t preparationWork{}, work{}, paths{}, scatteringEvents{}, nullCollisions{},
      shadowProposals{};
  uint64_t phaseSamples{}, truncatedPaths{}, rouletteTerminations{}, shadowRouletteTerminations{};
  double meanScatteringCosine{};
};
struct TransportImageResult {
  uint32_t width{}, height{}, samplesPerPixel{}, maxScatteringEvents{};
  uint64_t seed{}, retainedSceneBytes{}, maxImageBytes{};
  bool gpu{};
  TransportPinholeCamera camera;
  std::vector<TransportRayEstimate> pixels;
  TransportImageDiagnostics diagnostics;
};
struct TransportImageExportResult {
  uint64_t roundedValues{}, encodedBytes{};
  double maxAbsoluteError{}, maxRelativeError{};
};
// CPU Double spatial transport at the center of every pixel. Samples per ray
// are transport samples; this first camera route does NOT provide subpixel AA.
// Eye/target/up must form a finite nondegenerate camera. World coordinates and
// scene units are the reference estimator's. Pixels use top-to-bottom scanlines.
// SE is unavailable (stored0, explicitly tagged on export) when spp<2.
//
// Fixed64-pixel blocks seeded by imageSeed/globalBlockIndex make each pixel's
// RNG independent of block traversal, scheduling and future tile grouping.
// Changing image dimensions or backend may change the sequence. CPU/GPU RNGs
// differ; compare with statistical uncertainty, not identical noise patterns.
//
// At most262144 pixels,8192spp,16M total paths,1e9 aggregate work. Preparation
// work charges repeated scene validation/copy payloads per block, separately
// from estimator work. All live scene/image/block workspaces share byte caps.
// Logical payloads are bounded, not process RSS/caller vector capacities.
// Any invalid block/work/numeric failure throws without returning an image.
// Finite-order bias and all reference physical limitations remain unchanged.
[[nodiscard]] TransportImageResult RenderTransportImageCpu(const TransportScene &,
                                                           const TransportPinholeCamera & = {},
                                                           const TransportImageOptions & = {});

// Exclusive seekable OpenEXR with six scene-linear FLOAT channels:
// R,G,B,SE.R,SE.G,SE.B. No tone mapping, half conversion or lossy compression.
// CPU Double means/errors stay Double in memory. Conversion toFloat32 rejects
// overflow and nonzero-to-zero underflow; other rounding is reported here and
// recorded in EXR attributes together with camera/seed/spp/order/work counters.
// Input/result, retained scene, planar Float data and serialization workspace
// share min(maxBytes,image.maxImageBytes). Errors leave no new partial output.
[[nodiscard]] TransportImageExportResult
WriteTransportImageExr(const std::filesystem::path &, const TransportImageResult &,
                       uint64_t maxBytes = 512ull * 1024 * 1024);
} // namespace container::geometry::vdb
