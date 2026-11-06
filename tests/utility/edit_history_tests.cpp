#include "Container/utility/EditHistory.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using History = container::util::EditHistory;

History::Command change(int &value, int before, int after,
                        std::string label = "Change") {
  return {std::move(label),
          [&value, before] {
            value = before;
            return true;
          },
          [&value, after] {
            value = after;
            return true;
          }};
}

TEST(EditHistory, StartsEmptyAndRecordsAlreadyAppliedChanges) {
  History history;
  int value = 3;
  EXPECT_EQ(history.capacity(), 128u);
  EXPECT_FALSE(history.canUndo());
  EXPECT_FALSE(history.canRedo());
  EXPECT_TRUE(history.undoLabel().empty());
  EXPECT_TRUE(history.redoLabel().empty());
  EXPECT_FALSE(history.undo());
  EXPECT_FALSE(history.redo());

  ASSERT_TRUE(history.record(change(value, 1, 3, "Move")));
  EXPECT_EQ(value, 3);
  EXPECT_EQ(history.undoLabel(), "Move");
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(value, 1);
  EXPECT_FALSE(history.canUndo());
  EXPECT_EQ(history.redoLabel(), "Move");
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(value, 3);
  EXPECT_EQ(history.undoCount(), 1u);
  EXPECT_EQ(history.redoCount(), 0u);
}

TEST(EditHistory, ExecuteAppliesAndRecordsInOrder) {
  History history;
  int value = 0;
  ASSERT_TRUE(history.execute(change(value, 0, 1, "First")));
  ASSERT_TRUE(history.execute(change(value, 1, 2, "Second")));
  EXPECT_EQ(value, 2);
  EXPECT_EQ(history.size(), 2u);
  EXPECT_EQ(history.undoLabel(), "Second");
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(value, 1);
  EXPECT_EQ(history.undoLabel(), "First");
  EXPECT_EQ(history.redoLabel(), "Second");
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(value, 0);
  ASSERT_TRUE(history.redo());
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(value, 2);
}

TEST(EditHistory, GestureKeepsEarliestUndoAndLatestRedo) {
  History history;
  int value = 0;
  ASSERT_TRUE(history.execute(change(value, 0, 1, "Move 1"), 42));
  ASSERT_TRUE(history.execute(change(value, 1, 2, "Move 2"), 42));
  ASSERT_TRUE(history.execute(change(value, 2, 5, "Move 5"), 42));
  EXPECT_EQ(history.size(), 1u);
  EXPECT_EQ(history.undoLabel(), "Move 5");
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(value, 0);
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(value, 5);
}

TEST(EditHistory, CompletedGesturesAndDistinctIdentitiesStaySeparate) {
  History history;
  int value = 0;
  ASSERT_TRUE(history.execute(change(value, 0, 1), 42));
  history.finishGesture();
  ASSERT_TRUE(history.execute(change(value, 1, 2), 42));
  ASSERT_TRUE(history.execute(change(value, 2, 3), 43));
  ASSERT_TRUE(history.execute(change(value, 3, 4)));
  ASSERT_TRUE(history.execute(change(value, 4, 5)));
  ASSERT_TRUE(history.execute(change(value, 5, 6), 43));
  EXPECT_EQ(history.size(), 6u);
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(value, 5);
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(value, 4);
}

TEST(EditHistory, SuccessfulPlaybackEndsGesture) {
  History history;
  int value = 0;
  ASSERT_TRUE(history.execute(change(value, 0, 1), 7));
  ASSERT_TRUE(history.undo());
  ASSERT_TRUE(history.redo());
  ASSERT_TRUE(history.execute(change(value, 1, 2), 7));
  EXPECT_EQ(history.size(), 2u);
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(value, 1);
}

TEST(EditHistory, NewEditDropsRedoBranchAndReleasesCaptures) {
  History history;
  int value = 0;
  ASSERT_TRUE(history.execute(change(value, 0, 1, "First")));
  auto retained = std::make_shared<int>(2);
  std::weak_ptr<int> lifetime = retained;
  ASSERT_TRUE(history.execute({"Old branch",
                               [&value, retained] {
                                 value = 1;
                                 return true;
                               },
                               [&value, retained] {
                                 value = *retained;
                                 return true;
                               }}));
  retained.reset();
  ASSERT_TRUE(history.undo());
  EXPECT_FALSE(lifetime.expired());
  ASSERT_TRUE(history.execute(change(value, 1, 9, "New branch")));
  EXPECT_TRUE(lifetime.expired());
  EXPECT_EQ(history.size(), 2u);
  EXPECT_FALSE(history.canRedo());
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(value, 1);
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(value, 9);
}

TEST(EditHistory, CapacityEvictsOldestCommandAndReleasesCaptures) {
  History history(2);
  int value = 0;
  auto retained = std::make_shared<int>(0);
  std::weak_ptr<int> lifetime = retained;
  ASSERT_TRUE(history.execute({"Oldest",
                               [retained, &value] {
                                 value = *retained;
                                 return true;
                               },
                               [retained, &value] {
                                 value = 1;
                                 return true;
                               }}));
  retained.reset();
  ASSERT_TRUE(history.execute(change(value, 1, 2)));
  EXPECT_FALSE(lifetime.expired());
  ASSERT_TRUE(history.execute(change(value, 2, 3)));
  EXPECT_TRUE(lifetime.expired());
  EXPECT_EQ(history.size(), 2u);
  EXPECT_EQ(history.undoCount(), 2u);
  ASSERT_TRUE(history.undo());
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(value, 1);
  EXPECT_FALSE(history.undo());
}

