#include "Container/asl/HandKinematics.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using namespace container::asl;
constexpr std::array<std::size_t, 5> roots{1, 5, 9, 13, 17};
HandCalibration fixture(Handedness hand = Handedness::Right) {
    HandCalibration c;
    c.handedness = hand; c.valid = true;
    c.palmOffsets = {glm::dvec3(.55, .4, 0), {.3, 1, 0}, {.1, 1.1, 0}, {-.12, 1.05, 0}, {-.3, 1, 0}};
    if (hand == Handedness::Left) for (auto& p : c.palmOffsets) p.x = -p.x;
    for (std::size_t d = 0; d < 5; ++d) c.boneLengths[d] = {.4 - d * .02, .25 - d * .01, .2 - d * .01};
    return c;
}
HandPose articulated() {
    HandPose p;
    p.angles = {.25, -.12, .3, .55, .4, .45, .12, .7, .3, .65, -.1, .8, .4, .35, .08, .9, .2, .55, -.15, .65, .5};
    return p;
}
HandObservation observation(const HandCalibration& c, const HandPose& p, std::int64_t t = 0) {
    HandObservation f;
    f.positions = forwardHand(c, p); f.observed.fill(true); f.timestampMs = t;
    return f;
}
void expectLengths(const HandCalibration& c, const HandPose& p, const std::array<glm::dvec3, 21>& points) {
    for (std::size_t digit = 0; digit < 5; ++digit) for (std::size_t s = 0; s < 3; ++s)
        EXPECT_NEAR(glm::length(points[roots[digit] + s + 1] - points[roots[digit] + s]),
                    c.boneLengths[digit][s] * p.scale, 1e-12);
}

