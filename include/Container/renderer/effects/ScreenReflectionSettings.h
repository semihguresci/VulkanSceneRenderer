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
  HitUv,
  MissReason,
};

constexpr bool screenReflectionModeUsesHistory(ScreenReflectionMode mode) {
  return mode == ScreenReflectionMode::Low || mode == ScreenReflectionMode::High ||
         mode == ScreenReflectionMode::History;
}

constexpr bool screenReflectionModeIsTraceDiagnostic(ScreenReflectionMode mode) {
  return mode == ScreenReflectionMode::HitUv || mode == ScreenReflectionMode::MissReason;
}

constexpr uint32_t screenReflectionStepLimit(ScreenReflectionMode mode) {
  switch (mode) {
    case ScreenReflectionMode::Off: return 0u;
    case ScreenReflectionMode::Low: return 24u;
    default: return 72u;
  }
}

constexpr uint32_t screenReflectionDebugView(ScreenReflectionMode mode) {
  switch (mode) {
    case ScreenReflectionMode::Confidence: return 1u;
    case ScreenReflectionMode::Distance: return 2u;
    case ScreenReflectionMode::Source: return 3u;
    case ScreenReflectionMode::History: return 4u;
    case ScreenReflectionMode::HitUv: return 5u;
    case ScreenReflectionMode::MissReason: return 6u;
    default: return 0u;
  }
}

}  // namespace container::renderer
