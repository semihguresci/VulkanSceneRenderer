#include <Container/geometry/GltfCameraAuthoring.h>
#include <Container/geometry/GltfAnimation.h>

#include <gtest/gtest.h>

// This executable has one translation unit; the shared allocation controls
// verify both positive/stale final guards and every fallible preparation step.
#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {
using namespace container::geometry::gltf;
using P = GltfCameraProperty;
using V = tinygltf::Value;
using O = V::Object;
using A = V::Array;

tinygltf::Camera perspective() {
  tinygltf::Camera c; c.type = "perspective"; c.name = "Shared rolled camera";
  c.perspective.yfov = .8; c.perspective.znear = .1;
  c.extras = V(O{{"opaqueIndex", V(909)}, {"signedZero", V(-0.)}, {"note", V(std::string("keep"))}});
  c.extensions["VENDOR_camera"] = V(O{{"index", V(811)}, {"payload", V(A{V(1), V(2.)})}});
  c.perspective.extensions["VENDOR_lens"] = V(O{{"optics", V(std::string("anamorphic"))}});
  c.orthographic.extensions["VENDOR_dormant"] = V(O{{"keep", V(true)}});
  c.perspective.extras = V(O{{"lens", V(127)}}); c.orthographic.extras = V(std::string("dormant block"));
  c.extras_json_string = "{\"original\":true}"; c.extensions_json_string = "camera raw extensions";
  c.perspective.extras_json_string = "perspective raw extras";
  c.perspective.extensions_json_string = "perspective raw extensions";
  c.orthographic.extras_json_string = "orthographic raw extras";
  c.orthographic.extensions_json_string = "orthographic raw extensions";
  return c;
}
tinygltf::Camera orthographic() {
  auto c = perspective(); c.type = "orthographic";
  c.orthographic.xmag = 7.; c.orthographic.ymag = 3.; c.orthographic.znear = 0.; c.orthographic.zfar = 50.;
  return c;
}
GltfCameraProjectionPatch number(P p, double value) { return {{}, {{p, value}}}; }
struct Fixture {
  tinygltf::Model authored, evaluated;
  Fixture() {
    authored.cameras = {perspective(), orthographic()};
    evaluated.cameras = {orthographic(), orthographic(), perspective()};
    authored.nodes.resize(4); evaluated.nodes.resize(4);
    authored.nodes[0].camera = authored.nodes[2].camera = 0;
    evaluated.nodes[1].camera = evaluated.nodes[3].camera = 2;
    authored.nodes[0].rotation = {0, 0, .5, .8660254037844386};
    evaluated.nodes[1].rotation = authored.nodes[0].rotation;
    authored.cameras[0].perspective.aspectRatio = 1.25;
    evaluated.cameras[2].perspective.aspectRatio = 1.75;
    evaluated.cameras[2].perspective.yfov = .9;
    evaluated.cameras[2].name = "Evaluated metadata stays separate";
    evaluated.cameras[2].extras_json_string = "evaluated raw extras";
  }
  GltfCameraProjectionPlan plan(const GltfCameraProjectionPatch& patch = {}, const GltfCameraAuthoringLimits& limits = {}) const {
    return PlanGltfCameraProjectionEdit(GltfCameraSourceView(authored), GltfCameraSourceView(evaluated), 0, 2, patch, limits);
  }
  bool matches(const GltfCameraProjectionPlan& p) const noexcept {
    return GltfCameraProjectionEditMatches(p, GltfCameraSourceView(authored), GltfCameraSourceView(evaluated));
  }
};
void pointer(tinygltf::Model& m, std::string path) {
  if (m.animations.empty()) m.animations.emplace_back();
  auto& animation = m.animations[0]; animation.samplers.emplace_back();
  tinygltf::AnimationChannel channel; channel.target_path = "pointer";
  channel.sampler = int(animation.samplers.size() - 1);
  channel.target_extensions["KHR_animation_pointer"] = V(O{{"pointer", V(std::move(path))}});
  channel.target_extras = V(O{{"preserve", V(91)}});
  channel.target_extensions_json_string = "original pointer raw cache";
  animation.channels.push_back(std::move(channel));
}
void reject(const Fixture& f, const GltfCameraProjectionPatch& p) {
  EXPECT_THROW((void)f.plan(p), std::invalid_argument);
}
} // namespace

TEST(GltfCameraAuthoring, ExplicitDomainsPreserveMetadataSharedNodesAndIndependentUntouchedValues) {
  Fixture f; const auto a = f.authored.cameras[0], e = f.evaluated.cameras[2];
  const auto plan = f.plan(number(P::PerspectiveNear, .25));
  EXPECT_EQ(plan.authoredCamera, 0); EXPECT_EQ(plan.evaluatedCamera, 2);
  EXPECT_EQ(plan.authoredNodes, (std::vector<uint32_t>{0, 2}));
  EXPECT_EQ(plan.evaluatedNodes, (std::vector<uint32_t>{1, 3}));
  EXPECT_DOUBLE_EQ(plan.authoredAfter.perspective.znear, .25);
  EXPECT_DOUBLE_EQ(plan.evaluatedAfter.perspective.znear, .25);
  EXPECT_DOUBLE_EQ(plan.authoredAfter.perspective.yfov, .8);
  EXPECT_DOUBLE_EQ(plan.evaluatedAfter.perspective.yfov, .9);
  EXPECT_DOUBLE_EQ(plan.authoredAfter.perspective.aspectRatio, 1.25);
  EXPECT_DOUBLE_EQ(plan.evaluatedAfter.perspective.aspectRatio, 1.75);
  EXPECT_EQ(plan.authoredAfter.extensions, a.extensions);
  EXPECT_EQ(plan.authoredAfter.perspective.extensions, a.perspective.extensions);
  EXPECT_EQ(plan.authoredAfter.orthographic.extensions, a.orthographic.extensions);
  EXPECT_EQ(plan.authoredAfter.extras_json_string, a.extras_json_string);
  EXPECT_EQ(plan.evaluatedAfter.extras_json_string, e.extras_json_string);
  EXPECT_EQ(plan.authoredAfter.perspective.extensions_json_string, a.perspective.extensions_json_string);
  EXPECT_EQ(plan.authoredAfter.orthographic.extras_json_string, a.orthographic.extras_json_string);
  EXPECT_EQ(std::bit_cast<uint64_t>(plan.authoredAfter.extras.Get("signedZero").Get<double>()), std::bit_cast<uint64_t>(-0.));
  EXPECT_EQ(plan.changedProperties, (std::vector<P>{P::PerspectiveNear}));
  EXPECT_TRUE(plan.changed); EXPECT_FALSE(plan.changedType); EXPECT_TRUE(f.matches(plan));
  EXPECT_DOUBLE_EQ(f.authored.cameras[0].perspective.znear, a.perspective.znear);
  EXPECT_EQ(f.authored.nodes[0].rotation, (std::vector<double>{0, 0, .5, .8660254037844386}));
}

