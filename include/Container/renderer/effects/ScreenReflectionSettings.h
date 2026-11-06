#pragma once

#include <cstdint>

namespace container::renderer {

enum class ScreenReflectionMode : uint32_t {
  Off,
  Low,
  High,
  Confidence,
  Distance,
  Source,
};

}  // namespace container::renderer
