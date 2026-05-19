#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace container::renderer {

struct DrawCommand;
class BimManager;
enum class BimDrawCompactionSlot : uint32_t;

struct BimDrawCompactionPlanSource {
  BimDrawCompactionSlot slot{};
  const std::vector<DrawCommand> *commands{nullptr};
};

struct BimDrawCompactionPlanInputs {
  bool meshCompactionEnabled{true};
  const std::vector<DrawCommand> *opaqueSingleSided{nullptr};
  const std::vector<DrawCommand> *opaqueWindingFlipped{nullptr};
  const std::vector<DrawCommand> *opaqueDoubleSided{nullptr};
  const std::vector<DrawCommand> *transparentAggregate{nullptr};
  const std::vector<DrawCommand> *transparentSingleSided{nullptr};
  const std::vector<DrawCommand> *transparentWindingFlipped{nullptr};
  const std::vector<DrawCommand> *transparentDoubleSided{nullptr};
  const std::vector<DrawCommand> *nativePointOpaque{nullptr};
  const std::vector<DrawCommand> *nativePointTransparent{nullptr};
  const std::vector<DrawCommand> *nativeCurveOpaque{nullptr};
  const std::vector<DrawCommand> *nativeCurveTransparent{nullptr};
};

class BimDrawCompactionPlanner {
public:
  explicit BimDrawCompactionPlanner(BimDrawCompactionPlanInputs inputs);

  [[nodiscard]] std::vector<BimDrawCompactionPlanSource> build() const;

private:
  BimDrawCompactionPlanInputs inputs_{};
};

[[nodiscard]] BimDrawCompactionPlanInputs
makeBimDrawCompactionPlanInputs(const BimManager &bimManager);

[[nodiscard]] size_t
bimDrawCompactionOutputCapacity(const std::vector<DrawCommand> &commands);

[[nodiscard]] std::vector<BimDrawCompactionPlanSource>
buildBimDrawCompactionPlan(const BimDrawCompactionPlanInputs &inputs);

} // namespace container::renderer