TEST(GltfCameraAuthoring, OmittedFarAndAspectAreExplicitAndNeverMaterializeEvaluatorFallback) {
  Fixture f; f.authored.cameras[0].perspective.aspectRatio = f.evaluated.cameras[2].perspective.aspectRatio = 0;
  auto unchanged = f.plan(number(P::PerspectiveNear, .2));
  EXPECT_DOUBLE_EQ(unchanged.authoredAfter.perspective.zfar, 0);
  EXPECT_DOUBLE_EQ(unchanged.evaluatedAfter.perspective.aspectRatio, 0);
  GltfCameraProjectionPatch fixed{{}, {{P::PerspectiveFar, 1200.}, {P::PerspectiveAspectRatio, 2.}}};
  auto plan = f.plan(fixed);
  EXPECT_DOUBLE_EQ(plan.authoredAfter.perspective.zfar, 1200.);
  EXPECT_DOUBLE_EQ(plan.evaluatedAfter.perspective.aspectRatio, 2.);
  f.authored.cameras[0] = plan.authoredAfter; f.evaluated.cameras[2] = plan.evaluatedAfter;
  auto omitted = f.plan({{}, {{P::PerspectiveFar, std::monostate{}}, {P::PerspectiveAspectRatio, std::monostate{}}}});
  EXPECT_DOUBLE_EQ(omitted.authoredAfter.perspective.zfar, 0);
  EXPECT_DOUBLE_EQ(omitted.evaluatedAfter.perspective.aspectRatio, 0);
  EXPECT_DOUBLE_EQ(omitted.authoredBefore.perspective.zfar, 1200.);
  reject(f, number(P::PerspectiveFar, 0)); reject(f, number(P::PerspectiveAspectRatio, -0.));
}

TEST(GltfCameraAuthoring, OrthographicAllowsZeroNearAndKeepsIndependentMagnitudes) {
  Fixture f; f.authored.cameras[0] = f.evaluated.cameras[2] = orthographic();
  auto plan = f.plan(number(P::OrthographicYmag, 2.));
  EXPECT_DOUBLE_EQ(plan.authoredAfter.orthographic.znear, 0);
  EXPECT_DOUBLE_EQ(plan.authoredAfter.orthographic.xmag, 7);
  EXPECT_DOUBLE_EQ(plan.evaluatedAfter.orthographic.ymag, 2);
  EXPECT_TRUE(f.matches(plan));
  reject(f, number(P::OrthographicXmag, -1)); reject(f, number(P::OrthographicNear, -1));
  reject(f, number(P::OrthographicFar, 0));
}

TEST(GltfCameraAuthoring, ProjectionSwitchRequiresValidDestinationAndPreservesDormantMetadata) {
  Fixture f;
  GltfCameraProjectionPatch patch{GltfCameraProjection::Orthographic,
      {{P::OrthographicXmag, 8.}, {P::OrthographicYmag, 3.}, {P::OrthographicNear, 0.}, {P::OrthographicFar, 20.}}};
  auto plan = f.plan(patch);
  EXPECT_EQ(plan.authoredAfter.type, "orthographic"); EXPECT_TRUE(plan.changedType);
  EXPECT_EQ(plan.authoredAfter.orthographic.extensions_json_string, f.authored.cameras[0].orthographic.extensions_json_string);
  EXPECT_DOUBLE_EQ(plan.authoredAfter.perspective.yfov, .8);
  EXPECT_DOUBLE_EQ(plan.evaluatedAfter.perspective.yfov, .9);
  EXPECT_TRUE(f.matches(plan));
  reject(f, {GltfCameraProjection::Orthographic, {{P::OrthographicXmag, 8.}}});
  reject(f, {GltfCameraProjection::Orthographic, {{P::PerspectiveNear, .2}}});
  f.authored.cameras[0].orthographic = orthographic().orthographic;
  f.evaluated.cameras[2].orthographic = orthographic().orthographic;
  reject(f, {GltfCameraProjection::Orthographic, {}});
  reject(f, {GltfCameraProjection::Orthographic, {{P::OrthographicXmag, 8.}, {P::OrthographicYmag, 3.}, {P::OrthographicFar, 20.}}});
  f.authored.cameras[0] = f.evaluated.cameras[2] = orthographic();
  reject(f, {GltfCameraProjection::Perspective, {}});
  auto perspectivePlan = f.plan({GltfCameraProjection::Perspective,
      {{P::PerspectiveYfov, .7}, {P::PerspectiveNear, .2}, {P::PerspectiveFar, std::monostate{}},
       {P::PerspectiveAspectRatio, std::monostate{}}}});
  EXPECT_EQ(perspectivePlan.authoredAfter.type, "perspective");
  EXPECT_DOUBLE_EQ(perspectivePlan.authoredAfter.perspective.zfar, 0);
}

TEST(GltfCameraAuthoring, InvalidProjectionNumbersAndCollapsedBinary32IntervalsReject) {
  Fixture f;
  for (double v : {0., -1., std::numbers::pi, std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::denorm_min()}) reject(f, number(P::PerspectiveYfov, v));
  for (double v : {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max()})
    reject(f, number(P::PerspectiveNear, v));
  reject(f, number(P::PerspectiveFar, .05));
  reject(f, {{}, {{P::PerspectiveNear, 1.}, {P::PerspectiveFar, std::nextafter(1., 2.)}}});
  reject(f, number(P::PerspectiveAspectRatio, std::numeric_limits<double>::denorm_min()));
  f.authored.cameras[0] = f.evaluated.cameras[2] = orthographic();
  reject(f, number(P::OrthographicXmag, std::numeric_limits<double>::max()));
  reject(f, {{}, {{P::OrthographicNear, 1.}, {P::OrthographicFar, std::nextafter(1., 2.)}}});
}

TEST(GltfCameraAuthoring, DuplicateUnknownCrossProjectionAndRequiredOmissionEditsReject) {
  Fixture f;
  reject(f, {{}, {{P::PerspectiveNear, .2}, {P::PerspectiveNear, .3}}});
  reject(f, {{}, {{P::Count, .2}}});
  reject(f, {{}, {{P::PerspectiveYfov, std::monostate{}}}});
  reject(f, number(P::OrthographicXmag, 2));
  reject(f, {GltfCameraProjection(99), {}});
}

TEST(GltfCameraAuthoring, InvalidSelectedIdentitiesAssociationsAndResidentProjectionReject) {
  Fixture f;
  EXPECT_THROW((void)PlanGltfCameraProjectionEdit(GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated), -1, 2, {}), std::invalid_argument);
  EXPECT_THROW((void)PlanGltfCameraProjectionEdit(GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated), 0, 3, {}), std::invalid_argument);
  f.authored.nodes[1].camera = 99; reject(f, {});
  f.authored.nodes[1].camera = -1; f.authored.cameras[0].perspective.yfov = 0; reject(f, {});
  f.authored.cameras[0] = perspective(); f.evaluated.cameras[2].type = "unknown"; reject(f, {});
}

TEST(GltfCameraAuthoring, AnimatedChangesEditRestAndKeepCurrentSampleWhileTypeSwitchesReject) {
  Fixture f; pointer(f.authored, "/cameras/0/perspective/yfov"); pointer(f.evaluated, "/cameras/2/perspective/yfov");
  const auto edited = f.plan(number(P::PerspectiveYfov, .7));
  EXPECT_TRUE(edited.changed); EXPECT_DOUBLE_EQ(edited.authoredAfter.perspective.yfov, .7);
  EXPECT_DOUBLE_EQ(edited.evaluatedAfter.perspective.yfov, .9); EXPECT_TRUE(f.matches(edited));
  auto other = f.plan(number(P::PerspectiveNear, .2));
  EXPECT_EQ(other.authoredAnimationBindings, (std::vector<GltfCameraAnimationBinding>{{0, 0, P::PerspectiveYfov}}));
  EXPECT_TRUE(f.matches(other));
  f.evaluated.cameras[2].perspective.yfov = .8;
  auto noop = f.plan(number(P::PerspectiveYfov, .8));
  EXPECT_FALSE(noop.changed); EXPECT_TRUE(noop.changedProperties.empty()); EXPECT_TRUE(f.matches(noop));
  GltfCameraProjectionPatch switchType{GltfCameraProjection::Orthographic,
      {{P::OrthographicXmag, 2.}, {P::OrthographicYmag, 2.}, {P::OrthographicNear, 0.}, {P::OrthographicFar, 30.}}};
  reject(f, switchType);
}

