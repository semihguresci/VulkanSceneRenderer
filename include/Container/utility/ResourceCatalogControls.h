#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace container::ui {
enum class ResourceCatalogDomain : uint8_t { Archive, Evaluated, Runtime };
enum class ResourceCatalogKind : uint8_t { Image, Texture, Sampler, RuntimeTexture };
// Weak catalog intent only. The scene owner resolves the exact current
// incarnation and independently proves every dependency before publication.
struct ResourceCatalogTarget {
  uint64_t assetIdentity{}, resourceRevision{}, domainIdentity{}, incarnation{};
  uint32_t row{UINT32_MAX};
  ResourceCatalogDomain domain{};
  ResourceCatalogKind kind{};
  bool operator==(const ResourceCatalogTarget&) const = default;
};
struct ResourceCatalogChoice { ResourceCatalogTarget target; std::string label; };
struct ResourceCatalogSnapshot {
  uint64_t assetIdentity{}, resourceRevision{};
  std::vector<ResourceCatalogChoice> choices;
  std::optional<ResourceCatalogTarget> selected;
  bool available{}, retirementAvailable{};
  std::string retirementReason, error;
};
struct ResourceCatalogSelectionRequest { ResourceCatalogTarget target; };
struct ResourceRetirementRequest { ResourceCatalogTarget target; };
} // namespace container::ui
