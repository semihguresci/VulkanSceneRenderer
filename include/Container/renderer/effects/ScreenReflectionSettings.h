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
  History,
};

constexpr bool screenReflectionModeUsesHistory(ScreenReflectionMode mode) {
  return mode == ScreenReflectionMode::Low || mode == ScreenReflectionMode::High ||
         mode == ScreenReflectionMode::History;
}

}  // namespace container::renderer