TEST(GltfCameraAuthoring, EvaluatedOnlyAnimationKeepsTheSampleForAnAuthoredRestNoop) {
  Fixture f; pointer(f.evaluated, "/cameras/2/perspective/yfov");
  const auto noop = f.plan(number(P::PerspectiveYfov, .8));
  EXPECT_FALSE(noop.changed); EXPECT_TRUE(noop.changedProperties.empty());
  EXPECT_DOUBLE_EQ(noop.evaluatedAfter.perspective.yfov, .9);
  auto valid = f.plan(number(P::PerspectiveNear, .3));
  EXPECT_TRUE(valid.authoredAnimationBindings.empty()); ASSERT_EQ(valid.evaluatedAnimationBindings.size(), 1u);
  EXPECT_DOUBLE_EQ(valid.evaluatedAfter.perspective.yfov, .9);
}

TEST(GltfCameraAuthoring, AnimatedOptionalFieldsCannotLoseTheirAuthoredDefault) {
  for (const auto property : {P::PerspectiveAspectRatio, P::PerspectiveFar}) {
    Fixture f;
    f.authored.cameras[0].perspective.zfar = 100.;
    f.evaluated.cameras[2].perspective.zfar = 120.;
    const auto suffix = std::string(GltfCameraPropertyPath(property));
    pointer(f.authored, "/cameras/0/" + suffix); pointer(f.evaluated, "/cameras/2/" + suffix);
    reject(f, {{}, {{property, std::monostate{}}}});
    const auto changed = f.plan(number(property, property == P::PerspectiveFar ? 80. : 1.5));
    EXPECT_TRUE(changed.changed); EXPECT_TRUE(f.matches(changed));
  }
}

TEST(GltfCameraAuthoring, AnimatedRestValidationUsesAuthoredPlanesAndKeepsTheCurrentProjection) {
  Fixture f;
  f.authored.cameras[0].perspective.zfar = 100.;
  f.evaluated.cameras[2].perspective.znear = 80.;
  f.evaluated.cameras[2].perspective.zfar = 100.;
  for (const auto* property : {"perspective/znear", "perspective/zfar"}) {
    pointer(f.authored, std::string("/cameras/0/") + property);
    pointer(f.evaluated, std::string("/cameras/2/") + property);
  }
  const auto plan = f.plan(number(P::PerspectiveFar, 50.));
  EXPECT_DOUBLE_EQ(plan.authoredAfter.perspective.znear, .1);
  EXPECT_DOUBLE_EQ(plan.authoredAfter.perspective.zfar, 50.);
  EXPECT_DOUBLE_EQ(plan.evaluatedAfter.perspective.znear, 80.);
  EXPECT_DOUBLE_EQ(plan.evaluatedAfter.perspective.zfar, 100.);
  EXPECT_TRUE(f.matches(plan));
}

TEST(GltfCameraAuthoring, UnsupportedSelectedPointerSchemasAndMalformedTargetsFailClosed) {
  for (const auto* path : {"/cameras/0/perspective/yfov/0", "/cameras/0/perspective/yfov/future",
      "/cameras/0/name", "/cameras/00/perspective/yfov", "/cameras/999/perspective/yfov",
      "/cameras/0/perspective/zfar", "/cameras/0/orthographic/xmag", "/cameras/0", "cameras/0/perspective/yfov"}) {
    Fixture f; pointer(f.authored, path); reject(f, number(P::PerspectiveNear, .2));
  }
  Fixture f; pointer(f.authored, "/cameras/0/perspective/yfov");
  f.authored.animations[0].channels[0].target_extensions.at("KHR_animation_pointer") = V(2);
  reject(f, {});
}

TEST(GltfCameraAuthoring, BeforeProofIncludesOneUlpSignedZeroOpaqueTypesAndAllRawCaches) {
  const std::array<std::function<void(tinygltf::Camera&)>, 7> mutate{
      [](auto& c) { c.perspective.yfov = std::nextafter(c.perspective.yfov, 1.); },
      [](auto& c) { c.perspective.zfar = -0.; },
      [](auto& c) { c.extras.Get<O>()["opaqueIndex"] = V(909.); },
      [](auto& c) { c.extras.Get<O>()["signedZero"] = V(0.); },
      [](auto& c) { c.extensions_json_string += " changed"; },
      [](auto& c) { c.perspective.extras_json_string += " changed"; },
      [](auto& c) { c.orthographic.extensions_json_string += " changed"; }};
  for (const auto& change : mutate) {
    Fixture f; auto plan = f.plan(number(P::PerspectiveNear, .2)); change(f.authored.cameras[0]);
    EXPECT_FALSE(f.matches(plan));
    EXPECT_THROW(ValidateGltfCameraProjectionEditBefore(plan, GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated)), std::invalid_argument);
  }
}

TEST(GltfCameraAuthoring, SharedAssociationProofRejectsSameCountRebindingAndIgnoresUnrelatedPose) {
  Fixture f; auto plan = f.plan(number(P::PerspectiveNear, .2));
  f.authored.nodes[0].translation = {50, 60, 70};
  f.authored.cameras[1].name = "Unrelated camera metadata changed";
  EXPECT_TRUE(f.matches(plan));
  f.authored.nodes[0].camera = -1; f.authored.nodes[1].camera = 0;
  EXPECT_FALSE(f.matches(plan));
  f.authored.nodes[1].camera = -1; f.authored.nodes[0].camera = 0;
  f.authored.cameras.push_back(perspective()); EXPECT_FALSE(f.matches(plan));
}

TEST(GltfCameraAuthoring, RelevantRawAnimationChangesInvalidateAndUnrelatedNodeAnimationSurvives) {
  Fixture f; pointer(f.authored, "/cameras/0/perspective/yfov");
  auto& animation = f.authored.animations[0]; tinygltf::AnimationChannel node;
  node.target_path = "rotation"; node.target_node = 0; node.sampler = 0; animation.channels.push_back(node);
  auto plan = f.plan(number(P::PerspectiveNear, .2));
  animation.channels[1].target_node = 2; EXPECT_TRUE(f.matches(plan));
  animation.channels[0].target_extensions_json_string += " changed"; EXPECT_FALSE(f.matches(plan));
  animation.channels[0].target_extensions_json_string = "original pointer raw cache";
  pointer(f.authored, "/cameras/0/perspective/znear"); EXPECT_FALSE(f.matches(plan));
}

TEST(GltfCameraAuthoring, EveryPublicPlanFieldRequiresImmutableReceiptAndCopiesRemainValid) {
  Fixture f; const auto original = f.plan(number(P::PerspectiveNear, .2));
  EXPECT_TRUE(f.matches(GltfCameraProjectionPlan(original)));
  EXPECT_FALSE(f.matches(GltfCameraProjectionPlan{}));
  const std::array<std::function<void(GltfCameraProjectionPlan&)>, 10> mutate{
      [](auto& p) { p.authoredCamera = 1; }, [](auto& p) { p.evaluatedCamera = 0; },
      [](auto& p) { p.authoredAfter.perspective.znear = .3; },
      [](auto& p) { p.evaluatedBefore.extras_json_string += "x"; },
      [](auto& p) { p.authoredNodes[0] = 1; },
      [](auto& p) { p.evaluatedAnimationBindings.push_back({0, 0, P::PerspectiveNear}); },
      [](auto& p) { p.changedProperties.clear(); }, [](auto& p) { p.changedType = true; },
      [](auto& p) { p.changed = false; }, [](auto& p) { ++p.peakOwnedBytes; }};
  for (const auto& change : mutate) { auto copy = original; change(copy); EXPECT_FALSE(f.matches(copy)); }
}

