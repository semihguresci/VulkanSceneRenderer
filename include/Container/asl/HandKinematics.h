#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <glm/glm.hpp>

namespace container::asl {

// Experimental geometric hand constraints. Units follow the supplied coordinates;
// MediaPipe relative Z is accepted, but never becomes metric depth or force data.
enum class Handedness { Left, Right };
inline constexpr std::size_t HandLandmarkCount = 21;
inline constexpr std::size_t HandAngleCount = 21;
using HandAngles = std::array<double, HandAngleCount>;

struct HandObservation {
    std::array<glm::dvec3, HandLandmarkCount> positions{};
    std::array<bool, HandLandmarkCount> observed{};
    std::int64_t timestampMs{};
};

struct HandAngleLimit { double minimum{}, maximum{}; };
using HandAngleLimits = std::array<HandAngleLimit, HandAngleCount>;
const std::array<const char*, HandAngleCount>& handAngleNames();
HandAngleLimits experimentalHandAngleLimits();

struct HandCalibration {
    Handedness handedness{Handedness::Right};
    // Wrist-relative thumb CMC, index/middle/ring/pinky MCP in a RH palm basis.
    std::array<glm::dvec3, 5> palmOffsets{};
    // Three immutable lengths per digit; no independently scaled phalanges.
    std::array<std::array<double, 3>, 5> boneLengths{};
    std::size_t palmFrameCount{};
    std::array<std::array<std::size_t, 3>, 5> boneObservationCounts{};
    bool valid{};
    std::string message;
};

struct HandPose {
    glm::dvec3 wrist{};
    glm::dmat3 basis{1.0}; // Proper RH rotation, +Y towards fingers, +X handed palm axis.
    HandAngles angles{};   // Radians; positive flexion is palmar (-local Z), DIP independent.
    double scale{1.0};     // One global hand scale, estimated from palm only when requested.
};

struct HandFitOptions {
    HandAngleLimits limits{experimentalHandAngleLimits()};
    std::size_t maxIterations{64};
    double convergenceTolerance{1e-6};
    double damping{1e-3};
    double maxAngularVelocityRadPerSecond{0.0}; // Zero disables the optional bound.
    std::int64_t maxTemporalGapMs{250};
    bool estimateScaleFromPalm{true};
    double minimumScale{0.25};
    double maximumScale{4.0};
};

struct HandFitDiagnostics {
    std::size_t observedCount{};
    std::size_t observedArticulatedCount{};
    std::size_t iterations{};
    std::size_t anglesAtLimit{};
    double rmsResidual{};
    double maxResidual{};
    double maxBoneLengthError{};
    double objective{};
    bool velocityLimited{};
    bool temporalReset{};
    bool scaleClamped{};
    std::string message;
};

struct HandFit {
    HandPose pose;
    std::array<glm::dvec3, HandLandmarkCount> positions{};
    // Observed remains exactly the caller's mask; inferred never means observed.
    std::array<bool, HandLandmarkCount> observed{};
    std::array<bool, HandLandmarkCount> inferred{};
    std::int64_t timestampMs{};
    std::uint64_t handModelSignature{}; // Temporal state is bound to one immutable profile.
    HandFitDiagnostics diagnostics;
    bool valid{};
};

// Calibration belongs to a single clip/subject only. It does not consume labels
// and must not be pooled across training/validation/test identities.
HandCalibration calibrateHand(std::span<const HandObservation> frames, Handedness handedness);
std::array<glm::dvec3, HandLandmarkCount> forwardHand(const HandCalibration&, const HandPose&);
HandFit fitHand(const HandCalibration&, const HandObservation&, const HandFitOptions& = {},
                const HandFit* previous = nullptr);

} // namespace container::asl
