#pragma once

#include "Container/common/CommonMath.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace container::renderer {

class BimFloorPlanOverlayData {
 public:
  std::vector<DrawCommand> drawCommands{};
  // Storey commands address the same vertices and overlay object as the
  // aggregate command. Prepared scenes fill in the object index on install.
  std::map<std::string, std::vector<DrawCommand>, std::less<>> storeyDrawCommands;
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  glm::vec3 boundsCenter{0.0f};
  float boundsRadius{0.0f};

  [[nodiscard]] bool valid() const {
    return indexCount >= 2u && !drawCommands.empty();
  }

  [[nodiscard]] const std::vector<DrawCommand> &
  commands(std::string_view storey) const {
    if (storey.empty()) {
      return drawCommands;
    }
    static const std::vector<DrawCommand> empty;
    const auto found = storeyDrawCommands.find(storey);
    return found == storeyDrawCommands.end() ? empty : found->second;
  }
};

}  // namespace container::renderer
