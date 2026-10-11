#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace container::renderer {

// GPU completion publishes a new immutable lighting snapshot. A single writer
// uses the other snapshot; its input descriptors remain retained by image until
// the matching compute timeline value retires.
class DdgiAsyncHistory {
public:
  struct Work {
    uint32_t image{}, previous{}, next{};
    uint64_t graphicsReady{}, completion{};
  };

  void reset(uint32_t imageCount) {
    if (pending_)
      throw std::logic_error("DDGI async history reset with work in flight");
    imageUses_.assign(imageCount, 0);
    history_ = 0;
    publication_ = graphics_ = compute_ = 0;
  }
  [[nodiscard]] uint32_t history() const { return history_; }
  [[nodiscard]] uint64_t publication() const { return publication_; }
  [[nodiscard]] uint64_t graphicsSignal() const { return graphics_ + 1; }
  [[nodiscard]] bool pending() const { return pending_.has_value(); }
  [[nodiscard]] uint64_t imageRetirement(uint32_t image) const {
    return imageUses_.at(image);
  }
  [[nodiscard]] uint64_t lastCompute() const { return compute_; }
  void remapPublishedHistory(uint32_t index) {
    if (pending_ || index > 1)
      throw std::logic_error("DDGI cannot remap an active writer");
    history_ = index;
  }

  void commitGraphics(uint64_t value) {
    if (value != graphics_ + 1)
      throw std::logic_error("DDGI graphics timeline submission out of order");
    graphics_ = value;
  }
  [[nodiscard]] Work plan(uint32_t image) const {
    if (pending_ || graphics_ == 0 || image >= imageUses_.size())
      throw std::logic_error("DDGI async writer is not ready");
    return {image, history_, 1u - history_, graphics_, compute_ + 1};
  }
  void commit(const Work &work) {
    const auto expected = plan(work.image);
    if (work.previous != expected.previous || work.next != expected.next ||
        work.graphicsReady != expected.graphicsReady ||
        work.completion != expected.completion)
      throw std::logic_error("DDGI stale async writer submission");
    pending_ = work;
    compute_ = work.completion;
    imageUses_[work.image] = work.completion;
  }
  [[nodiscard]] std::optional<Work> publish(uint64_t completed) {
    if (!pending_ || completed < pending_->completion)
      return {};
    auto work = pending_;
    history_ = work->next;
    publication_ = work->completion;
    pending_.reset();
    return work;
  }

private:
  uint32_t history_{};
  uint64_t publication_{}, graphics_{}, compute_{};
  std::optional<Work> pending_;
  std::vector<uint64_t> imageUses_;
};
} // namespace container::renderer
