#include <Container/utility/GuiManager.h>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
using container::ui::GuiManager;
using container::ui::ModelLoadRequest;
using container::ui::ModelMergeRequest;
using container::ui::isGltfMergePath;

TEST(GuiModelMerge, DefaultsAndLegacyRequestsKeepReplacementSemantics) {
  const ModelMergeRequest merge;
  EXPECT_EQ(merge.parentName, "Imported scene"); EXPECT_FALSE(merge.incomingScene);
  EXPECT_EQ(merge.placementMatrix, glm::mat4(1));
  EXPECT_FALSE(merge.authoredPlacementMatrix);
  const ModelLoadRequest legacy{"old.gltf", .001f, "Existing model", false, {}};
  EXPECT_FALSE(legacy.merge); EXPECT_FALSE(legacy.unload); EXPECT_FLOAT_EQ(legacy.importScale, .001f);
  GuiManager gui;
  gui.queueModelLoadRequest("replace.ifcx", .001f, "Replace");
  const auto load = gui.consumeModelLoadRequest(); ASSERT_TRUE(load);
  EXPECT_EQ(load->path, "replace.ifcx"); EXPECT_EQ(load->label, "Replace");
  EXPECT_FLOAT_EQ(load->importScale, .001f); EXPECT_FALSE(load->merge);
  EXPECT_FALSE(gui.consumeModelLoadRequest());
}

TEST(GuiModelMerge, MergeRequestIsConsumableOnceWithoutFileIoOrAnImGuiContext) {
  GuiManager gui;
  const std::string path = "missing directory/scene.GLB";
  gui.queueModelMergeRequest(path);
  const auto request = gui.consumeModelLoadRequest(); ASSERT_TRUE(request); ASSERT_TRUE(request->merge);
  EXPECT_EQ(request->path, path); EXPECT_EQ(request->label, path);
  EXPECT_FALSE(request->unload); EXPECT_FALSE(request->openVdbOptions);
  EXPECT_EQ(request->merge->parentName, "Imported scene"); EXPECT_FALSE(request->merge->incomingScene);
  EXPECT_EQ(request->merge->placementMatrix, glm::mat4(1));
  EXPECT_FALSE(request->merge->authoredPlacementMatrix);
  EXPECT_FALSE(gui.consumeModelLoadRequest());
}

TEST(GuiModelMerge, NamedSourceSceneAndMirroredAffinePlacementArePreservedExactly) {
  GuiManager gui;
  auto placement = glm::translate(glm::mat4(1), {3, -2, 5});
  placement[0][0] = -2; placement[1][0] = .375f; placement[1][1] = 3; placement[2][2] = .5f;
  gui.queueModelMergeRequest("another.gltf", "Imported building B", std::numeric_limits<int>::max(), placement);
  const auto request = gui.consumeModelLoadRequest(); ASSERT_TRUE(request); ASSERT_TRUE(request->merge);
  EXPECT_EQ(request->merge->parentName, "Imported building B");
  EXPECT_EQ(request->merge->incomingScene, std::numeric_limits<int>::max());
  EXPECT_EQ(request->merge->placementMatrix, placement);
  EXPECT_FALSE(request->merge->authoredPlacementMatrix);
}

glm::mat4 runtimePlacement(const std::array<double, 16>& authored) {
  glm::mat4 runtime(1);
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      runtime[column][row] = static_cast<float>(authored[size_t(column * 4 + row)]);
  return runtime;
}

TEST(GuiModelMerge, AuthoredDecimalPrecisionSurvivesTheRuntimePlacementProjection) {
  GuiManager gui;
  const std::array<double, 16> authored{1.1, 0, 0, 0, .3, .9, 0, 0, 0, 0, 1.25, 0, .1, -.2, .7, 1};
  const auto runtime = runtimePlacement(authored);
  ASSERT_NE(double(runtime[3][0]), authored[12]); // The original decimal is not binary32-exact.
  gui.queueModelMergeRequest("precise.gltf", "Precise parent", 2, runtime, authored);
  const auto request = gui.consumeModelLoadRequest(); ASSERT_TRUE(request); ASSERT_TRUE(request->merge);
  ASSERT_TRUE(request->merge->authoredPlacementMatrix);
  EXPECT_EQ(*request->merge->authoredPlacementMatrix, authored);
  EXPECT_EQ(request->merge->placementMatrix, runtime);
  EXPECT_EQ(request->merge->incomingScene, 2);
  EXPECT_FALSE(gui.consumeModelLoadRequest());
}

