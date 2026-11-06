#include <Container/utility/SceneNodeAuthoringPolicy.h>

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace container::scene;
constexpr auto invalid = SceneGraph::kInvalidNode;
constexpr auto rename = SceneNodeAuthoringPurpose::Rename;
constexpr auto create = SceneNodeAuthoringPurpose::CreateEmptyChild;

struct Fixture {
  SceneGraph graph;
  uint32_t editor, wrapper, source, primitive, procedural;
  Fixture() {
    editor = graph.createNode(glm::mat4(1), 0, false, invalid, "Renamed context");
    graph.getNode(editor)->editorSceneRoot = true;
    wrapper = graph.createNode(glm::mat4(1), 0, false, invalid, "Renamed import");
    graph.getNode(wrapper)->interactionRootBoundary = true;
    source = graph.createNode(glm::mat4(1), 0, false, invalid, "Authored mesh");
    graph.getNode(source)->sourceNodeIndex = 0;
    primitive = graph.createNode(glm::mat4(1), 0, true, 7, "Primitive 7");
    graph.getNode(primitive)->sourceMeshIndex = 0;
    graph.getNode(primitive)->sourcePrimitiveIndex = 0;
    graph.getNode(primitive)->meshOwnerGraphNode = source;
    procedural = graph.createNode(glm::mat4(1), 0, true, 8, "Procedural sphere");
    EXPECT_TRUE(graph.setParent(wrapper, editor));
    EXPECT_TRUE(graph.setParent(source, wrapper));
    EXPECT_TRUE(graph.setParent(primitive, source));
    EXPECT_TRUE(graph.setParent(procedural, editor));
    graph.setActiveRoots(std::array{source, procedural});
  }
};

std::vector<SceneNode> payloads(const SceneGraph& graph) {
  std::vector<SceneNode> result;
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) result.push_back(*graph.getNode(id));
  return result;
}

TEST(SceneNodeAuthoringPolicy, ValidUtf8LabelsKeepExactBytesAndWhitespace) {
  const std::vector<std::string> labels{
      "Room", "  Room  ", "\xC4\xB0" "stanbul", "\xE7\xA9\xBA" " node",
      "e\xCC\x81", "Light \xF0\x9F\x92\xA1", "\xF4\x8F\xBF\xBF"};
  for (const auto& label : labels) {
    const auto original = label;
    EXPECT_NO_THROW(ValidateSceneNodeName(label));
    EXPECT_EQ(label, original);
  }
}

TEST(SceneNodeAuthoringPolicy, NameBudgetCountsUtf8BytesAndRejectsZeroBudget) {
  SceneNodeAuthoringLimits limits; limits.maxNameBytes = 4;
  EXPECT_NO_THROW(ValidateSceneNodeName("ABCD", limits));
  EXPECT_NO_THROW(ValidateSceneNodeName("\xF0\x9F\x92\xA1", limits));
  EXPECT_THROW(ValidateSceneNodeName("\xF0\x9F\x92\xA1" " ", limits), std::invalid_argument);
  limits.maxNameBytes = 0;
  EXPECT_THROW(ValidateSceneNodeName("A", limits), std::invalid_argument);
}

TEST(SceneNodeAuthoringPolicy, RejectsBlankControlsMalformedOverlongAndInvalidUnicode) {
  const std::vector<std::string> labels{
      "", "   ", "\xC2\xA0\xE3\x80\x80", std::string("a\0b", 3), "a\nb", "a\tb",
      "a\x7F", "\xC2\x85", "\xE2\x80\xA8", "\xE2\x80\xA9",
      "\x80", "\xC0\x80", "\xC1\xBF", "\xE0\x80\xAF", "\xF0\x80\x80\xAF",
      "\xC2", "\xE2\x82", "\xF0\x9F\x92", "\xC2" "x",
      "\xED\xA0\x80", "\xED\xBF\xBF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFF"};
  for (const auto& label : labels) EXPECT_THROW(ValidateSceneNodeName(label), std::invalid_argument);
}

