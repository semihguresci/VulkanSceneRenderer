#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace container::renderer {

struct DrawCommand;
enum class BimDrawCompactionSlot : uint32_t;

enum class BimPrimitivePassKind : uint32_t {
  Points = 0,
  Curves = 1,
};

struct BimPrimitivePassDrawLists {
  const std::vector<DrawCommand> *opaqueDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueWindingFlippedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueDoubleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentWindingFlippedDrawCommands{
      nullptr};
  const std::vector<DrawCommand> *transparentDoubleSidedDrawCommands{nullptr};
};

struct BimPrimitivePassPlanInputs {
  BimPrimitivePassKind kind{BimPrimitivePassKind::Points};
  bool enabled{false};
  bool depthTest{true};
  bool placeholderRangePreviewEnabled{false};
  bool nativeDrawsUseGpuVisibility{false};
  float opacity{1.0f};
  float primitiveSize{1.0f};
  BimPrimitivePassDrawLists placeholderDraws{};
  BimPrimitivePassDrawLists nativeDraws{};
};

struct BimPrimitivePassPlan {
  bool active{false};
  bool depthTest{true};
  bool nativeDrawsSelected{false};
  bool useObjectColor{false};
  bool gpuCompaction{false};
  float opacity{1.0f};
  float primitiveSize{1.0f};
  std::vector<const std::vector<DrawCommand> *> cpuDrawSources{};
  std::array<BimDrawCompactionSlot, 2> gpuSlots{};
  uint32_t gpuSlotCount{0};
};

class BimPrimitivePassPlanner {
public:
  explicit BimPrimitivePassPlanner(BimPrimitivePassPlanInputs inputs);

  [[nodiscard]] BimPrimitivePassPlan build() const;

private:
  BimPrimitivePassPlanInputs inputs_{};
};

[[nodiscard]] bool
hasBimPrimitivePassDrawCommands(const BimPrimitivePassDrawLists &draws);

[[nodiscard]] BimPrimitivePassPlan
buildBimPrimitivePassPlan(const BimPrimitivePassPlanInputs &inputs);

} // namespace container::renderer
