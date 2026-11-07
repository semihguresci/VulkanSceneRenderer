#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace container::util {

// CPU edit history. Callbacks own any state needed for later playback and must
// return true only after applying the complete edit. A false result or
// exception leaves the history cursor unchanged; callbacks are responsible for
// leaving their edited state unchanged on failure. Callbacks must not mutate
// this history.
class EditHistory {
public:
  struct Command {
    std::string label;
    std::function<bool()> undo;
    std::function<bool()> redo;
    // The command publishes its complete graph/renderer generation itself.
    // Adapters must not run an additional refresh after successful playback.
    bool selfContainedPublication{false};
  };

  using MergeId = std::uint64_t;

  explicit EditHistory(std::size_t capacity = 128) : capacity_(capacity) {
    // Allocate before applying any commands. Subsequent records only move or
    // erase commands, so an allocation failure cannot discard a redo branch.
    commands_.reserve(capacity_);
  }

  EditHistory(const EditHistory &) = delete;
  EditHistory &operator=(const EditHistory &) = delete;
  EditHistory(EditHistory &&) = delete;
  EditHistory &operator=(EditHistory &&) = delete;

  // Record an edit that the caller already applied. Adjacent records with the
  // same explicit mergeId form one gesture: retain the first undo and last
  // redo. Call finishGesture() at release/deactivation, even if the next
  // gesture edits the same property. Missing IDs always create individual undo
  // steps. The caller filters semantic no-ops before recording. Returns false
  // at capacity 0.
  bool record(Command command, std::optional<MergeId> mergeId = std::nullopt) {
    validate(command);
    if (capacity_ == 0)
      return false;

    if (mergeId && activeMergeId_ == mergeId && cursor_ == commands_.size() &&
        cursor_ != 0 &&
        commands_.back().selfContainedPublication == command.selfContainedPublication) {
      commands_.back().label.swap(command.label);
      commands_.back().redo.swap(command.redo);
      return true;
    }

    commands_.erase(commands_.begin() + cursor_, commands_.end());
    if (commands_.size() == capacity_)
      commands_.erase(commands_.begin());
    commands_.push_back(std::move(command));
    cursor_ = commands_.size();
    activeMergeId_ = mergeId;
    return true;
  }

  // Apply redo first and record only if it succeeds. A disabled history still
  // executes commands successfully. Exceptions propagate without moving cursor.
  bool execute(Command command, std::optional<MergeId> mergeId = std::nullopt) {
    validate(command);
    if (!command.redo())
      return false;
    record(std::move(command), mergeId);
    return true;
  }

  bool undo() {
    if (!canUndo() || !commands_[cursor_ - 1].undo())
      return false;
    --cursor_;
    finishGesture();
    return true;
  }

  bool redo() {
    if (!canRedo() || !commands_[cursor_].redo())
      return false;
    ++cursor_;
    finishGesture();
    return true;
  }

  [[nodiscard]] bool canUndo() const noexcept { return cursor_ != 0; }
  [[nodiscard]] bool canRedo() const noexcept {
    return cursor_ != commands_.size();
  }
  [[nodiscard]] bool undoIsSelfContainedPublication() const noexcept {
    return canUndo() && commands_[cursor_ - 1].selfContainedPublication;
  }
  [[nodiscard]] bool redoIsSelfContainedPublication() const noexcept {
    return canRedo() && commands_[cursor_].selfContainedPublication;
  }
  [[nodiscard]] std::string_view undoLabel() const noexcept {
    return canUndo() ? std::string_view(commands_[cursor_ - 1].label)
                     : std::string_view{};
  }
  [[nodiscard]] std::string_view redoLabel() const noexcept {
    return canRedo() ? std::string_view(commands_[cursor_].label)
                     : std::string_view{};
  }
  [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
  [[nodiscard]] std::size_t undoCount() const noexcept { return cursor_; }
  [[nodiscard]] std::size_t redoCount() const noexcept {
    return commands_.size() - cursor_;
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  void finishGesture() noexcept { activeMergeId_.reset(); }

  // Release all command captures, including redo entries, on scene replacement.
  void clear() noexcept {
    commands_.clear();
    cursor_ = 0;
    finishGesture();
  }

private:
  static void validate(const Command &command) {
    if (!command.undo || !command.redo)
      throw std::invalid_argument(
          "Edit history requires undo and redo callbacks");
  }

  std::size_t capacity_;
  std::vector<Command> commands_;
  std::size_t cursor_ = 0;
  std::optional<MergeId> activeMergeId_;
};

} // namespace container::util
