#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/geometry/OpenVdbTransportReference.h"
#include <memory>

namespace container::gpu {
class VulkanMemoryManager;
}

namespace container::renderer {

// Immutable native data can only be produced by the bounded validated VDB
// loader. Arbitrary serialized NanoVDB bytes or caller-supplied majorants are
// deliberately not accepted by this reference API.
class TransportGpuNativeSource {
public:
  [[nodiscard]] const geometry::OpenVdbVolume &volume() const noexcept { return volume_; }

private:
  geometry::OpenVdbVolume volume_;
  TransportGpuNativeSource() = default;
  friend std::shared_ptr<const TransportGpuNativeSource>
  LoadTransportGpuNativeSource(const std::filesystem::path &,
                               const geometry::OpenVdbVolumeOptions &);
};

[[nodiscard]] std::shared_ptr<const TransportGpuNativeSource>
LoadTransportGpuNativeSource(const std::filesystem::path &input,
                             const geometry::OpenVdbVolumeOptions &options = {});

struct TransportGpuReferenceOptions {
  geometry::vdb::TransportReferenceOptions transport{};
  uint32_t maxWorkPerPath{4096}; // [1,65536], also limited by total maxWork/paths.
};

struct TransportGpuDevice {
  VkPhysicalDevice physicalDevice{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t queueFamily{};
  container::gpu::VulkanMemoryManager *memory{};
};

struct TransportGpuReferenceResult : geometry::vdb::TransportReferenceResult {
  uint64_t shadowRouletteTerminations{}; // Separate from continuation roulette.
};

// Standalone Float32 compute reference, with exactly the CPU reference units
// and estimator. Caller keeps device/queue/memory alive and serializes queue
// access. No surface, renderer, TLAS, descriptors or frame resources are needed.
// trace waits only for its owned submission fence; this is not a frame uploader.
// Six storage buffers and one UBO; no RTX, shaderInt64 or shaderFloat64 required.
//
// Up to four media, 32 directional/point lights, 4096 black triangles/rays,
// scattering order0..16, <=1M total paths. Dense lattices use the CPU contract.
// A nonnull nativeSources[i] replaces ONLY medium i's density lattice: that
// medium's dimensions/bounds must match the factory volume (dense data ignored)
// and its albedo/extinction/emission/HG material remains the caller's. Density
// bounds/maximum/importScale come exclusively from the immutable factory.
// Temperature/area lights/spot lights/surface BSDF/OIT are not implemented here.
//
// Input values, Float32 conversion/underflow, world/index precision, packed
// payload/allocation/dispatch/device limits are checked. Statistical comparison
// with CPU Double is appropriate; RNG is deterministic uint32, not CPU's RNG.
// Nonzero input values must convert to normal Float32 (subnormal inputs are
// rejected, independently of optional shader denorm/flush-to-zero modes).
// Ratio-tracked shadow weights below 1e-4 use unbiased roulette: survival is
// weight/1e-4 quantized to a resolvable uint32 Bernoulli probability, and a
// survivor is divided by that ACTUAL probability. This changes variance, not
// expected transmittance, and prevents artificial tiny null-ratio products
// from underflowing before a bright light could restore their magnitude.
// Per-path work/numeric/majorant failure invalidates the WHOLE operation and
// throws without returning partial estimates. Finite scattering-order tails
// remain explicitly reported in truncatedPaths. Means/SE use scaled moments.
class OpenVdbTransportGpuReference {
public:
  OpenVdbTransportGpuReference(const TransportGpuDevice &device,
                               const std::filesystem::path &spirv);
  ~OpenVdbTransportGpuReference();
  OpenVdbTransportGpuReference(const OpenVdbTransportGpuReference &) = delete;
  OpenVdbTransportGpuReference &operator=(const OpenVdbTransportGpuReference &) = delete;
  [[nodiscard]] TransportGpuReferenceResult
  trace(const geometry::vdb::TransportScene &scene,
        std::span<const geometry::vdb::TransportRay> rays,
        const TransportGpuReferenceOptions &options = {},
        const std::array<std::shared_ptr<const TransportGpuNativeSource>, 4> &nativeSources = {});

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace container::renderer
