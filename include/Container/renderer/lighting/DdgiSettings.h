#pragma once

#include "Container/renderer/lighting/DdgiProbeVolume.h"

#include <cstdint>
#include <vector>

namespace container::renderer {

inline constexpr uint32_t kMaxDdgiRaysPerProbe = 4096;
inline constexpr float kMaxDdgiTraceDistance = 1000000.f;
inline constexpr float kMaxDdgiIntensity = 10000.f;
inline constexpr uint32_t kMaxDdgiVolumes = 4;

enum class DdgiDebugView : uint32_t {
  Off = 0,
  Irradiance = 1,
  Visibility = 2,
  Classification = 3,
  Relocation = 4,
};

// DDGI is optional and defaults off. Origin and all biases are in world-space
// units in the renderer's right-handed +Y-up basis. Lattice or ray-quality
// changes recreate history; live transport changes refresh a bounded sweep.
struct DdgiSettings {
  bool enabled = false;
  // Retains configured budgets and submitted history while stopping updates.
  bool paused = false;
  // Inclusive probe-center coverage is only a small scene default. Fit origin,
  // spacing and counts to the receiver bounds of each scene (including floors).
  DdgiProbeVolumeDesc volume{{-1.5f, -.1f, -1.5f}, {.5f, .5f, .5f}, {7, 5, 7}};
  // All volumes share one atlas and the frame/payload budgets. Finer cells
  // take precedence in overlaps and fade into coarser valid support at edges.
  std::vector<DdgiProbeVolumeDesc> additionalVolumes{};
  float volumeBlendCells = 1.f;
  // Opt-in bounded, straight-through attenuation for thin glass. Thick glass,
  // refraction and transmissive receivers retain their existing fallback.
  bool thinGlassTransmission = false;
  // Opt-in rough/solid dielectric secondary transport. Primary transparent
  // receivers continue to use their existing raster route.
  bool dielectricTransmission = false;
  // Keep the primary lattice centered near the camera on whole-cell steps;
  // overlapping world-space nodes retain history, exposed cells start empty.
  bool cameraFollow = false;
  // Use a second same-family compute queue when available. Otherwise retain
  // the synchronous Vulkan path and report the capability fallback.
  bool asyncCompute = false;
  DdgiResourceSettings resources{};
  float maxDistance = 20.f;
  float hysteresis = .95f;
  float normalBias = .03f;
  float viewBias = .01f;
  float visibilityBias = .05f;
  float intensity = 1.f;
  DdgiDebugView debugView = DdgiDebugView::Off;
  float debugProbeRadius = .06f;
  // Payload-only ceilings, separate from device limits and actual Vulkan/VMA
  // allocations. The surface budget covers ray-query hit-surface geometry.
  uint64_t payloadBudgetBytes = 64ull * 1024 * 1024;
  uint64_t surfaceBudgetBytes = 128ull * 1024 * 1024;
  bool operator==(const DdgiSettings &) const = default;
};

// CPU-only validation, including when enabled is false. Invalid explicit
// inputs throw invalid_argument instead of being clamped or hiding errors by
// disabling DDGI. Zero frame budgets pause updates; raysPerProbe stays
// positive. Resource planning checks the configured payload cap without
// imposing a synthetic GPU's image/buffer limits; the owner validates real
// device limits.
void validateDdgiSettings(const DdgiSettings &settings);

} // namespace container::renderer
