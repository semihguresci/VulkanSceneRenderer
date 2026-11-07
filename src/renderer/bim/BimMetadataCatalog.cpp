#include "Container/renderer/bim/BimMetadataCatalog.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace container::renderer {
namespace {

uint32_t semanticIdFromZeroBased(uint32_t id) {
  return id == std::numeric_limits<uint32_t>::max() ? 0u : id + 1u;
}

template <class LabelMap>
uint32_t semanticIdFromLabel(std::string_view label, const LabelMap &ids) {
  if (label.empty()) {
    return 0u;
  }
  const auto it = ids.find(label);
  if (it == ids.end()) {
    return 0u;
  }
  return semanticIdFromZeroBased(it->second);
}

} // namespace

std::string_view bimMetadataStoreyLabel(const BimElementMetadataView &metadata) {
  if (!metadata.storeyName.empty()) {
    return metadata.storeyName;
  }
  return metadata.storeyId;
}

std::string_view bimMetadataMaterialLabel(const BimElementMetadataView &metadata) {
  if (!metadata.materialName.empty()) {
    return metadata.materialName;
  }
  return metadata.materialCategory;
}

void BimMetadataCatalog::clear() {
  clearLabels();
  modelUnitMetadata_ = {};
  modelGeoreferenceMetadata_ = {};
}

void BimMetadataCatalog::clearLabels() {
  types_.clear();
  storeys_.clear();
  materials_.clear();
  disciplines_.clear();
  phases_.clear();
  fireRatings_.clear();
  loadBearingValues_.clear();
  statuses_.clear();
  storeyRanges_.clear();

  typeIds_.clear();
  storeyIds_.clear();
  materialIds_.clear();
  disciplineIds_.clear();
  phaseIds_.clear();
  fireRatingIds_.clear();
  loadBearingIds_.clear();
  statusIds_.clear();
  storeyRangeIndices_.clear();
}

void BimMetadataCatalog::reserve(size_t objectCount) {
  constexpr size_t maxInitialUniqueLabels = 128u;
  const size_t uniqueLabelHint = std::min(objectCount, maxInitialUniqueLabels);
  typeIds_.reserve(uniqueLabelHint);
  storeyIds_.reserve(uniqueLabelHint);
  materialIds_.reserve(uniqueLabelHint);
  disciplineIds_.reserve(uniqueLabelHint);
  phaseIds_.reserve(uniqueLabelHint);
  fireRatingIds_.reserve(uniqueLabelHint);
  loadBearingIds_.reserve(uniqueLabelHint);
  statusIds_.reserve(uniqueLabelHint);
  storeyRangeIndices_.reserve(uniqueLabelHint);
}

void BimMetadataCatalog::setModelUnitMetadata(BimModelUnitMetadata metadata) {
  modelUnitMetadata_ = std::move(metadata);
}

void BimMetadataCatalog::setModelGeoreferenceMetadata(
    BimModelGeoreferenceMetadata metadata) {
  modelGeoreferenceMetadata_ = std::move(metadata);
}

const BimModelUnitMetadata &BimMetadataCatalog::modelUnitMetadata() const {
  return modelUnitMetadata_;
}

const BimModelGeoreferenceMetadata &
BimMetadataCatalog::modelGeoreferenceMetadata() const {
  return modelGeoreferenceMetadata_;
}

const std::vector<std::string> &BimMetadataCatalog::types() const {
  return types_;
}

const std::vector<std::string> &BimMetadataCatalog::storeys() const {
  return storeys_;
}

const std::vector<std::string> &BimMetadataCatalog::materials() const {
  return materials_;
}

const std::vector<std::string> &BimMetadataCatalog::disciplines() const {
  return disciplines_;
}

const std::vector<std::string> &BimMetadataCatalog::phases() const {
  return phases_;
}

const std::vector<std::string> &BimMetadataCatalog::fireRatings() const {
  return fireRatings_;
}

const std::vector<std::string> &BimMetadataCatalog::loadBearingValues() const {
  return loadBearingValues_;
}

const std::vector<std::string> &BimMetadataCatalog::statuses() const {
  return statuses_;
}

const std::vector<BimStoreyRange> &BimMetadataCatalog::storeyRanges() const {
  return storeyRanges_;
}

uint32_t BimMetadataCatalog::registerUniqueLabel(
    LabelIds &ids, std::vector<std::string> &values, std::string_view label) {
  if (label.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }
  if (const auto existing = ids.find(label); existing != ids.end()) {
    return existing->second;
  }
  if (ids.size() >= std::numeric_limits<uint32_t>::max()) {
    return std::numeric_limits<uint32_t>::max();
  }
  const uint32_t id = static_cast<uint32_t>(ids.size());
  values.emplace_back(label);
  try {
    ids.try_emplace(values.back(), id);
  } catch (...) {
    values.pop_back();
    throw;
  }
  return id;
}

uint32_t BimMetadataCatalog::registerType(std::string_view label) {
  return registerUniqueLabel(typeIds_, types_, label);
}

