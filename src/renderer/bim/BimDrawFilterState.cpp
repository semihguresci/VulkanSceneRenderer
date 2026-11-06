#include "Container/renderer/bim/BimDrawFilterState.h"

#include "Container/renderer/bim/BimMetadataCatalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace container::renderer {
namespace {

[[nodiscard]] const BimElementMetadata *
metadataForObject(std::span<const BimElementMetadata> metadata,
                  uint32_t objectIndex) {
  if (objectIndex >= metadata.size()) {
    return nullptr;
  }
  return &metadata[objectIndex];
}

[[nodiscard]] std::string lowerAscii(std::string_view value) {
  std::string lowered(value);
  std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

[[nodiscard]] bool containsToken(std::string_view value,
                                 std::string_view token) {
  return lowerAscii(value).find(token) != std::string::npos;
}

[[nodiscard]] bool isTokenCharacter(char value) {
  return std::isalnum(static_cast<unsigned char>(value)) != 0;
}

[[nodiscard]] bool containsBoundedToken(std::string_view lowered,
                                        std::string_view token) {
  size_t offset = 0u;
  while ((offset = lowered.find(token, offset)) != std::string_view::npos) {
    const bool hasLeadingBoundary =
        offset == 0u || !isTokenCharacter(lowered[offset - 1u]);
    const size_t afterToken = offset + token.size();
    const bool hasTrailingBoundary =
        afterToken >= lowered.size() || !isTokenCharacter(lowered[afterToken]);
    if (hasLeadingBoundary && hasTrailingBoundary) {
      return true;
    }
    offset = afterToken;
  }
  return false;
}

[[nodiscard]] bool matchesIfcClassToken(std::string_view lowered,
                                        std::string_view token) {
  const std::string ifcPrefix = "ifc" + std::string(token);
  return lowered.starts_with(std::string_view{ifcPrefix}) ||
         containsBoundedToken(lowered, token);
}

[[nodiscard]] bool containsMepClassificationToken(std::string_view value) {
  const std::string lowered = lowerAscii(value);
  return matchesIfcClassToken(lowered, "pipe") ||
         matchesIfcClassToken(lowered, "duct") ||
         matchesIfcClassToken(lowered, "cable") ||
         matchesIfcClassToken(lowered, "service") ||
         containsBoundedToken(lowered, "mep") ||
         containsBoundedToken(lowered, "mechanical") ||
         containsBoundedToken(lowered, "electrical") ||
         containsBoundedToken(lowered, "plumbing") ||
         containsBoundedToken(lowered, "hvac");
}

[[nodiscard]] bool containsAnyMepToken(const BimElementMetadata &metadata) {
  const std::array<std::string_view, 3u> values{{
      metadata.type,
      metadata.objectType,
      metadata.discipline,
  }};
  for (std::string_view value : values) {
    if (containsMepClassificationToken(value)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool metadataMatchesDisciplinePreset(
    const BimElementMetadata &metadata, BimDisciplinePreset preset) {
  switch (preset) {
  case BimDisciplinePreset::None:
    return true;
  case BimDisciplinePreset::Architecture:
    return !containsAnyMepToken(metadata);
  case BimDisciplinePreset::MepXray:
    return containsAnyMepToken(metadata);
  }
  return true;
}

[[nodiscard]] bool phaseIsDemolished(const BimElementMetadata &metadata) {
  return containsToken(metadata.status, "demolish") ||
         containsToken(metadata.phase, "demolish");
}

[[nodiscard]] std::optional<size_t>
phaseIndex(std::span<const std::string> phaseOrder,
           std::string_view phaseLabel) {
  if (phaseLabel.empty()) {
    return std::nullopt;
  }
  const auto found = std::ranges::find(phaseOrder, phaseLabel);
  if (found == phaseOrder.end()) {
    return std::nullopt;
  }
  return static_cast<size_t>(std::distance(phaseOrder.begin(), found));
}

[[nodiscard]] bool metadataMatchesPhaseTimeline(
    const BimElementMetadata &metadata, const BimDrawFilter &filter,
    const BimDrawFilterStateInputs &inputs) {
  if (!filter.phaseTimelineEnabled) {
    return true;
  }
  if (phaseIsDemolished(metadata)) {
    return filter.phaseTimelineShowDemolished;
  }
  const auto currentPhase = phaseIndex(inputs.phaseOrder, metadata.phase);
  if (!currentPhase) {
    return true;
  }
  const size_t activePhaseIndex =
      inputs.phaseOrder.empty()
          ? static_cast<size_t>(filter.phaseTimelineActiveIndex)
          : std::min(static_cast<size_t>(filter.phaseTimelineActiveIndex),
                     inputs.phaseOrder.size() - 1u);
  if (*currentPhase < activePhaseIndex) {
    return filter.phaseTimelineShowExisting;
  }
  if (*currentPhase == activePhaseIndex) {
    return filter.phaseTimelineShowNew;
  }
  return filter.phaseTimelineGhostFuture;
}

[[nodiscard]] bool
metadataMatchesFilter(uint32_t objectIndex, const BimElementMetadata &metadata,
                      const BimDrawFilter &filter,
                      const BimElementMetadata *selectedMetadata,
                      const BimDrawFilterStateInputs &inputs) {
  if (filter.hideRoomVolumes && metadata.type == kBimRoomVolumeType) {
    return false;
  }
  const bool hasSelectedFilter =
      filter.selectedObjectIndex != std::numeric_limits<uint32_t>::max();
  if (filter.isolateSelection && hasSelectedFilter) {
    if (selectedMetadata == nullptr ||
        !sameBimProductIdentity(*selectedMetadata, metadata)) {
      return false;
    }
  }
  if (filter.hideSelection && hasSelectedFilter &&
      selectedMetadata != nullptr &&
      sameBimProductIdentity(*selectedMetadata, metadata)) {
    return false;
  }
  if (filter.typeFilterEnabled && !filter.type.empty() &&
      metadata.type != filter.type) {
    return false;
  }
  if (filter.storeyFilterEnabled && !filter.storey.empty() &&
      bimMetadataStoreyLabel(metadata) != filter.storey) {
    return false;
  }
  if (filter.materialFilterEnabled && !filter.material.empty() &&
      bimMetadataMaterialLabel(metadata) != filter.material) {
    return false;
  }
  if (filter.disciplineFilterEnabled && !filter.discipline.empty() &&
      metadata.discipline != filter.discipline) {
    return false;
  }
  if (filter.phaseFilterEnabled && !filter.phase.empty() &&
      metadata.phase != filter.phase) {
    return false;
  }
  if (filter.fireRatingFilterEnabled && !filter.fireRating.empty() &&
      metadata.fireRating != filter.fireRating) {
    return false;
  }
  if (filter.loadBearingFilterEnabled && !filter.loadBearing.empty() &&
      metadata.loadBearing != filter.loadBearing) {
    return false;
  }
  if (filter.statusFilterEnabled && !filter.status.empty() &&
      metadata.status != filter.status) {
    return false;
  }
  if (!metadataMatchesDisciplinePreset(metadata, filter.disciplinePreset)) {
    return false;
  }
  if (!metadataMatchesPhaseTimeline(metadata, filter, inputs)) {
    return false;
  }
  if (filter.drawBudgetEnabled && filter.drawBudgetMaxObjects > 0u &&
      objectIndex >= filter.drawBudgetMaxObjects &&
      objectIndex != filter.selectedObjectIndex) {
    return false;
  }
  return objectIndex != std::numeric_limits<uint32_t>::max();
}

void appendDrawCommand(std::vector<DrawCommand> &commands, uint32_t objectIndex,
                       uint32_t firstIndex, uint32_t indexCount,
                       bool allowMerge) {
  if (indexCount == 0u) {
    return;
  }
  if (allowMerge && !commands.empty()) {
    DrawCommand &last = commands.back();
    const uint32_t lastInstanceCount = std::max(last.instanceCount, 1u);
    if (last.firstIndex == firstIndex && last.indexCount == indexCount &&
        last.objectIndex + lastInstanceCount == objectIndex &&
        lastInstanceCount < std::numeric_limits<uint32_t>::max()) {
      last.instanceCount = lastInstanceCount + 1u;
      return;
    }
  }

  DrawCommand command{};
  command.objectIndex = objectIndex;
  command.firstIndex = firstIndex;
  command.indexCount = indexCount;
  command.instanceCount = 1u;
  commands.push_back(command);
}

void reserveLike(std::vector<DrawCommand> &out,
                 const std::vector<DrawCommand> *source) {
  if (source != nullptr) {
    out.reserve(source->size());
  }
}

void reserveLike(BimGeometryDrawLists &out,
                 const BimGeometryDrawLists *source) {
  if (source != nullptr) {
    out.reserve(source->opaqueDrawCommands.size(),
                source->transparentDrawCommands.size());
  }
}

} // namespace

bool bimDrawFiltersEqual(const BimDrawFilter &lhs, const BimDrawFilter &rhs) {
  return lhs.hideRoomVolumes == rhs.hideRoomVolumes &&
         lhs.typeFilterEnabled == rhs.typeFilterEnabled &&
         lhs.type == rhs.type &&
         lhs.storeyFilterEnabled == rhs.storeyFilterEnabled &&
         lhs.storey == rhs.storey &&
         lhs.materialFilterEnabled == rhs.materialFilterEnabled &&
         lhs.material == rhs.material &&
         lhs.disciplineFilterEnabled == rhs.disciplineFilterEnabled &&
         lhs.discipline == rhs.discipline &&
         lhs.phaseFilterEnabled == rhs.phaseFilterEnabled &&
         lhs.phase == rhs.phase &&
         lhs.fireRatingFilterEnabled == rhs.fireRatingFilterEnabled &&
         lhs.fireRating == rhs.fireRating &&
         lhs.loadBearingFilterEnabled == rhs.loadBearingFilterEnabled &&
         lhs.loadBearing == rhs.loadBearing &&
         lhs.statusFilterEnabled == rhs.statusFilterEnabled &&
         lhs.status == rhs.status &&
         lhs.drawBudgetEnabled == rhs.drawBudgetEnabled &&
         lhs.drawBudgetMaxObjects == rhs.drawBudgetMaxObjects &&
         lhs.isolateSelection == rhs.isolateSelection &&
         lhs.hideSelection == rhs.hideSelection &&
         lhs.selectedObjectIndex == rhs.selectedObjectIndex &&
         lhs.disciplinePreset == rhs.disciplinePreset &&
         lhs.phaseTimelineEnabled == rhs.phaseTimelineEnabled &&
         lhs.phaseTimelineActiveIndex == rhs.phaseTimelineActiveIndex &&
         lhs.phaseTimelineShowExisting == rhs.phaseTimelineShowExisting &&
         lhs.phaseTimelineShowNew == rhs.phaseTimelineShowNew &&
         lhs.phaseTimelineShowDemolished ==
             rhs.phaseTimelineShowDemolished &&
         lhs.phaseTimelineGhostFuture == rhs.phaseTimelineGhostFuture;
}

void BimDrawFilterState::clear() {
  filteredDrawLists_.clear();
  cachedFilter_ = {};
  cachedRevision_ = std::numeric_limits<uint64_t>::max();
}

bool BimDrawFilterState::objectMatchesFilter(
    uint32_t objectIndex, const BimDrawFilter &filter,
    const BimDrawFilterStateInputs &inputs) const {
  if (!filter.active()) {
    return objectIndex < inputs.objectCount;
  }

  const BimElementMetadata *metadata =
      metadataForObject(inputs.metadata, objectIndex);
  if (metadata == nullptr) {
    return false;
  }

  const BimElementMetadata *selectedMetadata = nullptr;
  if ((filter.isolateSelection || filter.hideSelection) &&
      filter.selectedObjectIndex != std::numeric_limits<uint32_t>::max()) {
    selectedMetadata =
        metadataForObject(inputs.metadata, filter.selectedObjectIndex);
  }
  return metadataMatchesFilter(objectIndex, *metadata, filter,
                               selectedMetadata, inputs);
}

const BimDrawLists &
BimDrawFilterState::filteredDrawLists(const BimDrawFilter &filter,
                                      const BimDrawFilterStateInputs &inputs) {
  if (cachedRevision_ == inputs.revision &&
      bimDrawFiltersEqual(cachedFilter_, filter)) {
    return filteredDrawLists_;
  }

  filteredDrawLists_.clear();
  cachedFilter_ = filter;
  cachedRevision_ = inputs.revision;

  const BimElementMetadata *selectedMetadata = nullptr;
  if ((filter.isolateSelection || filter.hideSelection) &&
      filter.selectedObjectIndex != std::numeric_limits<uint32_t>::max()) {
    selectedMetadata =
        metadataForObject(inputs.metadata, filter.selectedObjectIndex);
  }

  reserveLike(filteredDrawLists_.opaqueDrawCommands, inputs.opaqueDrawCommands);
  reserveLike(filteredDrawLists_.opaqueSingleSidedDrawCommands,
              inputs.opaqueSingleSidedDrawCommands);
  reserveLike(filteredDrawLists_.opaqueWindingFlippedDrawCommands,
              inputs.opaqueWindingFlippedDrawCommands);
  reserveLike(filteredDrawLists_.opaqueDoubleSidedDrawCommands,
              inputs.opaqueDoubleSidedDrawCommands);
  reserveLike(filteredDrawLists_.transparentDrawCommands,
              inputs.transparentDrawCommands);
  reserveLike(filteredDrawLists_.transparentSingleSidedDrawCommands,
              inputs.transparentSingleSidedDrawCommands);
  reserveLike(filteredDrawLists_.transparentWindingFlippedDrawCommands,
              inputs.transparentWindingFlippedDrawCommands);
  reserveLike(filteredDrawLists_.transparentDoubleSidedDrawCommands,
              inputs.transparentDoubleSidedDrawCommands);
  reserveLike(filteredDrawLists_.points, inputs.pointDrawLists);
  reserveLike(filteredDrawLists_.curves, inputs.curveDrawLists);
  reserveLike(filteredDrawLists_.nativePoints, inputs.nativePointDrawLists);
  reserveLike(filteredDrawLists_.nativeCurves, inputs.nativeCurveDrawLists);

  auto appendFiltered = [&](const std::vector<DrawCommand> *source,
                            std::vector<DrawCommand> &out, bool allowMerge) {
    if (source == nullptr) {
      return;
    }
    for (const DrawCommand &command : *source) {
      const uint32_t instanceCount = std::max(command.instanceCount, 1u);
      for (uint32_t instanceOffset = 0u; instanceOffset < instanceCount;
           ++instanceOffset) {
        if (command.objectIndex >
            std::numeric_limits<uint32_t>::max() - instanceOffset) {
          break;
        }
        const uint32_t objectIndex = command.objectIndex + instanceOffset;
        const BimElementMetadata *metadata =
            metadataForObject(inputs.metadata, objectIndex);
        if (metadata == nullptr ||
            !metadataMatchesFilter(objectIndex, *metadata, filter,
                                   selectedMetadata, inputs)) {
          continue;
        }
        appendDrawCommand(out, objectIndex, command.firstIndex,
                          command.indexCount, allowMerge);
      }
    }
  };

  appendFiltered(inputs.opaqueDrawCommands,
                 filteredDrawLists_.opaqueDrawCommands, false);
  appendFiltered(inputs.opaqueSingleSidedDrawCommands,
                 filteredDrawLists_.opaqueSingleSidedDrawCommands, true);
  appendFiltered(inputs.opaqueWindingFlippedDrawCommands,
                 filteredDrawLists_.opaqueWindingFlippedDrawCommands, true);
  appendFiltered(inputs.opaqueDoubleSidedDrawCommands,
                 filteredDrawLists_.opaqueDoubleSidedDrawCommands, true);
  appendFiltered(inputs.transparentDrawCommands,
                 filteredDrawLists_.transparentDrawCommands, false);
  appendFiltered(inputs.transparentSingleSidedDrawCommands,
                 filteredDrawLists_.transparentSingleSidedDrawCommands, false);
  appendFiltered(inputs.transparentWindingFlippedDrawCommands,
                 filteredDrawLists_.transparentWindingFlippedDrawCommands,
                 false);
  appendFiltered(inputs.transparentDoubleSidedDrawCommands,
                 filteredDrawLists_.transparentDoubleSidedDrawCommands, false);

  auto appendFilteredGeometry = [&](const BimGeometryDrawLists *source,
                                    BimGeometryDrawLists &out) {
    if (source == nullptr) {
      return;
    }
    appendFiltered(&source->opaqueDrawCommands, out.opaqueDrawCommands, false);
    appendFiltered(&source->opaqueSingleSidedDrawCommands,
                   out.opaqueSingleSidedDrawCommands, true);
    appendFiltered(&source->opaqueWindingFlippedDrawCommands,
                   out.opaqueWindingFlippedDrawCommands, true);
    appendFiltered(&source->opaqueDoubleSidedDrawCommands,
                   out.opaqueDoubleSidedDrawCommands, true);
    appendFiltered(&source->transparentDrawCommands,
                   out.transparentDrawCommands, false);
    appendFiltered(&source->transparentSingleSidedDrawCommands,
                   out.transparentSingleSidedDrawCommands, false);
    appendFiltered(&source->transparentWindingFlippedDrawCommands,
                   out.transparentWindingFlippedDrawCommands, false);
    appendFiltered(&source->transparentDoubleSidedDrawCommands,
                   out.transparentDoubleSidedDrawCommands, false);
  };
  appendFilteredGeometry(inputs.pointDrawLists, filteredDrawLists_.points);
  appendFilteredGeometry(inputs.curveDrawLists, filteredDrawLists_.curves);
  appendFilteredGeometry(inputs.nativePointDrawLists,
                         filteredDrawLists_.nativePoints);
  appendFilteredGeometry(inputs.nativeCurveDrawLists,
                         filteredDrawLists_.nativeCurves);

  return filteredDrawLists_;
}

} // namespace container::renderer
