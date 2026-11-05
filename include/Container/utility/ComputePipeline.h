#pragma once

#include "Container/common/CommonVulkan.h"

#include <memory>

namespace container::gpu {

class PipelineManager;

// Owners keep this slot across reloads and obtain the current handle when
// recording. The manager observes slots weakly, including during recreation.
// Recording, reload publication and owner retirement are serialized by the
// owning render thread; concurrent handle reads/publication are not supported.
class ComputePipeline {
 public:
  ComputePipeline() = default;
  ComputePipeline(ComputePipeline&&) noexcept = default;
  ComputePipeline& operator=(ComputePipeline&&) noexcept = default;
  ComputePipeline(const ComputePipeline&) = delete;
  ComputePipeline& operator=(const ComputePipeline&) = delete;

  operator VkPipeline() const noexcept;
  [[nodiscard]] ComputePipeline share() const noexcept {
    ComputePipeline copy;
    copy.state_ = state_;
    return copy;
  }
  void reset() noexcept { state_.reset(); }

 private:
  friend class PipelineManager;
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace container::gpu