TEST(GuiModelMerge, AuthoredRuntimeMismatchCannotReplaceAPendingPreciseMerge) {
  GuiManager gui;
  const std::array<double, 16> authored{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, .1, .2, .3, 1};
  const auto runtime = runtimePlacement(authored);
  gui.queueModelMergeRequest("keep.glb", "Keep precise", 3, runtime, authored);
  auto mismatch = runtime;
  mismatch[3][0] += .25f;
  EXPECT_THROW(gui.queueModelMergeRequest("incoming.gltf", "Mismatch", {}, mismatch, authored), std::invalid_argument);
  const auto request = gui.consumeModelLoadRequest(); ASSERT_TRUE(request); ASSERT_TRUE(request->merge);
  EXPECT_EQ(request->path, "keep.glb"); EXPECT_EQ(request->merge->parentName, "Keep precise");
  EXPECT_EQ(request->merge->incomingScene, 3); EXPECT_EQ(request->merge->placementMatrix, runtime);
  ASSERT_TRUE(request->merge->authoredPlacementMatrix);
  EXPECT_EQ(*request->merge->authoredPlacementMatrix, authored);
}

TEST(GuiModelMerge, InvalidAuthoredPrecisionCannotReplaceAPendingReplacement) {
  GuiManager gui;
  const std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  for (int malformed = 0; malformed < 7; ++malformed) {
    SCOPED_TRACE(malformed);
    auto authored = identity;
    if (malformed == 0) authored[12] = std::numeric_limits<double>::quiet_NaN();
    if (malformed == 1) authored[12] = std::numeric_limits<double>::infinity();
    if (malformed == 2) authored[3] = 1e-300; // Rounds to an affine runtime row, but is not authored affine.
    if (malformed == 3) authored[0] = 0.;
    if (malformed == 4) { authored[1] = authored[4] = 1.; authored[5] = 1.+1e-8; }
    if (malformed == 5) authored[0] = authored[5] = authored[10] = 1e20;
    if (malformed == 6) {
      // Straddle a float rounding midpoint: the runtime determinant is about
      // 1e-7, while the authored determinant is below the required 1e-12.
      const double midpoint = 1.+0x1p-24;
      authored[1] = 1.; authored[4] = midpoint-1e-14; authored[5] = midpoint+1e-14;
    }
    const auto runtime = malformed < 2 ? glm::mat4(1) : runtimePlacement(authored);
    gui.queueModelLoadRequest("keep.glb", .001f, "Keep replacement");
    EXPECT_THROW(gui.queueModelMergeRequest("incoming.gltf", "Invalid authored", {}, runtime, authored), std::invalid_argument);
    const auto request = gui.consumeModelLoadRequest(); ASSERT_TRUE(request);
    EXPECT_EQ(request->path, "keep.glb"); EXPECT_EQ(request->label, "Keep replacement");
    EXPECT_FLOAT_EQ(request->importScale, .001f); EXPECT_FALSE(request->merge);
  }
}

TEST(GuiModelMerge, UnsupportedPathsLeaveAnEarlierQueuedOperationIntact) {
  GuiManager gui;
  const std::array<std::string, 9> paths{"", "model.obj", "model.ifc", "model.bim", "model.vdb",
      "model.gltf.backup", "directory.glb/file.txt", "model.gltf/", std::string("wrong\0.glb", 10)};
  for (const auto& path : paths) {
    SCOPED_TRACE(path);
    gui.queueModelMergeRequest("earlier.glb", "Earlier parent", 2);
    EXPECT_THROW(gui.queueModelMergeRequest(path), std::invalid_argument);
    const auto request = gui.consumeModelLoadRequest(); ASSERT_TRUE(request); ASSERT_TRUE(request->merge);
    EXPECT_EQ(request->path, "earlier.glb"); EXPECT_EQ(request->merge->parentName, "Earlier parent");
    EXPECT_EQ(request->merge->incomingScene, 2);
  }
}

