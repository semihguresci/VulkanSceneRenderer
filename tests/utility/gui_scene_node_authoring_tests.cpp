#include <Container/utility/GuiManager.h>

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using container::ui::GuiManager;
using container::ui::SceneNodeAuthoringAction;
using container::ui::SceneNodeAuthoringRequest;

TEST(GuiSceneNodeAuthoring, QueuesExactUtf8IdentityAndTargetWithoutAnImGuiContext) {
  GuiManager gui;
  EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());
  gui.queueSceneNodeAuthoringRequest({SceneNodeAuthoringAction::Rename, 37, 12,
                                      "Fa\xc3\xa7" "ade \xe6\x9d\xb1"});
  const auto request = gui.consumeSceneNodeAuthoringRequest();
  ASSERT_TRUE(request);
  EXPECT_EQ(request->action, SceneNodeAuthoringAction::Rename);
  EXPECT_EQ(request->graphIdentity, 37u);
  EXPECT_EQ(request->node, 12u);
  EXPECT_EQ(request->name, "Fa\xc3\xa7" "ade \xe6\x9d\xb1");
  EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());
}

TEST(GuiSceneNodeAuthoring, RejectedReplacementPreservesThePendingEdit) {
  GuiManager gui;
  gui.queueSceneNodeAuthoringRequest({SceneNodeAuthoringAction::CreateEmpty, 9, 0, "Empty child"});
  for (const auto& request : {
      SceneNodeAuthoringRequest{SceneNodeAuthoringAction::Rename, 9, 1, "   "},
      SceneNodeAuthoringRequest{SceneNodeAuthoringAction::Rename, 0, 1, "Valid"},
      SceneNodeAuthoringRequest{SceneNodeAuthoringAction::Rename, 9,
          std::numeric_limits<uint32_t>::max(), "Valid"},
      SceneNodeAuthoringRequest{static_cast<SceneNodeAuthoringAction>(127), 9, 1, "Valid"}}) {
    EXPECT_THROW(gui.queueSceneNodeAuthoringRequest(request), std::invalid_argument);
  }
  const auto request = gui.consumeSceneNodeAuthoringRequest();
  ASSERT_TRUE(request);
  EXPECT_EQ(request->action, SceneNodeAuthoringAction::CreateEmpty);
  EXPECT_EQ(request->graphIdentity, 9u);
  EXPECT_EQ(request->node, 0u);
  EXPECT_EQ(request->name, "Empty child");
}

TEST(GuiSceneNodeAuthoring, DeleteTargetsRetainedIdentityWithoutInventingANewLabel) {
  GuiManager gui;
  gui.queueSceneNodeAuthoringRequest({SceneNodeAuthoringAction::DeleteSubtree, 19, 7, {}});
  const auto request = gui.consumeSceneNodeAuthoringRequest();
  ASSERT_TRUE(request);
  EXPECT_EQ(request->action, SceneNodeAuthoringAction::DeleteSubtree);
  EXPECT_EQ(request->graphIdentity, 19u);
  EXPECT_EQ(request->node, 7u);
  EXPECT_TRUE(request->name.empty());
  EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());
}

TEST(GuiSceneNodeAuthoring, DuplicateAcceptsDefaultOrExactNameAndRejectsInvalidReplacement) {
  GuiManager gui;
  gui.queueSceneNodeAuthoringRequest({SceneNodeAuthoringAction::DuplicateSubtree,31,4,{}});
  for(const auto& invalid:std::vector<std::string>{"  ","bad\nlabel",std::string(4097,'x')})
    EXPECT_THROW(gui.queueSceneNodeAuthoringRequest({SceneNodeAuthoringAction::DuplicateSubtree,31,4,invalid}),
        std::invalid_argument);
  auto request=gui.consumeSceneNodeAuthoringRequest();
  ASSERT_TRUE(request); EXPECT_TRUE(request->name.empty());
  EXPECT_EQ(request->action,SceneNodeAuthoringAction::DuplicateSubtree);
  EXPECT_EQ(request->graphIdentity,31u); EXPECT_EQ(request->node,4u);
  gui.queueSceneNodeAuthoringRequest({SceneNodeAuthoringAction::DuplicateSubtree,31,4,"Copy \xE6\x9D\xB1"});
  request=gui.consumeSceneNodeAuthoringRequest();
  ASSERT_TRUE(request); EXPECT_EQ(request->name,"Copy \xE6\x9D\xB1");
  EXPECT_FALSE(gui.consumeSceneNodeAuthoringRequest());
}
} // namespace