TEST(GltfCameraAuthoring, FiniteBudgetsCoverRecordsSharedUsesAnimationAndOwnedMetadata) {
  Fixture f; pointer(f.authored, "/cameras/0/perspective/yfov");
  const auto plan = f.plan(number(P::PerspectiveNear, .2));
  GltfCameraAuthoringLimits exact; exact.maxOwnedBytes = plan.peakOwnedBytes;
  EXPECT_TRUE(f.matches(f.plan(number(P::PerspectiveNear, .2), exact)));
  const std::array<std::function<void(GltfCameraAuthoringLimits&)>, 9> tighten{
      [](auto& l) { l.maxOwnedBytes = 1; }, [](auto& l) { l.maxMetadataValues = 1; },
      [](auto& l) { l.maxMetadataDepth = 0; }, [](auto& l) { l.maxCameraRecords = 1; },
      [](auto& l) { l.maxNodes = 1; }, [](auto& l) { l.maxAnimationClips = 0; },
      [](auto& l) { l.maxAnimationChannels = 0; }, [](auto& l) { l.maxPointerBytes = 8; },
      [](auto& l) { l.maxPatchOperations = 0; }};
  for (const auto& change : tighten) {
    GltfCameraAuthoringLimits limits; change(limits);
    EXPECT_THROW((void)f.plan(number(P::PerspectiveNear, .2), limits), std::invalid_argument);
  }
  auto reduced = exact; --reduced.maxOwnedBytes;
  EXPECT_THROW((void)f.plan(number(P::PerspectiveNear, .2), reduced), std::invalid_argument);
  EXPECT_DOUBLE_EQ(f.authored.cameras[0].perspective.znear, .1);
}

TEST(GltfCameraAuthoring, MetadataLimitsRejectInactiveBackingNonfiniteValuesAndExcessDepth) {
  Fixture f; f.authored.cameras[0].extras = V(7); f.authored.cameras[0].extras.Get<A>().push_back(V(1));
  reject(f, {});
  f.authored.cameras[0] = perspective(); f.authored.cameras[0].extras = V(std::numeric_limits<double>::infinity()); reject(f, {});
  f.authored.cameras[0] = perspective(); V nested(1);
  for (int i = 0; i < 12; ++i) nested = V(A{nested});
  f.authored.cameras[0].extras = std::move(nested); GltfCameraAuthoringLimits limits; limits.maxMetadataDepth = 8;
  EXPECT_THROW((void)f.plan({}, limits), std::invalid_argument);
  limits.maxMetadataDepth = 257; EXPECT_THROW((void)f.plan({}, limits), std::invalid_argument);
}

TEST(GltfCameraAuthoring, SelectedStorageDoesNotCopyUnrelatedBinaryOrCameraMetadata) {
  Fixture f; const auto baseline = f.plan(number(P::PerspectiveNear, .2));
  f.authored.buffers.emplace_back(); f.authored.buffers[0].data.resize(1024 * 1024, 73);
  f.authored.cameras[1].extras = V(std::string(1024 * 1024, 'x'));
  const auto* bytes = f.authored.buffers[0].data.data();
  auto plan = f.plan(number(P::PerspectiveNear, .2));
  EXPECT_EQ(plan.peakOwnedBytes, baseline.peakOwnedBytes);
  EXPECT_EQ(f.authored.buffers[0].data.data(), bytes); EXPECT_EQ(f.authored.buffers[0].data[12345], 73);
  EXPECT_TRUE(f.matches(plan));
}

TEST(GltfCameraAuthoring, PositiveStaleAndTamperedBeforeGuardsAllocateNothing) {
  Fixture f; pointer(f.authored, "/cameras/0/perspective/yfov");
  auto plan = f.plan(number(P::PerspectiveNear, .2)); auto tampered = plan; tampered.authoredAfter.perspective.znear = .4;
  bool valid{}, invalid{}, stale{}; size_t attempts{};
  {
    material_publication_test::AllocationGuard deny(true);
    valid = f.matches(plan); invalid = f.matches(tampered);
    f.authored.cameras[0].perspective.znear = .3; stale = f.matches(plan); attempts = deny.allocations();
  }
  EXPECT_TRUE(valid); EXPECT_FALSE(invalid); EXPECT_FALSE(stale); EXPECT_EQ(attempts, 0u);
}

TEST(GltfCameraAuthoring, EveryDeniedPreparationAllocationLeavesBothSourceRecordsUnchanged) {
  Fixture f; pointer(f.authored, "/cameras/0/perspective/yfov");
  const auto patch = number(P::PerspectiveNear, .2); const auto witness = f.plan();
  size_t allocations{};
  {
    material_publication_test::AllocationGuard count(false);
    auto plan = f.plan(patch); count.disable(); allocations = count.allocations();
  }
  ASSERT_GT(allocations, 0u); ASSERT_LT(allocations, 2000u);
  for (size_t allowance = 0; allowance < allocations; ++allowance) {
    bool denied{};
    {
      material_publication_test::AllocationGuard guard(false, allowance);
      try { (void)f.plan(patch); } catch (const std::bad_alloc&) { denied = true; }
    }
    EXPECT_TRUE(denied); EXPECT_TRUE(f.matches(witness));
  }
  EXPECT_TRUE(f.matches(f.plan(patch)));
}

TEST(GltfCameraAuthoring, CanonicalPropertyRegistryAndOwnedNoopLeavesAreExact) {
  bool pathsValid{}, invalid{}; size_t attempts{};
  {
    material_publication_test::AllocationGuard deny(true); pathsValid = true;
    for (size_t i = 0; i < size_t(P::Count); ++i) pathsValid &= ParseGltfCameraProperty(GltfCameraPropertyPath(P(i))) == P(i);
    invalid = GltfCameraPropertyPath(P::Count).empty() && !ParseGltfCameraProperty("perspective/future"); attempts = deny.allocations();
  }
  EXPECT_TRUE(pathsValid); EXPECT_TRUE(invalid); EXPECT_EQ(attempts, 0u);
  Fixture f; f.authored.cameras[0].perspective.zfar = f.evaluated.cameras[2].perspective.zfar = 100;
  auto plan = f.plan({{}, {{P::PerspectiveNear, .2}, {P::PerspectiveFar, 100.}}});
  auto owned = GltfCameraProjectionOwnedFields(plan);
  EXPECT_EQ(owned.properties, uint16_t((1u << size_t(P::PerspectiveNear)) | (1u << size_t(P::PerspectiveFar))));
  EXPECT_FALSE(owned.type); EXPECT_EQ(plan.changedProperties, (std::vector<P>{P::PerspectiveNear}));
  auto tampered = plan; ++tampered.peakOwnedBytes;
  EXPECT_THROW((void)GltfCameraProjectionOwnedFields(tampered), std::invalid_argument);
}

