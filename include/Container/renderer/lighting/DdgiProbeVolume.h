#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/vec3.hpp>
#include <optional>
#include <span>

namespace container::renderer {

// Axis-aligned world-space lattice in the renderer's right-handed +Y-up basis.
// origin is the first probe center, not a cell corner; the inclusive coverage
// ends at origin + spacing * (counts - 1). All axes require positive spacing.
struct DdgiProbeVolumeDesc {
  glm::vec3 origin{0.f};
  glm::vec3 spacing{1.f};
  glm::uvec3 counts{1u};
  bool operator==(const DdgiProbeVolumeDesc &) const = default;
};

enum class DdgiOutsideVolume { Reject, Clamp };

struct DdgiTrilinearSample {
  // Corner bits are X=1, Y=2, Z=4. Singleton axes repeat the same probe and
  // assign zero weight to the upper corner. Weights sum to one within float
  // rounding tolerance; they are geometric weights, not visibility weights.
  std::array<uint32_t, 8> probeIndices{};
  std::array<float, 8> weights{};
  bool clamped = false;
};

class DdgiProbeVolume {
public:
  // Throws invalid_argument for nonfinite/collapsed coordinates, zero counts,
  // or a lattice exceeding the uint32 GPU index domain.
  explicit DdgiProbeVolume(const DdgiProbeVolumeDesc &desc);

