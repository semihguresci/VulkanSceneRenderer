#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace container::geometry::gltf {

enum class ExtensionSupport { Supported, Partial, Unsupported };

struct ExtensionCapability {
  std::string_view name;
  ExtensionSupport support;
  std::string_view description;
};

// Describes runtime import/rendering support, not just TinyGLTF JSON parsing.
// Partial entries are usable, with an explicit approximation warning.
[[nodiscard]] std::span<const ExtensionCapability> extensionCapabilities();
[[nodiscard]] const ExtensionCapability* extensionCapability(std::string_view name);

enum class CapabilitySeverity { Warning, Error };

struct CapabilityDiagnostic {
  CapabilitySeverity severity;
  std::string code;
  std::string assetPath;
  std::string message;
};

struct AssetRequirements {
  std::string version;
  std::optional<std::string> minVersion;
  std::vector<std::string> extensionsUsed;
  std::vector<std::string> extensionsRequired;
};

struct CapabilityValidation {
  std::vector<CapabilityDiagnostic> diagnostics;
  [[nodiscard]] bool supported() const;
};

// Supports glTF 2.0. Newer 2.x assets may use their 2.0 fallback only when
// minVersion is absent or supported. A newer required minimum is an error.
[[nodiscard]] CapabilityValidation validateCapabilities(
    const AssetRequirements& requirements, std::string_view assetPath,
    std::span<const std::string> unavailableRuntimeExtensions = {});
[[nodiscard]] std::string formatCapabilityDiagnostic(
    const CapabilityDiagnostic& diagnostic);

}  // namespace container::geometry::gltf