TEST(SceneNodeAuthoringPolicy, ImportedPrimitiveResolvesImmutableOwnerAfterReparenting) {
  Fixture fixture;
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.primitive, rename), fixture.source);
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.primitive, create), fixture.source);
  ASSERT_TRUE(fixture.graph.setParent(fixture.primitive, fixture.procedural));
  fixture.graph.setName(fixture.procedural, "Authored mesh");
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.primitive, rename), fixture.source);
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.source, rename), fixture.source);
}

TEST(SceneNodeAuthoringPolicy, DuplicateResolvesActualOwnerAndRejectsStructuralInactiveOrRetiredTargets) {
  Fixture fixture;
  constexpr auto duplicate=SceneNodeAuthoringPurpose::DuplicateSubtree;
  ASSERT_TRUE(fixture.graph.setParent(fixture.primitive,fixture.procedural));
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph,fixture.primitive,duplicate),fixture.source);
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph,fixture.procedural,duplicate),fixture.procedural);
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph,fixture.editor,duplicate),std::invalid_argument);
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph,fixture.wrapper,duplicate),std::invalid_argument);
  fixture.graph.setActiveRoots(std::array{fixture.procedural});
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph,fixture.primitive,duplicate),std::invalid_argument);
  fixture.graph.getNode(fixture.procedural)->retired=true;
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph,fixture.procedural,duplicate),std::invalid_argument);
}

TEST(SceneNodeAuthoringPolicy, ProtectsTaggedWrappersWithoutNameHeuristics) {
  Fixture fixture;
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.editor, rename), std::invalid_argument);
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.wrapper, rename), std::invalid_argument);
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.wrapper, create), std::invalid_argument);
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.editor, create), fixture.editor);
  fixture.graph.setName(fixture.source, "Import Scale");
  fixture.graph.setName(fixture.procedural, "Scene Root");
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.source, rename), fixture.source);
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.procedural, rename), fixture.procedural);
}

TEST(SceneNodeAuthoringPolicy, ProceduralAndSourceUnmappedAggregatePrimitivesStayEditable) {
  Fixture fixture;
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.procedural, rename), fixture.procedural);
  auto& aggregate = *fixture.graph.getNode(fixture.primitive);
  aggregate.meshOwnerGraphNode = fixture.wrapper;
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.primitive, rename), fixture.primitive);
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.primitive, create), fixture.primitive);
}

TEST(SceneNodeAuthoringPolicy, ActiveContextAndEmptySceneRemainUsableWhileInactiveTargetsReject) {
  Fixture fixture;
  fixture.graph.setActiveRoots(std::array{fixture.source});
  fixture.graph.setVisible(fixture.source, false);
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.source, rename), fixture.source);
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.procedural, rename), std::invalid_argument);
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.procedural, create), std::invalid_argument);
  fixture.graph.setActiveRoots(std::span<const uint32_t>{});
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.editor, create), fixture.editor);
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.source, create), std::invalid_argument);
  fixture.graph.getNode(fixture.editor)->sourceNodeIndex = 17;
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.editor, create), std::invalid_argument);
}

TEST(SceneNodeAuthoringPolicy, ExplicitPhysicsMetadataDoesNotForbidSafeRenameOrLeafAddition) {
  Fixture fixture;
  const std::array product{fixture.source}, body{fixture.procedural}, endpoint{fixture.procedural};
  SceneNodeAuthoringRestrictions restrictions;
  restrictions.physicsProductNodes = product;
  restrictions.physicsBodyNodes = body;
  restrictions.jointEndpointNodes = endpoint;
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.primitive, rename, restrictions), fixture.source);
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.procedural, rename, restrictions), fixture.procedural);
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.procedural, create, restrictions), fixture.procedural);
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.primitive, create, restrictions), std::invalid_argument);
  const std::array protectedNodes{fixture.procedural}; restrictions.protectedNodes = protectedNodes;
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.procedural, rename, restrictions), std::invalid_argument);
}

