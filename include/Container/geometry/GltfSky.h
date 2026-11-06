#pragma once

#include "Container/renderer/lighting/SkySettings.h"

#include <cstddef>
#include <optional>

namespace tinygltf { class Model; }

namespace container::geometry::gltf {

// Optional OMI draft metadata. Unsupported or malformed entries are reported
// to the caller, which can retain the selected environment.
[[nodiscard]] std::optional<renderer::SkySettings>
readAuthoredSky(const tinygltf::Model& model, size_t sceneIndex);

} // namespace container::geometry::gltf
