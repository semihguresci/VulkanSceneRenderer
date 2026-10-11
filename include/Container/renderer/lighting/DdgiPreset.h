#pragma once

#include "Container/renderer/lighting/DdgiSettings.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace container::renderer {

inline constexpr size_t kMaxDdgiPresetBytes = 64 * 1024;
inline constexpr size_t kMaxDdgiSceneIdentityBytes = 4096;

// Presets contain configuration only; probe history is rebuilt on application.
// The identity includes both providers and import scales because volume
// coordinates are world-space. Explicit load must match the current scene.
struct DdgiPreset {
  std::string sceneIdentity;
  DdgiSettings settings{};
  bool operator==(const DdgiPreset &) const = default;
};

// Versioned, strict JSON. Missing/unknown/duplicate fields, oversized input,
// invalid types and invalid settings throw invalid_argument. Parsing returns a
// complete value, so failed loading never partially modifies live settings.
[[nodiscard]] std::string encodeDdgiPreset(const DdgiPreset &preset);
[[nodiscard]] DdgiPreset decodeDdgiPreset(std::string_view json);
[[nodiscard]] DdgiPreset loadDdgiPreset(const std::filesystem::path &path);
// Validates before writing and replaces an existing file atomically through a
// unique sibling temporary file. Failed writes leave the previous file intact.
void saveDdgiPreset(const std::filesystem::path &path,
                    const DdgiPreset &preset);

[[nodiscard]] std::string
ddgiSceneIdentity(const std::filesystem::path &primaryPath,
                  const std::filesystem::path &auxiliaryPath = {},
                  float primaryScale = 1.f, float auxiliaryScale = 1.f);
// User-local configuration storage is stable across working directories and
// does not require writing beside an installed executable.
[[nodiscard]] std::filesystem::path ddgiDefaultPresetDirectory();
// Stable hash of the complete identity; filenames never incorporate a model
// path, so unusual names cannot escape the selected storage directory.
[[nodiscard]] std::filesystem::path
ddgiScenePresetPath(std::string_view sceneIdentity,
                    const std::filesystem::path &storageDirectory =
                        ddgiDefaultPresetDirectory());

} // namespace container::renderer
