#pragma once

#include <cstdint>
#include <array>

namespace container::renderer {

enum class ToneMapper : uint32_t {
  Aces = 0, Reinhard = 1, Clipping = 2, Filmic = 3, Uncharted = 4,
  AgX = 5, KhronosNeutral = 6,
};

struct DisplayGrade {
  ToneMapper toneMapper{ToneMapper::Aces};
  float saturation{1.0f};
  float contrast{1.0f};
  float temperatureKelvin{6506.11144f};
  float tintDuv{0.00325895312f};
  float vibrance{0.0f};
  std::array<float, 3> shadowTone{1.0f, 1.0f, 1.0f};
  std::array<float, 3> highlightTone{1.0f, 1.0f, 1.0f};
};

} // namespace container::renderer