TEST(SceneNodeAuthoringPolicy, RejectsMissingOwnersStaleRestrictionsAndInvalidSourcesBeforeMutation) {
  Fixture fixture;
  const auto original = payloads(fixture.graph); const auto revision = fixture.graph.revision();
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, invalid, rename), std::invalid_argument);
  const std::array stale{invalid}; SceneNodeAuthoringRestrictions restrictions; restrictions.physicsBodyNodes = stale;
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.source, rename, restrictions), std::invalid_argument);
  EXPECT_EQ(payloads(fixture.graph), original); EXPECT_EQ(fixture.graph.revision(), revision);
  fixture.graph.getNode(fixture.primitive)->meshOwnerGraphNode = 999999;
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.primitive, rename), std::invalid_argument);
  fixture.graph.getNode(fixture.primitive)->meshOwnerGraphNode = fixture.source;
  fixture.graph.getNode(fixture.source)->sourceNodeIndex = -2;
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.primitive, rename), std::invalid_argument);
}

TEST(SceneNodeAuthoringPolicy, SubtreePreorderAndConstraintFlagsUseOnlyExplicitBindingsAndTags) {
  Fixture fixture;
  const auto original = payloads(fixture.graph); const auto revision = fixture.graph.revision();
  const std::array product{fixture.source}, body{fixture.primitive}, endpoint{fixture.procedural};
  SceneNodeAuthoringRestrictions restrictions;
  restrictions.physicsProductNodes = product; restrictions.physicsBodyNodes = body; restrictions.jointEndpointNodes = endpoint;
  const auto source = CollectSceneNodeAuthoringSubtree(fixture.graph, fixture.source, restrictions);
  EXPECT_EQ(source.nodes, (std::vector<uint32_t>{fixture.source, fixture.primitive}));
  EXPECT_FALSE(source.containsProtectedNodes); EXPECT_TRUE(source.containsPhysicsProductNodes);
  EXPECT_TRUE(source.containsPhysicsBodies); EXPECT_FALSE(source.containsJointEndpoints);
  const auto all = CollectSceneNodeAuthoringSubtree(fixture.graph, fixture.editor, restrictions);
  EXPECT_EQ(all.nodes, (std::vector<uint32_t>{fixture.editor, fixture.wrapper, fixture.source, fixture.primitive, fixture.procedural}));
  EXPECT_TRUE(all.containsProtectedNodes); EXPECT_TRUE(all.containsJointEndpoints);
  const auto ordinary = CollectSceneNodeAuthoringSubtree(fixture.graph, fixture.source);
  EXPECT_FALSE(ordinary.containsPhysicsProductNodes); EXPECT_FALSE(ordinary.containsPhysicsBodies);
  EXPECT_EQ(payloads(fixture.graph), original); EXPECT_EQ(fixture.graph.revision(), revision);
}

TEST(SceneNodeAuthoringPolicy, SubtreeRejectsDuplicateCycleMissingAndInconsistentTopology) {
  for (int mutation = 0; mutation < 5; ++mutation) {
    Fixture fixture;
    switch (mutation) {
      case 0: fixture.graph.getNode(fixture.source)->children.push_back(fixture.primitive); break;
      case 1: fixture.graph.getNode(fixture.source)->children.push_back(invalid); break;
      case 2: fixture.graph.getNode(fixture.primitive)->parent = fixture.editor; break;
      case 3:
        fixture.graph.getNode(fixture.primitive)->children.push_back(fixture.source);
        fixture.graph.getNode(fixture.source)->parent = fixture.primitive; break;
      case 4: fixture.graph.getNode(fixture.wrapper)->children.clear(); break;
    }
    const auto before = payloads(fixture.graph);
    EXPECT_THROW(CollectSceneNodeAuthoringSubtree(fixture.graph, fixture.source), std::invalid_argument);
    EXPECT_EQ(payloads(fixture.graph), before);
  }
}