TEST(GltfCameraAuthoring, ReplayRestoresOnlyOwnedLeavesAndPreservesCurrentMetadataAndAnimatedOtherFields) {
  Fixture f; pointer(f.authored, "/cameras/0/perspective/yfov"); pointer(f.evaluated, "/cameras/2/perspective/yfov");
  auto original = f.plan(number(P::PerspectiveNear, .25));
  f.authored.cameras[0] = original.authoredAfter; f.evaluated.cameras[2] = original.evaluatedAfter;
  f.authored.cameras[0].name = "Current authored name"; f.evaluated.cameras[2].name = "Current evaluated name";
  f.authored.cameras[0].extras.Get<O>()["newMetadata"] = V(19);
  f.evaluated.cameras[2].perspective.yfov = 1.1; f.evaluated.cameras[2].perspective.aspectRatio = 2.25;
  auto undo = RebaseGltfCameraProjectionEdit(original, GltfCameraEditSide::Before,
      GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated));
  EXPECT_DOUBLE_EQ(undo.authoredAfter.perspective.znear, .1); EXPECT_DOUBLE_EQ(undo.evaluatedAfter.perspective.znear, .1);
  EXPECT_EQ(undo.authoredAfter.name, "Current authored name"); EXPECT_EQ(undo.evaluatedAfter.name, "Current evaluated name");
  EXPECT_EQ(undo.authoredAfter.extras.Get("newMetadata").Get<int>(), 19);
  EXPECT_DOUBLE_EQ(undo.evaluatedAfter.perspective.yfov, 1.1); EXPECT_DOUBLE_EQ(undo.evaluatedAfter.perspective.aspectRatio, 2.25);
  EXPECT_TRUE(f.matches(undo));
  f.authored.cameras[0] = undo.authoredAfter; f.evaluated.cameras[2] = undo.evaluatedAfter;
  f.evaluated.cameras[2].perspective.yfov = 1.2;
  auto redo = RebaseGltfCameraProjectionEdit(undo, GltfCameraEditSide::After,
      GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated));
  EXPECT_DOUBLE_EQ(redo.authoredAfter.perspective.znear, .25); EXPECT_DOUBLE_EQ(redo.evaluatedAfter.perspective.yfov, 1.2);
  EXPECT_EQ(redo.authoredAfter.name, "Current authored name"); EXPECT_TRUE(f.matches(redo));
}

TEST(GltfCameraAuthoring, ReplayRejectsTamperedOriginalOwnedConflictsAndChangedSharedContexts) {
  Fixture f; auto original = f.plan(number(P::PerspectiveNear, .25));
  f.authored.cameras[0] = original.authoredAfter; f.evaluated.cameras[2] = original.evaluatedAfter;
  const auto replay = [&](const auto& plan) { return RebaseGltfCameraProjectionEdit(plan, GltfCameraEditSide::Before,
      GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated)); };
  auto altered = original; altered.authoredBefore.perspective.znear = .15;
  EXPECT_THROW((void)replay(altered), std::invalid_argument);
  f.authored.cameras[0].perspective.znear = std::nextafter(.25, 1.);
  EXPECT_THROW((void)replay(original), std::invalid_argument);
  f.authored.cameras[0].perspective.znear = .25;
  f.authored.nodes[0].camera = -1; f.authored.nodes[1].camera = 0;
  EXPECT_THROW((void)replay(original), std::invalid_argument);
  f.authored.nodes[0].camera = 0; f.authored.nodes[1].camera = -1;
  pointer(f.evaluated, "/cameras/2/perspective/znear");
  EXPECT_THROW((void)replay(original), std::invalid_argument);
}

TEST(GltfCameraAuthoring, ProjectionSwitchAndOmittedOptionalLeavesReplayOriginalDormantBits) {
  Fixture f; auto original = f.plan({GltfCameraProjection::Orthographic,
      {{P::OrthographicXmag, 8.}, {P::OrthographicYmag, 3.}, {P::OrthographicNear, 0.}, {P::OrthographicFar, 20.}}});
  f.authored.cameras[0] = original.authoredAfter; f.evaluated.cameras[2] = original.evaluatedAfter;
  f.evaluated.cameras[2].perspective.yfov = 1.1;
  auto undo = RebaseGltfCameraProjectionEdit(original, GltfCameraEditSide::Before,
      GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated));
  EXPECT_EQ(undo.authoredAfter.type, "perspective"); EXPECT_DOUBLE_EQ(undo.authoredAfter.orthographic.xmag, 0);
  EXPECT_DOUBLE_EQ(undo.evaluatedAfter.perspective.yfov, 1.1); EXPECT_TRUE(GltfCameraProjectionValid(undo.evaluatedAfter));
  f.authored.cameras[0] = undo.authoredAfter; f.evaluated.cameras[2] = undo.evaluatedAfter;
  auto redo = RebaseGltfCameraProjectionEdit(original, GltfCameraEditSide::After,
      GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated));
  EXPECT_EQ(redo.evaluatedAfter.type, "orthographic"); EXPECT_DOUBLE_EQ(redo.evaluatedAfter.orthographic.xmag, 8);
  Fixture optional; auto fixed = optional.plan({{}, {{P::PerspectiveFar, 200.}, {P::PerspectiveAspectRatio, 2.}}});
  optional.authored.cameras[0] = fixed.authoredAfter; optional.evaluated.cameras[2] = fixed.evaluatedAfter;
  auto omitted = RebaseGltfCameraProjectionEdit(fixed, GltfCameraEditSide::Before,
      GltfCameraSourceView(optional.authored), GltfCameraSourceView(optional.evaluated));
  EXPECT_DOUBLE_EQ(omitted.authoredAfter.perspective.zfar, 0);
  EXPECT_DOUBLE_EQ(omitted.authoredAfter.perspective.aspectRatio, 1.25);
}

namespace {
int cameraFloats(tinygltf::Model& m, std::initializer_list<float> values, int type, size_t count) {
  tinygltf::Buffer buffer; buffer.data.resize(values.size() * sizeof(float));
  std::memcpy(buffer.data.data(), values.begin(), buffer.data.size());
  tinygltf::BufferView view; view.buffer = int(m.buffers.size()); view.byteLength = buffer.data.size();
  tinygltf::Accessor accessor; accessor.sparse = {}; accessor.bufferView = int(m.bufferViews.size());
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT; accessor.type = type; accessor.count = count;
  if (type == TINYGLTF_TYPE_SCALAR) { const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
    accessor.minValues = {*lo}; accessor.maxValues = {*hi}; }
  const auto id = int(m.accessors.size()); m.buffers.push_back(buffer); m.bufferViews.push_back(view); m.accessors.push_back(accessor); return id;
}
void cameraChannel(tinygltf::Model& m, int input, int output, const std::string& path, int node = -1) {
  if (m.animations.empty()) m.animations.emplace_back();
  tinygltf::AnimationSampler sampler; sampler.input = input; sampler.output = output;
  sampler.extras = V(O{{"samplerMetadata", V(31)}});
  tinygltf::AnimationChannel channel; channel.sampler = int(m.animations[0].samplers.size()); channel.target_node = node;
  channel.target_path = node < 0 ? "pointer" : path;
  if (node < 0) channel.target_extensions["KHR_animation_pointer"] = V(O{{"pointer", V(path)}});
  m.animations[0].samplers.push_back(sampler); m.animations[0].channels.push_back(channel);
}
struct CameraRestFixture {
  tinygltf::Model authored, evaluated;
  CameraRestFixture() {
    authored.cameras = {perspective(), orthographic()}; authored.nodes.resize(3);
    authored.nodes[0].camera = authored.nodes[1].camera = 0; authored.materials.emplace_back();
    const auto time = cameraFloats(authored, {0, 2}, TINYGLTF_TYPE_SCALAR, 2);
    cameraChannel(authored, time, cameraFloats(authored, {.8f, 1.2f}, TINYGLTF_TYPE_SCALAR, 2), "/cameras/0/perspective/yfov");
    cameraChannel(authored, time, cameraFloats(authored, {.2f, .8f}, TINYGLTF_TYPE_SCALAR, 2), "/materials/0/pbrMetallicRoughness/roughnessFactor");
    cameraChannel(authored, time, cameraFloats(authored, {0, 0, 0, 2, 3, 4}, TINYGLTF_TYPE_VEC3, 2), "translation", 2);
    evaluated = authored;
  }
  AnimationPlayer running() {
    AnimationPlayer player(authored); player.selectClip(evaluated, 0); player.seek(evaluated, .5); player.play(); return player;
  }
  GltfCameraProjectionPlan plan(double near = .25) const {
    return PlanGltfCameraProjectionEdit(GltfCameraSourceView(authored), GltfCameraSourceView(evaluated), 0, 0, number(P::PerspectiveNear, near));
  }
};
} // namespace