TEST(AslHandKinematics, ForwardHasFixedBonesAndRigidPalm) {
    const auto c = fixture();
    auto p = articulated(); p.scale = 2.3; p.wrist = {3, -2, 7};
    const auto points = forwardHand(c, p);
    expectLengths(c, p, points);
    for (std::size_t a = 0; a < 5; ++a) for (std::size_t b = 0; b < 5; ++b)
        EXPECT_NEAR(glm::length(points[roots[a]] - points[roots[b]]),
                    glm::length(c.palmOffsets[a] - c.palmOffsets[b]) * p.scale, 1e-12);
}
TEST(AslHandKinematics, IndependentDipDoesNotForcePipCoupling) {
    const auto c = fixture(); HandPose a, b; b.angles[8] = .8;
    const auto pa = forwardHand(c, a), pb = forwardHand(c, b);
    EXPECT_LT(glm::length(pa[7] - pb[7]), 1e-12);
    EXPECT_GT(glm::length(pa[8] - pb[8]), .1);
}
TEST(AslHandKinematics, LeftRightMirrorWithProperBases) {
    const auto p = articulated();
    const auto right = forwardHand(fixture(), p), left = forwardHand(fixture(Handedness::Left), p);
    for (std::size_t j = 0; j < 21; ++j) {
        EXPECT_NEAR(left[j].x, -right[j].x, 1e-12);
        EXPECT_NEAR(left[j].y, right[j].y, 1e-12);
        EXPECT_NEAR(left[j].z, right[j].z, 1e-12);
    }
}
TEST(AslHandKinematics, CalibrationUsesFixedMedianLengthsNotCurledNeutralAxes) {
    const auto c = fixture();
    const std::array frames{observation(c, articulated()), observation(c, HandPose{})};
    const auto calibrated = calibrateHand(frames, Handedness::Right);
    ASSERT_TRUE(calibrated.valid) << calibrated.message;
    EXPECT_EQ(calibrated.palmFrameCount, 2);
    for (std::size_t d = 0; d < 5; ++d) {
        EXPECT_LT(glm::length(calibrated.palmOffsets[d] - c.palmOffsets[d]), 1e-12);
        for (std::size_t s = 0; s < 3; ++s) {
            EXPECT_NEAR(calibrated.boneLengths[d][s], c.boneLengths[d][s], 1e-12);
            EXPECT_EQ(calibrated.boneObservationCounts[d][s], 2);
        }
    }
}
TEST(AslHandKinematics, BoundedIkRecoversSyntheticArticulation) {
    const auto c = fixture(); const auto source = observation(c, articulated());
    const auto fit = fitHand(c, source);
    ASSERT_TRUE(fit.valid) << fit.diagnostics.message;
    EXPECT_LT(fit.diagnostics.rmsResidual, 1e-5);
    EXPECT_LT(fit.diagnostics.maxBoneLengthError, 1e-12);
    EXPECT_EQ(fit.observed, source.observed);
    for (std::size_t a = 0; a < 21; ++a) EXPECT_NEAR(fit.pose.angles[a], articulated().angles[a], 1e-3);
}
TEST(AslHandKinematics, RotationTranslationAndPalmOnlyScaleAreEquivariant) {
    const auto c = fixture(Handedness::Left);
    auto p = articulated();
    const double a = .71, b = -.43;
    glm::dmat3 z{{std::cos(a), std::sin(a), 0}, {-std::sin(a), std::cos(a), 0}, {0, 0, 1}};
    glm::dmat3 x{{1, 0, 0}, {0, std::cos(b), std::sin(b)}, {0, -std::sin(b), std::cos(b)}};
    p.basis = z * x; p.wrist = {4, -3, 1}; p.scale = 1.7;
    const auto fit = fitHand(c, observation(c, p));
    ASSERT_TRUE(fit.valid);
    EXPECT_NEAR(fit.pose.scale, 1.7, 1e-12);
    EXPECT_NEAR(glm::determinant(fit.pose.basis), 1.0, 1e-12);
    EXPECT_LT(fit.diagnostics.rmsResidual, 1e-5);
    expectLengths(c, fit.pose, fit.positions);
}
TEST(AslHandKinematics, MissingPointsStayUnobservedEvenWhenInferred) {
    const auto c = fixture(); auto source = observation(c, articulated());
    source.observed[8] = false;
    source.positions[8] = glm::dvec3(std::numeric_limits<double>::quiet_NaN());
    const auto fit = fitHand(c, source);
    ASSERT_TRUE(fit.valid);
    EXPECT_FALSE(fit.observed[8]); EXPECT_TRUE(fit.inferred[8]);
    EXPECT_TRUE(std::isfinite(fit.positions[8].x));
    EXPECT_TRUE(std::isnan(source.positions[8].x)); // Input remains untouched.
    EXPECT_EQ(fit.diagnostics.observedCount, 20);
}
TEST(AslHandKinematics, MissingOrDegeneratePalmFallsBackToRaw) {
    const auto c = fixture(); auto source = observation(c, articulated());
    source.observed[0] = false;
    const auto missing = fitHand(c, source);
    EXPECT_FALSE(missing.valid); EXPECT_EQ(missing.observed, source.observed);
    EXPECT_EQ(missing.positions[8], source.positions[8]);
    source.observed[0] = true; source.positions[17] = source.positions[5];
    EXPECT_FALSE(fitHand(c, source).valid);
}
TEST(AslHandKinematics, NoHandsAndUnmeasuredCalibrationDoNotInventAHand) {
    HandObservation empty;
    EXPECT_FALSE(calibrateHand(std::span<const HandObservation>(&empty, 1), Handedness::Right).valid);
    EXPECT_FALSE(fitHand(fixture(), empty).valid);
    auto incomplete = observation(fixture(), HandPose{}); incomplete.observed[4] = false;
    EXPECT_FALSE(calibrateHand(std::span<const HandObservation>(&incomplete, 1), Handedness::Right).valid);
}
TEST(AslHandKinematics, AngleLimitsAlwaysHoldAndAreReported) {
    const auto c = fixture(); auto p = articulated(); p.angles[7] = 2;
    HandFitOptions options; options.limits[7] = {0, .2};
    const auto fit = fitHand(c, observation(c, p), options);
    ASSERT_TRUE(fit.valid);
    EXPECT_LE(fit.pose.angles[7], .2); EXPECT_GE(fit.pose.angles[7], 0);
    EXPECT_GT(fit.diagnostics.anglesAtLimit, 0);
    for (std::size_t a = 0; a < 21; ++a) {
        EXPECT_GE(fit.pose.angles[a], options.limits[a].minimum);
        EXPECT_LE(fit.pose.angles[a], options.limits[a].maximum);
    }
}
TEST(AslHandKinematics, AngularVelocityUsesActualTimestampAndGapResets) {
    const auto c = fixture();
    const auto initial = fitHand(c, observation(c, HandPose{}, 1000)); ASSERT_TRUE(initial.valid);
    auto target = HandPose{}; target.angles[7] = 1;
    HandFitOptions options; options.maxAngularVelocityRadPerSecond = .5;
    const auto next = fitHand(c, observation(c, target, 1040), options, &initial);
    ASSERT_TRUE(next.valid); EXPECT_TRUE(next.diagnostics.velocityLimited);
    for (std::size_t a = 0; a < 21; ++a) EXPECT_LE(std::abs(next.pose.angles[a] - initial.pose.angles[a]), .020000001);
    const auto gap = fitHand(c, observation(c, target, 2000), options, &initial);
    ASSERT_TRUE(gap.valid); EXPECT_TRUE(gap.diagnostics.temporalReset);
    EXPECT_FALSE(gap.diagnostics.velocityLimited); EXPECT_LT(gap.diagnostics.rmsResidual, 1e-5);
    const auto duplicate = fitHand(c, observation(c, target, 1000), options, &initial);
    EXPECT_TRUE(duplicate.diagnostics.temporalReset);
    const auto backwards = fitHand(c, observation(c, target, 999), options, &initial);
    EXPECT_TRUE(backwards.diagnostics.temporalReset);
}
TEST(AslHandKinematics, RepeatedFitsAreDeterministicAndProfilesUntouched) {
    const auto c = fixture(); const auto source = observation(c, articulated());
    const auto a = fitHand(c, source), b = fitHand(c, source);
    EXPECT_EQ(a.pose.angles, b.pose.angles); EXPECT_EQ(a.diagnostics.iterations, b.diagnostics.iterations);
    EXPECT_EQ(c.boneLengths, fixture().boneLengths);
}
TEST(AslHandKinematics, RejectsBadOptionsAndImproperRotation) {
    const auto c = fixture(); const auto source = observation(c, articulated());
    HandFitOptions options; options.limits[0] = {1, 0}; EXPECT_THROW(fitHand(c, source, options), std::invalid_argument);
    HandPose p; p.basis[0].x = -1; EXPECT_THROW(forwardHand(c, p), std::invalid_argument);
    p = {}; p.scale = 0; EXPECT_THROW(forwardHand(c, p), std::invalid_argument);
    options = {}; options.maxIterations = 0; EXPECT_THROW(fitHand(c, source, options), std::invalid_argument);
    auto malformed = c; malformed.boneLengths[0][0] = 0;
    EXPECT_THROW(forwardHand(malformed, HandPose{}), std::invalid_argument);
    malformed = c; malformed.palmOffsets[0].x = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(forwardHand(malformed, HandPose{}), std::invalid_argument);
}
TEST(AslHandKinematics, RejectsNonfiniteObservedDataAndTemporalState) {
    const auto c = fixture(); auto source = observation(c, articulated(), 1000);
    auto prior = fitHand(c, source); ASSERT_TRUE(prior.valid);
    source.positions[8].x = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(fitHand(c, source), std::invalid_argument);
    const std::array frames{source}; EXPECT_FALSE(calibrateHand(frames, Handedness::Right).valid);
    source = observation(c, articulated(), 1040);
    prior.pose.angles[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(fitHand(c, source, {}, &prior), std::invalid_argument);
}
TEST(AslHandKinematics, TemporalStateIsBoundToItsProfileAndSafeForEpochExtremes) {
    const auto c = fixture();
    auto previous = fitHand(c, observation(c, HandPose{}, std::numeric_limits<std::int64_t>::min()));
    auto changed = c; changed.boneLengths[0][0] *= 1.01;
    const auto next = fitHand(changed, observation(changed, articulated(), std::numeric_limits<std::int64_t>::max()), {}, &previous);
    ASSERT_TRUE(next.valid); EXPECT_TRUE(next.diagnostics.temporalReset);
    EXPECT_NE(next.handModelSignature, previous.handModelSignature);
}
TEST(AslHandKinematics, AllPalmLeastSquaresImprovesOnAnchorHeuristicForNoisyRoots) {
    const auto c = fixture(); auto source = observation(c, articulated());
    source.positions[5] += glm::dvec3(.08, -.06, .025);
    source.positions[9] += glm::dvec3(-.03, .02, -.01);
    source.positions[17] += glm::dvec3(.01, .04, -.02);
    HandPose prior;
    prior.wrist = source.positions[0];
    auto x = glm::normalize(source.positions[5] - source.positions[17]);
    auto y = (source.positions[5] + source.positions[17]) * .5 - prior.wrist;
    y = glm::normalize(y - x * glm::dot(x, y));
    prior.basis = {x, y, glm::cross(x, y)};
    std::array ratios{glm::length(source.positions[5] - prior.wrist) / glm::length(c.palmOffsets[1]),
                      glm::length(source.positions[17] - prior.wrist) / glm::length(c.palmOffsets[4]),
                      glm::length(source.positions[5] - source.positions[17]) / glm::length(c.palmOffsets[1] - c.palmOffsets[4])};
    std::sort(ratios.begin(), ratios.end()); prior.scale = ratios[1];
    const auto old = forwardHand(c, prior); const auto fit = fitHand(c, source);
    ASSERT_TRUE(fit.valid);
    double oldObjective = 0, newObjective = 0;
    for (const auto root : roots) {
        oldObjective += glm::dot(old[root] - source.positions[root], old[root] - source.positions[root]);
        newObjective += glm::dot(fit.positions[root] - source.positions[root], fit.positions[root] - source.positions[root]);
    }
    EXPECT_LT(newObjective, oldObjective - 1e-5);
    EXPECT_LT(glm::length(fit.pose.wrist - source.positions[0]), 1e-12);
}
TEST(AslHandKinematics, ProperPalmRotationRecoversHalfTurnForBothHandednesses) {
    const auto axis = glm::normalize(glm::dvec3(1, 2, 3));
    glm::dmat3 rotation(0.0);
    for (std::size_t c = 0; c < 3; ++c) for (std::size_t r = 0; r < 3; ++r)
        rotation[c][r] = 2 * axis[c] * axis[r] - (c == r ? 1.0 : 0.0);
    for (const auto h : {Handedness::Right, Handedness::Left}) {
        const auto c = fixture(h); auto p = articulated();
        p.basis = rotation; p.scale = 1.8; p.wrist = {2, -5, 1};
        const auto fit = fitHand(c, observation(c, p));
        ASSERT_TRUE(fit.valid);
        EXPECT_LT(fit.diagnostics.rmsResidual, 1e-5);
        EXPECT_NEAR(fit.pose.scale, 1.8, 1e-12);
        EXPECT_NEAR(glm::determinant(fit.pose.basis), 1.0, 1e-12);
    }
}
TEST(AslHandKinematics, AnatomicalPalmarCurlUsesIndependentFrontFacingPalmFixture) {
    // Explicit right palm facing a viewer at +world Z, with fingers towards +Y
    // and thumb to viewer-left. These observations are NOT generated by FK.
    HandObservation right;
    right.observed.fill(true);
    right.positions = {glm::dvec3(0, 0, 0), {-.035, .025, 0}, {-.06, .05, 0}, {-.08, .07, 0}, {-.10, .09, 0},
                       {-.03, .10, 0}, {-.03, .14, 0}, {-.03, .165, 0}, {-.03, .185, 0},
                       {-.01, .105, 0}, {-.01, .145, 0}, {-.01, .170, 0}, {-.01, .190, 0},
                       {.01, .095, 0}, {.01, .135, 0}, {.01, .160, 0}, {.01, .180, 0},
                       {.03, .08, 0}, {.03, .12, 0}, {.03, .145, 0}, {.03, .165, 0}};
    for (const auto h : {Handedness::Right, Handedness::Left}) {
        auto source = right;
        if (h == Handedness::Left) for (auto& p : source.positions) p.x = -p.x;
        const std::array frames{source}; const auto c = calibrateHand(frames, h);
        ASSERT_TRUE(c.valid);
        const auto neutral = fitHand(c, source); ASSERT_TRUE(neutral.valid);
        EXPECT_LT(neutral.diagnostics.rmsResidual, 1e-6);
        EXPECT_LT(neutral.pose.basis[2].z, -.999);
        for (const auto angle : {std::size_t(5), std::size_t(7), std::size_t(8)}) {
            auto curled = neutral.pose; curled.angles[angle] += .3;
            const auto points = forwardHand(c, curled);
            EXPECT_GT(points[8].z, .001) << "Positive palmar curl must move toward the viewer, angle " << angle;
            EXPECT_NEAR(points[5].z, 0, 1e-12);
            expectLengths(c, curled, points);
        }
        for (const auto angle : {std::size_t(0), std::size_t(3), std::size_t(4)}) {
            auto curled = neutral.pose; curled.angles[angle] += .3;
            EXPECT_GT(forwardHand(c, curled)[4].z, .001)
                << "Positive thumb palmar curl must move toward the viewer, angle " << angle;
        }
    }
}
} // namespace