TEST(EditHistory, ClearDropsBothBranchesAndResetsGesture) {
  History history;
  int value = 0;
  auto retained = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = retained;
  ASSERT_TRUE(history.execute(
      {"Owned", [retained] { return true; }, [retained] { return true; }}));
  retained.reset();
  ASSERT_TRUE(history.execute(change(value, 0, 1), 8));
  ASSERT_TRUE(history.undo());
  history.clear();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_EQ(history.size(), 0u);
  EXPECT_EQ(history.undoCount(), 0u);
  EXPECT_EQ(history.redoCount(), 0u);
  EXPECT_TRUE(history.undoLabel().empty());
  EXPECT_TRUE(history.redoLabel().empty());
  ASSERT_TRUE(history.execute(change(value, 0, 2), 8));
  EXPECT_EQ(history.size(), 1u);
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(value, 0);
}

TEST(EditHistory, FailedUndoAndRedoKeepCursorAndLabels) {
  History history;
  bool allowUndo = false;
  bool allowRedo = true;
  ASSERT_TRUE(history.execute(
      {"Guarded", [&] { return allowUndo; }, [&] { return allowRedo; }}));
  EXPECT_FALSE(history.undo());
  EXPECT_EQ(history.undoCount(), 1u);
  EXPECT_EQ(history.undoLabel(), "Guarded");
  EXPECT_FALSE(history.canRedo());
  allowUndo = true;
  ASSERT_TRUE(history.undo());
  allowRedo = false;
  EXPECT_FALSE(history.redo());
  EXPECT_EQ(history.redoCount(), 1u);
  EXPECT_EQ(history.redoLabel(), "Guarded");
  EXPECT_FALSE(history.canUndo());
  allowRedo = true;
  EXPECT_TRUE(history.redo());
}

TEST(EditHistory, ThrowingPlaybackKeepsCursorAndRemainsUsable) {
  History history;
  bool failUndo = true;
  bool failRedo = false;
  ASSERT_TRUE(history.execute({"May throw",
                               [&] {
                                 if (failUndo)
                                   throw std::runtime_error("undo failed");
                                 return true;
                               },
                               [&] {
                                 if (failRedo)
                                   throw std::runtime_error("redo failed");
                                 return true;
                               }}));
  EXPECT_THROW(history.undo(), std::runtime_error);
  EXPECT_EQ(history.undoCount(), 1u);
  EXPECT_EQ(history.undoLabel(), "May throw");
  failUndo = false;
  ASSERT_TRUE(history.undo());
  failRedo = true;
  EXPECT_THROW(history.redo(), std::runtime_error);
  EXPECT_EQ(history.redoCount(), 1u);
  EXPECT_EQ(history.redoLabel(), "May throw");
  failRedo = false;
  EXPECT_TRUE(history.redo());
}

TEST(EditHistory, FailedExecutionPreservesRedoBranch) {
  History history;
  int value = 0;
  ASSERT_TRUE(history.execute(change(value, 0, 1, "Existing")));
  ASSERT_TRUE(history.undo());
  EXPECT_FALSE(
      history.execute({"Rejected", [] { return true; }, [] { return false; }}));
  EXPECT_THROW(history.execute({"Throws", [] { return true; },
                                []() -> bool {
                                  throw std::runtime_error("execution failed");
                                }}),
               std::runtime_error);
  EXPECT_EQ(history.size(), 1u);
  EXPECT_EQ(history.undoCount(), 0u);
  EXPECT_EQ(history.redoLabel(), "Existing");
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(value, 1);
}

TEST(EditHistory, OneCommandRestoresSeveralChangedValuesTogether) {
  History history;
  int position = 0;
  int scale = 1;
  ASSERT_TRUE(history.execute({"Transform",
                               [&] {
                                 position = 0;
                                 scale = 1;
                                 return true;
                               },
                               [&] {
                                 position = 5;
                                 scale = 3;
                                 return true;
                               }}));
  EXPECT_EQ(position, 5);
  EXPECT_EQ(scale, 3);
  EXPECT_EQ(history.size(), 1u);
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(position, 0);
  EXPECT_EQ(scale, 1);
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(position, 5);
  EXPECT_EQ(scale, 3);
}

TEST(EditHistory, ZeroCapacityExecutesWithoutRetainingCommands) {
  History history(0);
  int value = 0;
  EXPECT_FALSE(history.record(change(value, 0, 1)));
  ASSERT_TRUE(history.execute(change(value, 0, 2)));
  EXPECT_EQ(value, 2);
  EXPECT_EQ(history.size(), 0u);
  EXPECT_FALSE(history.canUndo());
}

TEST(EditHistory, EmptyCallbacksAreRejectedBeforeExecution) {
  History history;
  int executions = 0;
  EXPECT_THROW(history.record({"Missing redo", [] { return true; }, {}}),
               std::invalid_argument);
  EXPECT_THROW(history.execute({"Missing undo",
                                {},
                                [&] {
                                  ++executions;
                                  return true;
                                }}),
               std::invalid_argument);
  EXPECT_EQ(executions, 0);
  EXPECT_EQ(history.size(), 0u);
}
} // namespace