TEST(GltfCameraAnimation, SelectedRestCommitsUnderDenialPreservingOtherRestsCurvesAndAllPlaybackControls) {
  CameraRestFixture f; auto player = f.running(); const auto before = player; auto plan = f.plan();
  EXPECT_EQ(plan.authoredNodes, (std::vector<uint32_t>{0, 1}));
  auto prepared = player.prepareCameraRestEdit(plan, GltfCameraSourceView(f.authored), f.evaluated);
  auto moved = std::move(prepared); EXPECT_FALSE(prepared); ASSERT_TRUE(moved);
  bool ready{}, committed{}; size_t allocations{};
  { material_publication_test::AllocationGuard deny(true);
    ready = player.cameraRestEditMatches(moved, GltfCameraSourceView(f.authored), f.evaluated);
    committed = player.exchangeCameraRestEdit(moved, GltfCameraSourceView(f.authored), f.evaluated); allocations = deny.allocations(); }
  EXPECT_TRUE(ready); EXPECT_TRUE(committed); EXPECT_EQ(allocations, 0u); EXPECT_FALSE(moved);
  EXPECT_TRUE(GltfCameraRecordsEqual(player.library().restCameras[0], plan.authoredAfter));
  EXPECT_TRUE(GltfCameraRecordsEqual(player.library().restCameras[1], before.library().restCameras[1]));
  EXPECT_EQ(player.library().restMaterials, before.library().restMaterials); EXPECT_EQ(player.library().restNodes, before.library().restNodes);
  EXPECT_EQ(player.library().clips, before.library().clips); EXPECT_EQ(player.selectedClip(), before.selectedClip());
  EXPECT_DOUBLE_EQ(player.time(), before.time()); EXPECT_DOUBLE_EQ(player.speed(), before.speed());
  EXPECT_EQ(player.playing(), before.playing()); EXPECT_EQ(player.loop(), before.loop());
  EXPECT_DOUBLE_EQ(f.evaluated.cameras[0].perspective.znear, .1); // source publication remains caller-owned
  f.authored.cameras[0] = plan.authoredAfter; f.evaluated.cameras[0] = plan.evaluatedAfter;
  player.seek(f.evaluated, 1.5); EXPECT_DOUBLE_EQ(f.evaluated.cameras[0].perspective.znear, .25);
  EXPECT_NEAR(f.evaluated.cameras[0].perspective.yfov, .8f + .75 * double(1.2f - .8f), 1e-7);
  player.clearClip(f.evaluated); EXPECT_DOUBLE_EQ(f.evaluated.cameras[0].perspective.znear, .25);
  EXPECT_DOUBLE_EQ(f.evaluated.cameras[0].perspective.yfov, .8);
}

TEST(GltfCameraAnimation, SelectedRestReplaysAtTodaysAnimatedOtherFieldWithoutRewindingClock) {
  CameraRestFixture f; auto player = f.running(); auto original = f.plan();
  auto apply = player.prepareCameraRestEdit(original, GltfCameraSourceView(f.authored), f.evaluated);
  ASSERT_TRUE(player.exchangeCameraRestEdit(apply, GltfCameraSourceView(f.authored), f.evaluated));
  f.authored.cameras[0] = original.authoredAfter; f.evaluated.cameras[0] = original.evaluatedAfter;
  player.seek(f.evaluated, 1.25); const auto yfov = f.evaluated.cameras[0].perspective.yfov;
  auto undo = RebaseGltfCameraProjectionEdit(original, GltfCameraEditSide::Before,
      GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated));
  auto restUndo = player.prepareCameraRestEdit(undo, GltfCameraSourceView(f.authored), f.evaluated);
  size_t allocations{}; bool committed{};
  { material_publication_test::AllocationGuard deny(true); committed = player.exchangeCameraRestEdit(restUndo,
      GltfCameraSourceView(f.authored), f.evaluated); allocations = deny.allocations(); }
  EXPECT_TRUE(committed); EXPECT_EQ(allocations, 0u); EXPECT_DOUBLE_EQ(player.time(), 1.25);
  EXPECT_DOUBLE_EQ(undo.evaluatedAfter.perspective.yfov, yfov); EXPECT_DOUBLE_EQ(player.library().restCameras[0].perspective.znear, .1);
}

TEST(GltfCameraAnimation, AnimatedRestEditsAndHistoryPreserveCurrentClipSamplesAndOriginalCurves) {
  CameraRestFixture f; auto player = f.running(); const auto curves = player.library().clips;
  auto original = PlanGltfCameraProjectionEdit(GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated),
      0, 0, number(P::PerspectiveYfov, .65));
  auto edit = player.prepareCameraRestEdit(original, GltfCameraSourceView(f.authored), f.evaluated);
  EXPECT_DOUBLE_EQ(original.authoredAfter.perspective.yfov, .65);
  EXPECT_DOUBLE_EQ(edit.evaluatedCamera().perspective.yfov, f.evaluated.cameras[0].perspective.yfov);
  bool committed{}; size_t allocations{};
  { material_publication_test::AllocationGuard deny(true);
    committed = player.exchangeCameraRestEdit(edit, GltfCameraSourceView(f.authored), f.evaluated);
    allocations = deny.allocations(); }
  ASSERT_TRUE(committed); EXPECT_EQ(allocations, 0u); EXPECT_DOUBLE_EQ(player.time(), .5);
  f.authored.cameras[0] = original.authoredAfter; f.evaluated.cameras[0] = edit.evaluatedCamera();
  player.seek(f.evaluated, 1.25); const auto sample = f.evaluated.cameras[0].perspective.yfov;

  auto undo = RebaseGltfCameraProjectionEdit(original, GltfCameraEditSide::Before,
      GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated));
  auto undoRest = player.prepareCameraRestEdit(undo, GltfCameraSourceView(f.authored), f.evaluated);
  EXPECT_DOUBLE_EQ(undoRest.evaluatedCamera().perspective.yfov, sample);
  { material_publication_test::AllocationGuard deny(true);
    committed = player.exchangeCameraRestEdit(undoRest, GltfCameraSourceView(f.authored), f.evaluated);
    allocations = deny.allocations(); }
  ASSERT_TRUE(committed); EXPECT_EQ(allocations, 0u); EXPECT_DOUBLE_EQ(player.time(), 1.25);
  f.authored.cameras[0] = undo.authoredAfter; f.evaluated.cameras[0] = undoRest.evaluatedCamera();
  player.clearClip(f.evaluated); EXPECT_DOUBLE_EQ(f.evaluated.cameras[0].perspective.yfov, .8);

  auto redo = RebaseGltfCameraProjectionEdit(original, GltfCameraEditSide::After,
      GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated));
  auto redoRest = player.prepareCameraRestEdit(redo, GltfCameraSourceView(f.authored), f.evaluated);
  EXPECT_DOUBLE_EQ(redoRest.evaluatedCamera().perspective.yfov, .65);
  ASSERT_TRUE(player.exchangeCameraRestEdit(redoRest, GltfCameraSourceView(f.authored), f.evaluated));
  f.authored.cameras[0] = redo.authoredAfter; f.evaluated.cameras[0] = redoRest.evaluatedCamera();
  EXPECT_EQ(player.library().clips, curves); EXPECT_FALSE(player.selectedClip()); EXPECT_FALSE(player.playing());
  player.selectClip(f.evaluated, 0); player.seek(f.evaluated, .5);
  EXPECT_NEAR(f.evaluated.cameras[0].perspective.yfov, .9, 1e-7);
  player.clearClip(f.evaluated); EXPECT_DOUBLE_EQ(f.evaluated.cameras[0].perspective.yfov, .65);
}