TEST(SceneNodeAuthoringPolicy, BudgetsIncludeGraphScratchEdgesActiveViewsAndExplicitMetadata) {
  Fixture fixture; SceneNodeAuthoringLimits limits;
  limits.maxVisitedNodes = fixture.graph.nodeCount()-1;
  EXPECT_THROW(CollectSceneNodeAuthoringSubtree(fixture.graph, fixture.source, {}, limits), std::invalid_argument);
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.source, rename, {}, limits), std::invalid_argument);
  limits.maxVisitedNodes = fixture.graph.nodeCount(); limits.maxVisitedEdges = 3;
  EXPECT_THROW(CollectSceneNodeAuthoringSubtree(fixture.graph, fixture.source, {}, limits), std::invalid_argument);
  limits.maxVisitedEdges = 4; // Four topology edges plus two active-view roots.
  EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.source, rename, {}, limits), std::invalid_argument);
  limits.maxVisitedEdges = 6;
  EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.source, rename, {}, limits), fixture.source);
  const std::vector<uint32_t> repeated(6, fixture.source);
  SceneNodeAuthoringRestrictions restrictions; restrictions.protectedNodes = repeated;
  EXPECT_THROW(CollectSceneNodeAuthoringSubtree(fixture.graph, fixture.source, restrictions, limits), std::invalid_argument);
  limits.maxVisitedEdges = 0;
  EXPECT_THROW(CollectSceneNodeAuthoringSubtree(fixture.graph, fixture.source, {}, limits), std::invalid_argument);
}

TEST(SceneNodeAuthoringPolicy, TraversesTwentyThousandWideAndTenThousandDeepNodesIteratively) {
  for (const auto [count, deep] : {std::pair{20001u, false}, std::pair{10000u, true}}) {
    SceneGraph empty; std::vector<SceneNode> tail(count);
    for (uint32_t id = 1; id < count; ++id) {
      tail[id].parent = deep ? id-1 : 0;
      tail[tail[id].parent].children.push_back(id);
    }
    auto graph = empty.makeAppendCandidate(std::move(tail));
    const std::array endpoint{count-1}; SceneNodeAuthoringRestrictions restrictions;
    restrictions.jointEndpointNodes = endpoint;
    const auto collected = CollectSceneNodeAuthoringSubtree(graph, 0, restrictions);
    ASSERT_EQ(collected.nodes.size(), count);
    for (uint32_t id = 0; id < count; ++id) EXPECT_EQ(collected.nodes[id], id);
    EXPECT_TRUE(collected.containsJointEndpoints);
  }
}
TEST(SceneNodeAuthoringPolicy, RetirementResolvesOwnerAndRejectsRetiredTargetsWithoutMutation) {
  Fixture fixture;
  const auto subtree = ValidateSceneNodeRetirement(fixture.graph, fixture.primitive);
  EXPECT_EQ(subtree.nodes, (std::vector<uint32_t>{fixture.source, fixture.primitive}));
  auto prepared = fixture.graph.prepareRetireSubtree(subtree.nodes.front());
  ASSERT_TRUE(fixture.graph.publishRetirement(prepared));
  const auto before = payloads(fixture.graph);
  for (const auto purpose : {rename, create, SceneNodeAuthoringPurpose::DeleteSubtree})
    EXPECT_THROW(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.primitive, purpose), std::invalid_argument);
  EXPECT_THROW(ValidateSceneNodeRetirement(fixture.graph, fixture.source), std::invalid_argument);
  EXPECT_EQ(payloads(fixture.graph), before);
}

TEST(SceneNodeAuthoringPolicy, RetirementRejectsExplicitProductBodyJointSkinAndProtectedDescendants) {
  Fixture fixture;
  const auto before = payloads(fixture.graph);
  const std::array bound{fixture.primitive};
  for (const int kind : {0, 1, 2, 3, 4}) {
    SceneNodeAuthoringRestrictions restrictions;
    if (kind == 0) restrictions.protectedNodes = bound;
    if (kind == 1) restrictions.physicsProductNodes = bound;
    if (kind == 2) restrictions.physicsBodyNodes = bound;
    if (kind == 3) restrictions.jointEndpointNodes = bound;
    if (kind == 4) restrictions.skinDependencyNodes = bound;
    EXPECT_THROW(ValidateSceneNodeRetirement(fixture.graph, fixture.source, restrictions), std::invalid_argument);
    // A metadata rename still does not detach or invalidate body/skin bindings.
    if (kind != 0) EXPECT_EQ(ResolveSceneNodeAuthoringTarget(fixture.graph, fixture.source, rename, restrictions), fixture.source);
  }
  EXPECT_THROW(ValidateSceneNodeRetirement(fixture.graph, fixture.editor), std::invalid_argument);
  EXPECT_THROW(ValidateSceneNodeRetirement(fixture.graph, fixture.wrapper), std::invalid_argument);
  EXPECT_EQ(payloads(fixture.graph), before);
}

} // namespace
