#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Container/renderer/lighting/LightEditTypes.h"

namespace container::ui {

// Weak UI state only. Renderer-owned lifetimes and history stay in the
// coordinator; drawing never changes light buffers, clocks or source records.
class LightEditControls {
 public:
  static constexpr size_t maxPendingRequests = 32;
  static constexpr size_t maxDisplayBytes = 512;
  void setSnapshot(container::renderer::LightEditSnapshot snapshot);
  [[nodiscard]] const container::renderer::LightEditSnapshot& snapshot()
      const noexcept {
    return snapshot_;
  }
  void queue(container::renderer::LightEditRequest request);
  [[nodiscard]] std::vector<container::renderer::LightEditRequest> consume();
  void queueAdd(container::renderer::EditableLightType);
  [[nodiscard]] std::optional<container::renderer::EditableLightType>
  consumeAdd() noexcept;
  void finishGesture(bool cancel = false);
  [[nodiscard]] bool gestureActive() const noexcept {
    return active_.has_value();
  }
  [[nodiscard]] bool hasPending() const noexcept {
    return !requests_.empty() || add_.has_value();
  }
  void processInput();
  void drawAddButtons();
  void draw();

 private:
  struct Gesture {
    container::renderer::LightEditTarget target;
    uint64_t id{};
    uint32_t item{}, field{};
    bool started{};
  };
  void track(uint32_t item, container::renderer::LightEditField field,
             bool changed, bool activated, bool active);
  container::renderer::LightEditSnapshot snapshot_{};
  container::renderer::LightEditValues draft_{};
  std::optional<Gesture> active_{};
  uint64_t nextGesture_{};
  std::vector<container::renderer::LightEditRequest> requests_{};
  std::optional<container::renderer::EditableLightType> add_{};
  std::string error_{};
  int cancelledFrame_{-1};
};

}  // namespace container::ui