TEST(GltfCameraAnimation, CandidateUsesEditedRestWhenAnotherClipDoesNotControlTheOwnedField) {
  CameraRestFixture f;
  f.authored.animations.push_back(f.authored.animations[0]);
  f.authored.animations.back().channels.erase(f.authored.animations.back().channels.begin());
  f.evaluated = f.authored;
  auto player = f.running(); player.selectClip(f.evaluated, 1); player.seek(f.evaluated, .75);
  auto plan = PlanGltfCameraProjectionEdit(GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated),
      0, 0, number(P::PerspectiveYfov, .65));
  auto edit = player.prepareCameraRestEdit(plan, GltfCameraSourceView(f.authored), f.evaluated);
  EXPECT_DOUBLE_EQ(edit.evaluatedCamera().perspective.yfov, .65);
  EXPECT_EQ(player.selectedClip(), 1u); EXPECT_DOUBLE_EQ(player.time(), .75);
}

TEST(GltfCameraAnimation, CandidatePreservesUnownedEvaluatedFieldsAndAnimatedRestNoops) {
  CameraRestFixture f; auto player = f.running();
  f.evaluated.cameras[0].perspective.yfov = 1.1;
  auto edit = player.prepareCameraRestEdit(f.plan(), GltfCameraSourceView(f.authored), f.evaluated);
  EXPECT_DOUBLE_EQ(edit.evaluatedCamera().perspective.yfov, 1.1);
  EXPECT_DOUBLE_EQ(edit.evaluatedCamera().perspective.znear, .25);
  const auto noop = PlanGltfCameraProjectionEdit(GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated),
      0, 0, number(P::PerspectiveYfov, .8));
  EXPECT_FALSE(noop.changed); EXPECT_DOUBLE_EQ(noop.evaluatedAfter.perspective.yfov, 1.1);
}

TEST(GltfCameraAnimation, SelectedRawAndEncodedCurveChangesRejectCanAndDirectExchangeWithoutAllocation) {
  const std::array<std::function<void(CameraRestFixture&)>, 6> mutate{
      [](auto& f) { f.evaluated.buffers[1].data[0] ^= 1; },
      [](auto& f) { f.evaluated.accessors[1].extras_json_string = "changed raw accessor"; },
      [](auto& f) { f.evaluated.bufferViews[1].byteStride = 8; },
      [](auto& f) { f.evaluated.animations[0].samplers[0].extras.Get<O>()["samplerMetadata"] = V(31.); },
      [](auto& f) { f.evaluated.animations[0].channels[0].target_extensions_json_string = "changed target"; },
      [](auto& f) { f.evaluated.extensionsRequired.push_back("KHR_accessor_float64"); }};
  for (const auto& change : mutate) {
    CameraRestFixture f; auto player = f.running(); const auto rest = player.library().restCameras[0];
    auto prepared = player.prepareCameraRestEdit(f.plan(), GltfCameraSourceView(f.authored), f.evaluated); change(f);
    bool ready{}, committed{}; size_t allocations{};
    { material_publication_test::AllocationGuard deny(true); ready = player.cameraRestEditMatches(prepared, GltfCameraSourceView(f.authored), f.evaluated);
      committed = player.exchangeCameraRestEdit(prepared, GltfCameraSourceView(f.authored), f.evaluated); allocations = deny.allocations(); }
    EXPECT_FALSE(ready); EXPECT_FALSE(committed); EXPECT_EQ(allocations, 0u);
    EXPECT_TRUE(GltfCameraRecordsEqual(player.library().restCameras[0], rest));
  }
}

TEST(GltfCameraAnimation, SelectedDecodedReplacementRejectsButUnrelatedMaterialNodeCurvesStayCurrent) {
  CameraRestFixture f; auto player = f.running(); auto token = player.prepareCameraRestEdit(f.plan(), GltfCameraSourceView(f.authored), f.evaluated);
  auto changedSource = f.authored; const float changed = 1.1f; std::memcpy(changedSource.buffers[1].data.data(), &changed, sizeof(changed));
  AnimationPlayer changedPlayer(changedSource); changedPlayer.selectClip(changedSource, 0); changedPlayer.seek(changedSource, .5); changedPlayer.play();
  player = std::move(changedPlayer); bool rejected{}; size_t allocations{};
  { material_publication_test::AllocationGuard deny(true); rejected = !player.cameraRestEditMatches(token,
      GltfCameraSourceView(f.authored), f.evaluated); allocations = deny.allocations(); }
  EXPECT_TRUE(rejected); EXPECT_EQ(allocations, 0u);

  CameraRestFixture other; auto live = other.running(); auto ready = live.prepareCameraRestEdit(other.plan(), GltfCameraSourceView(other.authored), other.evaluated);
  auto alternative = other.authored; const float material = .3f, node = 9.f;
  std::memcpy(alternative.buffers[2].data.data(), &material, sizeof(material)); std::memcpy(alternative.buffers[3].data.data(), &node, sizeof(node));
  AnimationPlayer replacement(alternative); replacement.selectClip(alternative, 0); replacement.seek(alternative, .5); replacement.play();
  live = std::move(replacement); other.evaluated.buffers[2].data = alternative.buffers[2].data;
  other.evaluated.buffers[3].data = alternative.buffers[3].data;
  bool accepted{}, committed{};
  { material_publication_test::AllocationGuard deny(true); accepted = live.cameraRestEditMatches(ready,
      GltfCameraSourceView(other.authored), other.evaluated);
    committed = live.exchangeCameraRestEdit(ready, GltfCameraSourceView(other.authored), other.evaluated); allocations = deny.allocations(); }
  EXPECT_TRUE(accepted); EXPECT_TRUE(committed); EXPECT_EQ(allocations, 0u);
  EXPECT_DOUBLE_EQ(live.library().clips[0].tracks[1].values[0], double(material));
  EXPECT_DOUBLE_EQ(live.library().clips[0].tracks[2].values[0], double(node));
}

TEST(GltfCameraAnimation, StaleControlsForeignPlayerAndCompetingRestTokensPreserveOldRecord) {
  CameraRestFixture f; auto player = f.running(); auto first = player.prepareCameraRestEdit(f.plan(), GltfCameraSourceView(f.authored), f.evaluated);
  auto second = player.prepareCameraRestEdit(f.plan(.3), GltfCameraSourceView(f.authored), f.evaluated);
  auto foreign = player; bool accepted{}, committed{}; size_t allocations{};
  { material_publication_test::AllocationGuard deny(true);
    accepted = foreign.cameraRestEditMatches(first, GltfCameraSourceView(f.authored), f.evaluated);
    committed = foreign.exchangeCameraRestEdit(first, GltfCameraSourceView(f.authored), f.evaluated); allocations = deny.allocations(); }
  EXPECT_FALSE(accepted); EXPECT_FALSE(committed); EXPECT_EQ(allocations, 0u);
  player.setSpeed(2); EXPECT_FALSE(player.cameraRestEditMatches(first, GltfCameraSourceView(f.authored), f.evaluated)); player.setSpeed(1);
  ASSERT_TRUE(player.exchangeCameraRestEdit(first, GltfCameraSourceView(f.authored), f.evaluated));
  { material_publication_test::AllocationGuard deny(true);
    accepted = player.cameraRestEditMatches(second, GltfCameraSourceView(f.authored), f.evaluated);
    committed = player.exchangeCameraRestEdit(second, GltfCameraSourceView(f.authored), f.evaluated); allocations = deny.allocations(); }
  EXPECT_FALSE(accepted); EXPECT_FALSE(committed); EXPECT_EQ(allocations, 0u); EXPECT_DOUBLE_EQ(player.library().restCameras[0].perspective.znear, .25);
}

