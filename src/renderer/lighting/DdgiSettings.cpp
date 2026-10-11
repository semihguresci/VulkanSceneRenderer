#include "Container/renderer/lighting/DdgiSettings.h"
#include "Container/renderer/lighting/DdgiVolumeSet.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace container::renderer {
namespace {

void finiteRange(float value, float minimum, float maximum, const char *name) {
  if (!std::isfinite(value) || value < minimum || value > maximum)
    throw std::invalid_argument(
        std::string("DDGI ") + name + " must be finite and in [" +
        std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
}

} // namespace

void validateDdgiSettings(const DdgiSettings &settings) {
  const DdgiVolumeSet volumes(settings);
  if (settings.resources.updates.raysPerProbe == 0 ||
      settings.resources.updates.raysPerProbe > kMaxDdgiRaysPerProbe)
    throw std::invalid_argument("DDGI raysPerProbe must be in [1,4096]");
  if (!std::isfinite(settings.maxDistance) || settings.maxDistance <= 0.f ||
      settings.maxDistance > kMaxDdgiTraceDistance)
    throw std::invalid_argument(
        "DDGI maxDistance must be finite and in (0,1000000]");
  finiteRange(settings.hysteresis, 0.f, .99f, "hysteresis");
  finiteRange(settings.normalBias, 0.f, settings.maxDistance, "normalBias");
  finiteRange(settings.viewBias, 0.f, settings.maxDistance, "viewBias");
  finiteRange(settings.visibilityBias, 0.f, settings.maxDistance,
              "visibilityBias");
  finiteRange(settings.intensity, 0.f, kMaxDdgiIntensity, "intensity");
  if (static_cast<uint32_t>(settings.debugView) >
      static_cast<uint32_t>(DdgiDebugView::Relocation))
    throw std::invalid_argument("DDGI debugView must be in [0,4]");
  if (!std::isfinite(settings.debugProbeRadius) ||
      settings.debugProbeRadius <= 0.f ||
      settings.debugProbeRadius > kMaxDdgiTraceDistance)
    throw std::invalid_argument(
        "DDGI debugProbeRadius must be finite and in (0,1000000]");
  if (settings.payloadBudgetBytes == 0)
    throw std::invalid_argument("DDGI payloadBudgetBytes must be positive");
  if (settings.surfaceBudgetBytes == 0)
    throw std::invalid_argument("DDGI surfaceBudgetBytes must be positive");

  DdgiResourceLimits limits;
  limits.maxImageDimension2D = std::numeric_limits<uint32_t>::max();
  limits.maxBufferBytes = std::numeric_limits<uint64_t>::max();
  limits.maxPayloadBytes = settings.payloadBudgetBytes;
  (void)planDdgiResources(volumes.probeCount(), settings.resources, limits);
}

} // namespace container::renderer
