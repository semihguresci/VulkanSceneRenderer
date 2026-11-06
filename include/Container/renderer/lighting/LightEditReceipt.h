#pragma once

#include "Container/renderer/lighting/LightEditTypes.h"
#include <memory>
#include <optional>

namespace container::renderer {
class LightingManager;
struct LightEditAddState;
struct LightEditLimits {
  uint64_t maxPreparedCpuBytes{64ull*1024*1024},maxPreparedGpuBytes{256ull*1024*1024};
  uint64_t maxCommandBytes{64ull*1024},maxLabelBytes{4096},maxWork{64ull*1024*1024};
};
struct LightEditCanonicalPayload {
  enum class Kind:uint8_t {None,Point,Area} kind{Kind::None};
  container::gpu::PointLightData point{};
  container::gpu::AreaLightData area{};
};
class LightEditCommand {
public:
  LightEditCommand() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool published() const noexcept;
  [[nodiscard]] bool applied() const noexcept;
  [[nodiscard]] bool netChanged() const noexcept;
  [[nodiscard]] uint32_t ownedFields() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] LightEditTarget target() const noexcept;
private:
  struct State;
  std::shared_ptr<State> state_;
  friend class LightingManager;
  friend class PreparedLightEditPlan;
  friend struct LightEditReceiptAccess;
};
// Pure selected override planner. No source, rest, World or GPU mutation.
// Presence is distinct from an equal sampled source value.
class PreparedLightEditPlan {
public:
  PreparedLightEditPlan() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool changed() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] LightEditCommand command() const noexcept;
  [[nodiscard]] const std::optional<EditableLightEntity>& overrideAfter() const;
  [[nodiscard]] const EditableLightEntity& selectedAfter() const;
  [[nodiscard]] const LightEditCanonicalPayload& canonicalAfter() const;
private:
  struct State;
  std::shared_ptr<State> state_;
  friend class LightingManager;
  friend struct LightEditReceiptAccess;
};
[[nodiscard]] PreparedLightEditPlan PlanLightEdit(const LightEditTarget&,const EditableLightEntity& currentDisplayed,
    const EditableLightEntity& currentBase,const std::optional<EditableLightEntity>& currentOverride,
    const LightEditPatch&,const LightEditCommand* gesture=nullptr,const LightEditLimits& = {},const LightEditCanonicalPayload* currentRaw=nullptr);
[[nodiscard]] PreparedLightEditPlan RebaseLightEdit(const LightEditTarget&,const EditableLightEntity& currentDisplayed,
    const EditableLightEntity& currentBase,const std::optional<EditableLightEntity>& currentOverride,
    const LightEditCommand&,bool applied,const LightEditLimits& = {},const LightEditCanonicalPayload* currentRaw=nullptr);
[[nodiscard]] bool LightEditPlanMatches(const PreparedLightEditPlan&,const LightEditTarget&,
    const EditableLightEntity& currentDisplayed,const EditableLightEntity& currentBase,
    const std::optional<EditableLightEntity>& currentOverride) noexcept;
// Internal publication hook: verifies the immutable selected receipt, then
// updates only bounded command phase/realized evidence. The scene owner guards
// actual live publication before invoking this allocation-free operation.
[[nodiscard]] bool PublishLightEditPlan(PreparedLightEditPlan&,uint64_t editorRevision=0) noexcept;
[[nodiscard]] bool LightEditValuesEqual(const LightEditValues&,const LightEditValues&,uint32_t fields=kAllLightEditFields) noexcept;
// Internal original-creation receipt. The opaque authority is minted only by
// LightingManager; an arbitrary selected property command grants no tail cut.
[[nodiscard]] PreparedLightEditPlan PlanLightEditStructure(const LightEditTarget&,
    const std::optional<EditableLightEntity>& before,const std::optional<EditableLightEntity>& after,
    const EditableLightEntity& selected,const LightEditCommand* original,bool applied,
    std::shared_ptr<const LightEditAddState>,const LightEditLimits& = {});
[[nodiscard]] std::shared_ptr<const LightEditAddState> LightEditAddition(const LightEditCommand&) noexcept;
[[nodiscard]] bool LightEditCommandPreviewMatches(const LightEditCommand&,const LightEditTarget&,
    const std::optional<LightEditValues>& currentOverride) noexcept;
} // namespace container::renderer
