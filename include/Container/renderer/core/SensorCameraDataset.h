#pragma once

#include "Container/renderer/core/SceneDataCapture.h"
#include "Container/robotics/BimSensorLabels.h"
#include "Container/robotics/SensorCamera.h"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace container::renderer {

struct EncodedSensorCameraFrame {
  struct FreeExr { void operator()(unsigned char* bytes) const noexcept; };
  std::unique_ptr<unsigned char, FreeExr> exr;
  size_t exrSize{};
  std::string metadataJson;

  [[nodiscard]] std::string_view exrBytes() const noexcept {
    return exr ? std::string_view(reinterpret_cast<const char*>(exr.get()), exrSize) : std::string_view{};
  }
};

// Inputs are aligned, top-left-origin host snapshots. RGBA is scene-linear
// FLOAT; alpha is ignored. Surface depths are already axial metric values.
// objectLabels is indexed by the low-field GPU object slot (pick value - 1),
// not by a source element. Every nonzero surface must have a valid BIM label.
// Output channels B/G/R/Z are FLOAT; InstanceID/SemanticID are exact UINT.
// Optional distortion pulls from the ideal inputs, using bilinear RGB and
// nearest surface/labels at the same coordinate. Invalid depth/labels are zero.
[[nodiscard]] EncodedSensorCameraFrame encodeSensorCameraFrame(
    std::span<const float> rgba, std::span<const SceneDataCapturePixel> surfaces,
    std::span<const robotics::BimSensorLabel> objectLabels,
    const robotics::SensorCameraSettings& settings, const robotics::SensorCameraFrame& frame);

} // namespace container::renderer
