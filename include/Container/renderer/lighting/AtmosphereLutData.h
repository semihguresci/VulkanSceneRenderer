#pragma once

#include <array>
#include <cstddef>

namespace container::renderer {
// Storage shared by sky authoring and the shader-visible lighting buffer.
inline constexpr std::size_t kAtmosphereLutWidth = 16;
inline constexpr std::size_t kAtmosphereLutHeight = 8;
using AtmosphereLut =
    std::array<std::array<float, 4>, kAtmosphereLutWidth * kAtmosphereLutHeight>;
static_assert(sizeof(AtmosphereLut) == 2048);
} // namespace container::renderer
