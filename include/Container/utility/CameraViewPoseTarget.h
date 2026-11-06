#pragma once

#include <Container/utility/CameraProjectionTarget.h>

namespace container::scene {

// A camera definition may be shared by different source nodes. Pose commands
// additionally identify the graph node's original lifetime; numeric slots and
// equal payload are insufficient after a cut history branch is replaced.
struct CameraViewPoseTarget {
  CameraProjectionTarget camera;
  uint64_t nodeIncarnation{};
  bool operator==(const CameraViewPoseTarget&) const noexcept = default;
};

} // namespace container::scene
