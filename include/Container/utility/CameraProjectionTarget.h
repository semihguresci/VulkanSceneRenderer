#pragma once

#include <cstdint>

namespace container::scene {

// Weak editor intent. No field grants ownership or camera replay authority.
struct CameraProjectionTarget {
  uint64_t asset{}, sourceNamespace{}, revision{}, graphIdentity{}, graphRevision{}, incarnation{};
  uint32_t graphNode{UINT32_MAX};
  int32_t sourceNode{-1}, authoredCamera{-1}, evaluatedCamera{-1};
  bool operator==(const CameraProjectionTarget&) const noexcept = default;
};

} // namespace container::scene
