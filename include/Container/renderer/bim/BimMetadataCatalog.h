#pragma once

#include "Container/renderer/bim/BimManager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace container::renderer {

enum class BimMetadataSemanticCategory : uint8_t {
  Type,
  Storey,
  Material,
  Discipline,
  Phase,
  FireRating,
  LoadBearing,
  Status,
};

class BimMetadataCatalog {
public:
  void clear();
  void clearLabels();
  // Object counts are only a bounded startup hint: label storage grows with
  // unique labels rather than reserving one dictionary slot per draw object.
  void reserve(size_t objectCount);

  void setModelUnitMetadata(BimModelUnitMetadata metadata);
  void setModelGeoreferenceMetadata(BimModelGeoreferenceMetadata metadata);

  [[nodiscard]] const BimModelUnitMetadata &modelUnitMetadata() const;
  [[nodiscard]] const BimModelGeoreferenceMetadata &
  modelGeoreferenceMetadata() const;

  [[nodiscard]] const std::vector<std::string> &types() const;
  [[nodiscard]] const std::vector<std::string> &storeys() const;
  [[nodiscard]] const std::vector<std::string> &materials() const;
  [[nodiscard]] const std::vector<std::string> &disciplines() const;
  [[nodiscard]] const std::vector<std::string> &phases() const;
  [[nodiscard]] const std::vector<std::string> &fireRatings() const;
  [[nodiscard]] const std::vector<std::string> &loadBearingValues() const;
  [[nodiscard]] const std::vector<std::string> &statuses() const;
  [[nodiscard]] const std::vector<BimStoreyRange> &storeyRanges() const;

  [[nodiscard]] uint32_t registerType(std::string_view label);
  void registerStorey(const BimElementMetadataView &metadata,
                      const BimElementBounds &bounds);
  void registerMaterial(const BimElementMetadataView &metadata);
  void registerDiscipline(std::string_view label);
  void registerPhase(std::string_view label);
  void registerFireRating(std::string_view label);
  void registerLoadBearing(std::string_view label);
  void registerStatus(std::string_view label);
  void sortStoreyRanges();

  [[nodiscard]] uint32_t
  semanticIdForCategory(BimMetadataSemanticCategory category,
                        std::string_view label) const;
  [[nodiscard]] uint32_t
  semanticIdForMetadata(const BimElementMetadataView &metadata,
                        BimSemanticColorMode mode) const;
  [[nodiscard]] BimVisibilityGpuObjectMetadata
  visibilityGpuMetadata(const BimElementMetadataView &metadata) const;

private:
  using LabelIds = std::unordered_map<std::string, uint32_t,
                                      BimIdentityStringHash,
                                      BimIdentityStringEqual>;
  uint32_t registerUniqueLabel(LabelIds &ids,
                               std::vector<std::string> &values,
                               std::string_view label);

  std::vector<std::string> types_{};
  std::vector<std::string> storeys_{};
  std::vector<std::string> materials_{};
  std::vector<std::string> disciplines_{};
  std::vector<std::string> phases_{};
  std::vector<std::string> fireRatings_{};
  std::vector<std::string> loadBearingValues_{};
  std::vector<std::string> statuses_{};
  std::vector<BimStoreyRange> storeyRanges_{};

  LabelIds typeIds_{};
  LabelIds storeyIds_{};
  LabelIds materialIds_{};
  LabelIds disciplineIds_{};
  LabelIds phaseIds_{};
  LabelIds fireRatingIds_{};
  LabelIds loadBearingIds_{};
  LabelIds statusIds_{};
  std::unordered_map<std::string, size_t, BimIdentityStringHash,
                     BimIdentityStringEqual> storeyRangeIndices_{};

  BimModelUnitMetadata modelUnitMetadata_{};
  BimModelGeoreferenceMetadata modelGeoreferenceMetadata_{};
};

[[nodiscard]] std::string_view
bimMetadataStoreyLabel(const BimElementMetadataView &metadata);
[[nodiscard]] std::string_view
bimMetadataMaterialLabel(const BimElementMetadataView &metadata);

} // namespace container::renderer