  [[nodiscard]] const DdgiProbeVolumeDesc &description() const noexcept {
    return desc_;
  }
  [[nodiscard]] uint32_t probeCount() const noexcept { return probeCount_; }
  [[nodiscard]] glm::vec3 minimum() const noexcept { return desc_.origin; }
  [[nodiscard]] glm::vec3 maximum() const noexcept { return maximum_; }
  // X-fast: x + countX * (y + countY * z). Reject out-of-range indices.
  [[nodiscard]] uint32_t linearIndex(glm::uvec3 coordinates) const;
  [[nodiscard]] glm::uvec3 coordinates(uint32_t index) const;
  // Centers are rounded to float from a double multiply/add. Interpolation
  // uses these same actual centers, including when a large origin makes the
  // rounded spacing differ slightly between adjacent probes.
  [[nodiscard]] glm::vec3 probePosition(uint32_t index) const;
  [[nodiscard]] bool contains(glm::vec3 position) const noexcept;
  // Nonfinite queries are invalid even with Clamp. Reject returns nullopt
  // outside inclusive coverage; Clamp explicitly projects onto that coverage.
  [[nodiscard]] std::optional<DdgiTrilinearSample>
  interpolate(glm::vec3 position,
              DdgiOutsideVolume outside = DdgiOutsideVolume::Reject) const;

private:
  DdgiProbeVolumeDesc desc_;
  glm::vec3 maximum_{};
  uint32_t probeCount_ = 0;
};

enum class DdgiProbeClassification : uint32_t {
  Uninitialized = 0,
  Active = 1,
  Inactive = 2,
};

// Explicit 32-byte future shader storage contract; relocation stays in world
// space. Classification is separate from age/validity, so uninitialized probe
// data cannot accidentally become an indirect-light contribution.
struct alignas(16) DdgiProbeState {
  std::array<float, 3> relocation{};
  DdgiProbeClassification classification =
      DdgiProbeClassification::Uninitialized;
  uint32_t lastUpdatedFrame = 0;
  uint32_t validRayCount = 0;
  uint32_t flags = 0;
  // Runtime content epoch; zero remains valid for pure CPU contract fixtures.
  uint32_t reserved = 0;
};

// Scratch payload only: RGB radiance + hit distance, world-space ray direction
// + reserved. Generation, tracing and blending are subsequent implementation.
struct alignas(16) DdgiRaySample {
  std::array<float, 4> radianceAndDistance{};
  std::array<float, 4> directionAndReserved{};
};

struct alignas(16) DdgiProbeUpdate {
  uint32_t probeIndex = 0;
  uint32_t firstRay = 0;
  uint32_t rayCount = 0;
  // Runtime owner fills the content epoch after scheduling. The planner emits
  // 0.
  uint32_t reserved = 0;
};

static_assert(sizeof(DdgiProbeState) == 32);
static_assert(sizeof(DdgiRaySample) == 32);
static_assert(sizeof(DdgiProbeUpdate) == 16);
static_assert(offsetof(DdgiProbeState, classification) == 12);
static_assert(offsetof(DdgiProbeState, lastUpdatedFrame) == 16);
static_assert(offsetof(DdgiRaySample, directionAndReserved) == 16);

struct DdgiUpdateBudget {
  uint32_t maxProbesPerFrame = 128;
  uint32_t raysPerProbe = 128;
  uint32_t maxRaysPerFrame = 16384;
  bool operator==(const DdgiUpdateBudget &) const = default;
};

// Deterministic round-robin schedule. No partial probe ray sets are emitted;
// either zero budget disables updates without advancing the cursor. Consumers
// must persist nextProbe only after submission succeeds.
struct DdgiUpdateBatch {
  uint32_t volumeProbeCount = 0;
  uint32_t firstProbe = 0;
  uint32_t probeCount = 0;
  uint32_t raysPerProbe = 0;
  uint32_t rayCount = 0;
  uint32_t nextProbe = 0;
};

[[nodiscard]] DdgiUpdateBatch planDdgiUpdates(const DdgiProbeVolume &volume,
                                              const DdgiUpdateBudget &budget,
                                              uint32_t firstProbe = 0);
// Caller-owned storage: writes exactly batch.probeCount records, preserves any
// suffix, and rejects malformed batches or insufficient spans before writing.
void writeDdgiProbeUpdates(const DdgiUpdateBatch &batch,
                           std::span<DdgiProbeUpdate> destination);

struct DdgiResourceSettings {
  // Octahedral directional maps: one square tile per probe, one texel of
  // wrap/seam border on every edge. Interiors exclude that border.
  uint32_t irradianceInteriorTexels = 6;
  uint32_t visibilityInteriorTexels = 14;
  DdgiUpdateBudget updates{};
  bool operator==(const DdgiResourceSettings &) const = default;
};

struct DdgiResourceLimits {
  uint32_t maxImageDimension2D = 4096;
  // Future GPU owner supplies min(maxStorageBufferRange, configured budget).
  uint64_t maxBufferBytes = 64ull * 1024 * 1024;
  uint64_t maxPayloadBytes = 256ull * 1024 * 1024;
};

struct DdgiAtlasPlan {
  uint32_t interiorTexels = 0;
  uint32_t tileTexels = 0;
  uint32_t tilesPerRow = 0;
  uint32_t tileRows = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t bytesPerHistory = 0;
};

struct DdgiResourcePlan {
  // Independent, single-layer 2D atlases, row-major tiles by linear probe
  // index. Irradiance is RGBA16F (RGB irradiance + reserved), visibility RG32F
  // (first/second distance moments): both 8 bytes/texel, no mipmaps.
  // Two history copies support serialized ping-pong updates. They are not
  // multiplied by frames-in-flight; future GPU integration must synchronize
  // all readers/writers before reusing either copy.
  static constexpr uint32_t historyCopies = 2;
  static constexpr uint32_t borderTexels = 1;
  DdgiAtlasPlan irradiance{};
  DdgiAtlasPlan visibility{};
  DdgiUpdateBatch updateCapacity{};
  uint64_t probeStateBytes = 0;
  uint64_t probePositionBytes = 0;
  uint64_t rayScratchBytes = 0;
  uint64_t updateRecordBytes = 0;
  // Includes rounded atlas padding + both histories + all four buffers.
  // Payload-only: excludes Vulkan alignment, driver metadata, staging and
  // other renderer allocations. Actual VMA allocation limits must be checked
  // by the resource owner before allocating/uploading in a later stage.
  uint64_t payloadBytes = 0;
};

// Allocation-free planning. Throws invalid_argument for unsupported settings
// or overflow/limit violations, with the failing field/resource in the error.
[[nodiscard]] DdgiResourcePlan
planDdgiResources(const DdgiProbeVolume &volume,
                  const DdgiResourceSettings &settings = {},
                  const DdgiResourceLimits &limits = {});
// Packed collections use the same atlases/buffers and one shared ray budget.
[[nodiscard]] DdgiResourcePlan
planDdgiResources(uint32_t probeCount,
                  const DdgiResourceSettings &settings = {},
                  const DdgiResourceLimits &limits = {});

} // namespace container::renderer