TEST(GltfCameraAnimation, NoopAndAbandonedRestTokensDoNotChangeRestOrInvalidateOtherPreparation) {
  CameraRestFixture f; auto player = f.running(); auto plan = f.plan();
  auto ready = player.prepareCameraRestEdit(plan, GltfCameraSourceView(f.authored), f.evaluated);
  { auto abandoned = player.prepareCameraRestEdit(plan, GltfCameraSourceView(f.authored), f.evaluated); }
  auto noopPlan = PlanGltfCameraProjectionEdit(GltfCameraSourceView(f.authored), GltfCameraSourceView(f.evaluated), 0, 0, {});
  auto noop = player.prepareCameraRestEdit(noopPlan, GltfCameraSourceView(f.authored), f.evaluated);
  bool committed{}, stillReady{}; size_t allocations{};
  { material_publication_test::AllocationGuard deny(true); committed = player.exchangeCameraRestEdit(noop,
      GltfCameraSourceView(f.authored), f.evaluated);
    stillReady = player.cameraRestEditMatches(ready, GltfCameraSourceView(f.authored), f.evaluated); allocations = deny.allocations(); }
  EXPECT_TRUE(committed); EXPECT_TRUE(stillReady); EXPECT_EQ(allocations, 0u); EXPECT_FALSE(noop);
  EXPECT_DOUBLE_EQ(player.library().restCameras[0].perspective.znear, .1);
}

TEST(GltfCameraAnimation, SelectedRestBudgetsDoNotCloneUnrelatedRestsOrGeometryAndFailuresAreAtomic) {
  CameraRestFixture f; auto player = f.running(); auto plan = f.plan();
  const auto baseline = player.prepareCameraRestEdit(plan, GltfCameraSourceView(f.authored), f.evaluated).ownedBytes();
  CameraRestFixture large; large.authored.materials[0].extras = V(std::string(2 * 1024 * 1024, 'x'));
  large.authored.cameras[1].extras = V(std::string(2 * 1024 * 1024, 'y')); large.authored.buffers[2].data.resize(2 * 1024 * 1024, 83);
  large.evaluated = large.authored; auto largePlayer = large.running(); GltfCameraAnimationEditLimits bounded; bounded.maxOwnedBytes = 128 * 1024;
  auto selected = largePlayer.prepareCameraRestEdit(large.plan(), GltfCameraSourceView(large.authored), large.evaluated, bounded);
  EXPECT_EQ(selected.ownedBytes(), baseline); EXPECT_LT(selected.ownedBytes(), bounded.maxOwnedBytes);
  for (size_t variant = 0; variant < 6; ++variant) {
    auto limits = bounded;
    if (variant == 0) limits.maxOwnedBytes = 1;
    if (variant == 1) limits.maxDecodedBytes = 1;
    if (variant == 2) limits.maxMetadataValues = 1;
    if (variant == 3) limits.maxMetadataDepth = 1;
    if (variant == 4) limits.maxChannels = 0;
    if (variant == 5) limits.maxClips = 0;
    EXPECT_THROW((void)player.prepareCameraRestEdit(plan, GltfCameraSourceView(f.authored), f.evaluated, limits), std::invalid_argument);
  }
  auto changed = plan; changed.authoredAfter.perspective.znear = .3;
  EXPECT_THROW((void)player.prepareCameraRestEdit(changed, GltfCameraSourceView(f.authored), f.evaluated), std::invalid_argument);
  EXPECT_DOUBLE_EQ(player.library().restCameras[0].perspective.znear, .1);
}

TEST(GltfCameraAnimation, EveryDeniedSelectedRestPreparationLeavesExistingPlanRestAndClockCurrent) {
  CameraRestFixture f; auto player = f.running(); auto plan = f.plan();
  auto existing = player.prepareCameraRestEdit(plan, GltfCameraSourceView(f.authored), f.evaluated); size_t allocations{};
  { material_publication_test::AllocationGuard count(false); auto prepared = player.prepareCameraRestEdit(plan,
      GltfCameraSourceView(f.authored), f.evaluated); count.disable(); allocations = count.allocations(); }
  ASSERT_GT(allocations, 0u); ASSERT_LT(allocations, 3000u);
  for (size_t allowance = 0; allowance < allocations; ++allowance) {
    bool denied{};
    { material_publication_test::AllocationGuard guard(false, allowance);
      try { (void)player.prepareCameraRestEdit(plan, GltfCameraSourceView(f.authored), f.evaluated); }
      catch (const std::bad_alloc&) { denied = true; } }
    EXPECT_TRUE(denied); EXPECT_TRUE(player.cameraRestEditMatches(existing, GltfCameraSourceView(f.authored), f.evaluated));
  }
  EXPECT_DOUBLE_EQ(player.time(), .5); EXPECT_DOUBLE_EQ(player.library().restCameras[0].perspective.znear, .1);
}

TEST(GltfCameraAnimation, SelectedStridedSparseProofReadsOnlyEncodedCameraElementsAtFinalBoundary) {
  CameraRestFixture f; auto& bytes = f.authored.buffers[1].data; bytes.assign(2 * 1024 * 1024, 83);
  f.authored.bufferViews[1].byteStride = 64; f.authored.bufferViews[1].byteLength = bytes.size();
  f.authored.accessors[1].byteOffset = 4; const std::array<float, 2> values{.8f, 1.2f};
  for (size_t i = 0; i < 2; ++i) std::memcpy(bytes.data() + 4 + i * 64, &values[i], sizeof(float));
  f.evaluated = f.authored; auto player = f.running(); GltfCameraAnimationEditLimits limits; limits.maxOwnedBytes = 128 * 1024;
  auto token = player.prepareCameraRestEdit(f.plan(), GltfCameraSourceView(f.authored), f.evaluated, limits);
  EXPECT_LT(token.ownedBytes(), limits.maxOwnedBytes);
  f.evaluated.buffers[1].data[20] ^= 1; bool valid{}, stale{}; size_t allocations{};
  { material_publication_test::AllocationGuard deny(true); valid = player.cameraRestEditMatches(token,
      GltfCameraSourceView(f.authored), f.evaluated);
    f.evaluated.buffers[1].data[4] ^= 1; stale = player.cameraRestEditMatches(token,
      GltfCameraSourceView(f.authored), f.evaluated); allocations = deny.allocations(); }
  EXPECT_TRUE(valid); EXPECT_FALSE(stale); EXPECT_EQ(allocations, 0u);

  CameraRestFixture sparse; tinygltf::Buffer indices; indices.data = {0, 1}; sparse.authored.buffers.push_back(indices);
  tinygltf::BufferView view; view.buffer = 4; view.byteLength = 2; sparse.authored.bufferViews.push_back(view);
  auto& accessor = sparse.authored.accessors[1]; accessor.bufferView = -1; accessor.sparse = {};
  accessor.sparse.isSparse = true; accessor.sparse.count = 2; accessor.sparse.indices.bufferView = 4;
  accessor.sparse.indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE; accessor.sparse.values.bufferView = 1;
  sparse.evaluated = sparse.authored; auto sparsePlayer = sparse.running();
  auto sparseToken = sparsePlayer.prepareCameraRestEdit(sparse.plan(), GltfCameraSourceView(sparse.authored), sparse.evaluated);
  { material_publication_test::AllocationGuard deny(true); valid = sparsePlayer.cameraRestEditMatches(sparseToken,
      GltfCameraSourceView(sparse.authored), sparse.evaluated);
    sparse.evaluated.buffers[4].data[1] = 0; stale = sparsePlayer.cameraRestEditMatches(sparseToken,
      GltfCameraSourceView(sparse.authored), sparse.evaluated); allocations = deny.allocations(); }
  EXPECT_TRUE(valid); EXPECT_FALSE(stale); EXPECT_EQ(allocations, 0u);
}