TEST(GuiModelMerge, InvalidParametersCannotOverwriteAPendingReplacement) {
  GuiManager gui;
  const auto expectReplacement = [&] {
    auto request = gui.consumeModelLoadRequest(); ASSERT_TRUE(request);
    EXPECT_EQ(request->path, "keep.glb"); EXPECT_EQ(request->label, "Keep");
    EXPECT_FLOAT_EQ(request->importScale, .001f); EXPECT_FALSE(request->merge);
  };
  for (const auto& name : {std::string{}, std::string(" \t\n"), std::string("Parent\0name", 11)}) {
    gui.queueModelLoadRequest("keep.glb", .001f, "Keep");
    EXPECT_THROW(gui.queueModelMergeRequest("incoming.gltf", name), std::invalid_argument);
    expectReplacement();
  }
  gui.queueModelLoadRequest("keep.glb", .001f, "Keep");
  EXPECT_THROW(gui.queueModelMergeRequest("incoming.gltf", "Parent", -1), std::invalid_argument);
  expectReplacement();
  for (int malformed = 0; malformed < 4; ++malformed) {
    SCOPED_TRACE(malformed); glm::mat4 matrix(1);
    if (malformed == 0) matrix[0][3] = .25f;
    if (malformed == 1) matrix[1][1] = 0;
    if (malformed == 2) matrix[3][0] = std::numeric_limits<float>::infinity();
    if (malformed == 3) matrix[0][0] = matrix[1][1] = matrix[2][2] = 1e20f;
    gui.queueModelLoadRequest("keep.glb", .001f, "Keep");
    EXPECT_THROW(gui.queueModelMergeRequest("incoming.gltf", "Parent", {}, matrix), std::invalid_argument);
    expectReplacement();
  }
}

TEST(GuiModelMerge, ReplacementUnloadAndMergeReplaceTheQueuedOperationWithoutPayloadLeakage) {
  GuiManager gui;
  gui.queueModelMergeRequest("incoming.gltf", "B", 1);
  gui.queueModelLoadRequest("replace.glb", 1, "Replace");
  auto request = gui.consumeModelLoadRequest(); ASSERT_TRUE(request);
  EXPECT_FALSE(request->merge); EXPECT_EQ(request->path, "replace.glb");
  gui.queueModelMergeRequest("incoming.gltf"); gui.queueModelUnloadRequest();
  request = gui.consumeModelLoadRequest(); ASSERT_TRUE(request);
  EXPECT_TRUE(request->unload); EXPECT_FALSE(request->merge); EXPECT_TRUE(request->path.empty());
  gui.queueModelUnloadRequest(); gui.queueModelMergeRequest("incoming.gltf");
  request = gui.consumeModelLoadRequest(); ASSERT_TRUE(request);
  EXPECT_FALSE(request->unload); ASSERT_TRUE(request->merge); EXPECT_EQ(request->path, "incoming.gltf");
}

TEST(GuiModelMerge, PickerAndRequestValidationAcceptOnlyGltfAndGlbExtensions) {
  for (const auto* path : {"sample.gltf", "sample.glb", "SAMPLE.GLTF", "folder/file.GlB", "folder with spaces/file.gltf"})
    EXPECT_TRUE(isGltfMergePath(path)) << path;
  for (const auto* path : {"", "sample", "sample.gltf.bak", "sample.ifcx", "sample.usd", "sample.vdb", "folder.glb/item"})
    EXPECT_FALSE(isGltfMergePath(path)) << path;
}
} // namespace
