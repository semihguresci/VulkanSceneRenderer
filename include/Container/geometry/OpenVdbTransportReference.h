#pragma once

#include "Container/geometry/OpenVdbVolume.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace container::geometry::vdb {

using TransportVector = std::array<double, 3>;

// CPU reference inputs, independent of OpenVDB/Vulkan build availability. All
// distances use the same world unit. Density is dimensionless, cell-centered,
// X-fastest, trilinearly interpolated with edge clamping INSIDE the closed box,
// and zero outside. Dimensions are in [1,256]. A single-cell grid is homogeneous.
struct TransportMedium {
  std::array<uint32_t, 3> dimensions{1, 1, 1};
  TransportVector boundsMin{-1.0, -1.0, -1.0}, boundsMax{1.0, 1.0, 1.0};
  std::vector<float> density{1.0f};
  double extinction{1.0};                // Gray extinction / world length, multiplied by density.
  TransportVector albedo{1.0, 1.0, 1.0}; // RGB single-scattering albedo, [0,1].
  double anisotropy{0.0};                // Henyey-Greenstein g in [-0.95,0.95].
  // Independent source j/density: scene-linear radiance / world length
  // (equivalently power / world-volume / steradian). This permits sigma_t=0.
  TransportVector emission{0.0, 0.0, 0.0};
};

// Copies a dense preview's lattice/bounds only; material defaults remain above.
// Native sparse payloads fail rather than being silently omitted or resampled.
[[nodiscard]] TransportMedium MakeTransportMedium(const OpenVdbVolume &volume);

struct TransportRay {
  TransportVector origin{0.0, 0.0, 3.0}, direction{0.0, 0.0, -1.0};
};

enum class TransportLightType { Directional, Point };

struct TransportLight {
  TransportLightType type{TransportLightType::Directional};
  // Directional: direction from a scattering point TOWARD the light; normalized
  // internally. Point: world position. No artificial range/attenuation cutoff.
  TransportVector vector{0.0, 0.0, 1.0};
  // Directional: incident radiance integrated over its delta solid angle.
  // Point: radiant intensity, so the incident delta-light weight is value/r^2.
  TransportVector value{1.0, 1.0, 1.0};
};

struct TransportTriangle {
  TransportVector a{}, b{}, c{};
};

struct TransportScene {
  std::vector<TransportMedium> media;
  std::vector<TransportLight> lights;
  // Two-sided, opaque BLACK surfaces: block camera, continuation and shadow
  // rays. No surface reflection, refraction or emission is modeled.
  std::vector<TransportTriangle> occluders;
  TransportVector environment{0.0, 0.0, 0.0}; // Constant incident radiance.
};

struct TransportReferenceOptions {
  uint32_t samplesPerRay{4096};     // [1,1'000'000].
  uint32_t maxScatteringEvents{16}; // [0,64]; explicit finite-order truncation.
  uint32_t rouletteStart{4};        // [1,65]; >maxScatteringEvents disables roulette.
  uint64_t seed{1};
  // An auxiliary inverse-world-length proposal for independent emission.
  // Used if any source is nonzero; M=max(gray extinction bound, this rate).
  double emissionSamplingRate{1.0}; // [1e-6,1e6].
  uint64_t maxDensityValues{16'000'000};
  // Logical payload/result workspace, not a process-RSS or caller-capacity cap.
  uint64_t maxInputBytes{256ull * 1024ull * 1024ull};
  // Counts paths, tracking proposals, density fetches and triangle tests.
  // Exceeding the cap throws; no partial result is returned. [1,1e9].
  uint64_t maxWork{100'000'000};
};

struct TransportRayEstimate {
  TransportVector radiance{}, standardError{}; // Error unavailable (0) at spp=1.
};

struct TransportReferenceResult {
  std::vector<TransportRayEstimate> rays;
  uint64_t work{}, paths{}, scatteringEvents{}, nullCollisions{}, shadowProposals{};
  uint64_t phaseSamples{};
  double meanScatteringCosine{}; // Diagnostic mean dot(current,next) of HG samples.
  // A nonzero-throughput path encountering its NEXT scattering event beyond
  // the configured order is omitted and counted. This is finite-order bias,
  // not a converged infinite-order transport claim. Roulette has no such bias.
  uint64_t truncatedPaths{}, rouletteTerminations{};
};

// Actual spatial null-collision path tracing, with RGB throughput, medium
// selection proportional to gray extinction, HG continuation, delta-light
// next-event estimates and independent nonnegative ratio-tracked shadows.
// Environment is scored only on escape; emission is scored at proposal points,
// avoiding next-event/path-continuation double counting. g>0 samples the next
// reverse path direction around its CURRENT direction (physical forward HG).
// Overlapping extinction/source coefficients add; medium order changes noise,
// not the transport equation. No local multiple-scattering gain is used.
//
// Limits: <=8 media, <=32 lights, <=4096 triangles/rays, finite input values;
// coordinates <=1e12, coefficients <=1e6, positive grid spacing with >=1024
// representable Double increments. Density <=1e6; albedo [0,1]. Invalid numeric
// progress/overflow/underflow fails explicitly. Input validation is separately
// bounded by density/scene caps. Work is serial and bounded, with no I/O
// or hidden threads. Deterministic seed/scene/ray ordering produces equal bits.
// The finite-order result converges statistically with spp; increasing the
// order is independently required when truncatedPaths is significant.
// Estimator references: PBRT 4e, Volume Scattering / Phase Functions,
// Transmittance, and Light Transport II / Volume Scattering Integrators:
// https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/Volume_Scattering_Integrators
[[nodiscard]] TransportReferenceResult
TraceTransportReference(const TransportScene &scene, std::span<const TransportRay> rays,
                        const TransportReferenceOptions &options = {});

} // namespace container::geometry::vdb