void BimMetadataCatalog::registerStorey(const BimElementMetadataView &metadata,
                                        const BimElementBounds &bounds) {
  const std::string_view label = bimMetadataStoreyLabel(metadata);
  if (label.empty()) {
    return;
  }
  registerUniqueLabel(storeyIds_, storeys_, label);
  if (!bounds.valid) {
    return;
  }

  const auto existingRange = storeyRangeIndices_.find(label);
  if (existingRange == storeyRangeIndices_.end()) {
    storeyRangeIndices_.try_emplace(std::string(label), storeyRanges_.size());
    storeyRanges_.push_back(BimStoreyRange{
        .label = std::string(label),
        .minElevation = bounds.min.y,
        .maxElevation = bounds.max.y,
        .objectCount = 1u,
    });
    return;
  }

  BimStoreyRange &range = storeyRanges_[existingRange->second];
  range.minElevation = std::min(range.minElevation, bounds.min.y);
  range.maxElevation = std::max(range.maxElevation, bounds.max.y);
  ++range.objectCount;
}

void BimMetadataCatalog::registerMaterial(const BimElementMetadataView &metadata) {
  registerUniqueLabel(materialIds_, materials_,
                      bimMetadataMaterialLabel(metadata));
}

void BimMetadataCatalog::registerDiscipline(std::string_view label) {
  registerUniqueLabel(disciplineIds_, disciplines_, label);
}

void BimMetadataCatalog::registerPhase(std::string_view label) {
  registerUniqueLabel(phaseIds_, phases_, label);
}

void BimMetadataCatalog::registerFireRating(std::string_view label) {
  registerUniqueLabel(fireRatingIds_, fireRatings_, label);
}

void BimMetadataCatalog::registerLoadBearing(std::string_view label) {
  registerUniqueLabel(loadBearingIds_, loadBearingValues_, label);
}

void BimMetadataCatalog::registerStatus(std::string_view label) {
  registerUniqueLabel(statusIds_, statuses_, label);
}

void BimMetadataCatalog::sortStoreyRanges() {
  std::ranges::sort(storeyRanges_,
                    [](const BimStoreyRange &lhs, const BimStoreyRange &rhs) {
                      if (lhs.minElevation == rhs.minElevation) {
                        return lhs.label < rhs.label;
                      }
                      return lhs.minElevation < rhs.minElevation;
                    });
}

uint32_t
BimMetadataCatalog::semanticIdForCategory(BimMetadataSemanticCategory category,
                                          std::string_view label) const {
  switch (category) {
  case BimMetadataSemanticCategory::Type:
    return semanticIdFromLabel(label, typeIds_);
  case BimMetadataSemanticCategory::Storey:
    return semanticIdFromLabel(label, storeyIds_);
  case BimMetadataSemanticCategory::Material:
    return semanticIdFromLabel(label, materialIds_);
  case BimMetadataSemanticCategory::Discipline:
    return semanticIdFromLabel(label, disciplineIds_);
  case BimMetadataSemanticCategory::Phase:
    return semanticIdFromLabel(label, phaseIds_);
  case BimMetadataSemanticCategory::FireRating:
    return semanticIdFromLabel(label, fireRatingIds_);
  case BimMetadataSemanticCategory::LoadBearing:
    return semanticIdFromLabel(label, loadBearingIds_);
  case BimMetadataSemanticCategory::Status:
    return semanticIdFromLabel(label, statusIds_);
  }
  return 0u;
}

uint32_t
BimMetadataCatalog::semanticIdForMetadata(const BimElementMetadataView &metadata,
                                          BimSemanticColorMode mode) const {
  switch (mode) {
  case BimSemanticColorMode::Type:
  case BimSemanticColorMode::Off:
    return semanticIdFromZeroBased(metadata.semanticTypeId);
  case BimSemanticColorMode::Storey:
    return semanticIdForCategory(BimMetadataSemanticCategory::Storey,
                                 bimMetadataStoreyLabel(metadata));
  case BimSemanticColorMode::Material:
    return semanticIdForCategory(BimMetadataSemanticCategory::Material,
                                 bimMetadataMaterialLabel(metadata));
  case BimSemanticColorMode::FireRating:
    return semanticIdForCategory(BimMetadataSemanticCategory::FireRating,
                                 metadata.fireRating);
  case BimSemanticColorMode::LoadBearing:
    return semanticIdForCategory(BimMetadataSemanticCategory::LoadBearing,
                                 metadata.loadBearing);
  case BimSemanticColorMode::Status:
    return semanticIdForCategory(BimMetadataSemanticCategory::Status,
                                 metadata.status);
  }
  return 0u;
}

BimVisibilityGpuObjectMetadata BimMetadataCatalog::visibilityGpuMetadata(
    const BimElementMetadataView &metadata) const {
  BimVisibilityGpuObjectMetadata gpuMetadata{};
  gpuMetadata.semanticIds = {
      semanticIdFromZeroBased(metadata.semanticTypeId),
      semanticIdForCategory(BimMetadataSemanticCategory::Storey,
                            bimMetadataStoreyLabel(metadata)),
      semanticIdForCategory(BimMetadataSemanticCategory::Material,
                            bimMetadataMaterialLabel(metadata)),
      semanticIdForCategory(BimMetadataSemanticCategory::Discipline,
                            metadata.discipline),
  };
  gpuMetadata.propertyIds = {
      semanticIdForCategory(BimMetadataSemanticCategory::Phase, metadata.phase),
      semanticIdForCategory(BimMetadataSemanticCategory::FireRating,
                            metadata.fireRating),
      semanticIdForCategory(BimMetadataSemanticCategory::LoadBearing,
                            metadata.loadBearing),
      semanticIdForCategory(BimMetadataSemanticCategory::Status,
                            metadata.status),
  };
  gpuMetadata.identity = {
      metadata.productIdentityId,
      metadata.objectIndex,
      static_cast<uint32_t>(metadata.geometryKind),
      0u,
  };
  return gpuMetadata;
}

} // namespace container::renderer
