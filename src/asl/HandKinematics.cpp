#include "Container/asl/HandKinematics.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
#include <glm/gtc/quaternion.hpp>

namespace container::asl {
namespace {
constexpr std::array<std::size_t, 5> roots{1, 5, 9, 13, 17};
constexpr double epsilon = 1e-10;
constexpr double pi = 3.14159265358979323846;
using Points = std::array<glm::dvec3, HandLandmarkCount>;
bool finite(const glm::dvec3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
bool seen(const HandObservation& f, std::size_t j) { return f.observed[j] && finite(f.positions[j]); }
void validateCalibration(const HandCalibration& c) {
    if (!c.valid) throw std::invalid_argument("Hand calibration is invalid");
    for (const auto& p : c.palmOffsets)
        if (!finite(p)) throw std::invalid_argument("Hand palm offsets must be finite");
    for (const auto& digit : c.boneLengths) for (const double length : digit)
        if (!std::isfinite(length) || length <= epsilon)
            throw std::invalid_argument("Hand phalanx lengths must be finite and positive");
    const auto x = c.palmOffsets[1] - c.palmOffsets[4];
    const auto y = (c.palmOffsets[1] + c.palmOffsets[4]) * 0.5;
    if (glm::length(x) <= epsilon || glm::length(glm::cross(x, y)) <= epsilon)
        throw std::invalid_argument("Hand calibrated palm anchors must be non-collinear");
}
std::uint64_t signature(const HandCalibration& c) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto add = [&](std::uint64_t value) {
        for (std::size_t byte = 0; byte < 8; ++byte) { hash ^= value & 255; hash *= 1099511628211ULL; value >>= 8; }
    };
    add(c.handedness == Handedness::Right ? 1 : 0);
    for (const auto& p : c.palmOffsets) for (std::size_t axis = 0; axis < 3; ++axis) add(std::bit_cast<std::uint64_t>(p[axis]));
    for (const auto& digit : c.boneLengths) for (const double length : digit) add(std::bit_cast<std::uint64_t>(length));
    return hash;
}
double sign(Handedness h) { return h == Handedness::Right ? 1.0 : -1.0; }
glm::dmat3 rx(double a) {
    const double c = std::cos(a), s = std::sin(a);
    return {{1, 0, 0}, {0, c, s}, {0, -s, c}};
}
glm::dmat3 ry(double a) {
    const double c = std::cos(a), s = std::sin(a);
    return {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
}
glm::dmat3 rz(double a) {
    const double c = std::cos(a), s = std::sin(a);
    return {{c, s, 0}, {-s, c, 0}, {0, 0, 1}};
}

// The handedness sign keeps both bases proper rotations. Left/right anatomy is
// represented by mirrored palm offsets and abduction/twist signs, not a LH basis.
bool palmBasis(const HandObservation& f, Handedness h, glm::dmat3& basis) {
    if (!seen(f, 0) || !seen(f, 5) || !seen(f, 17)) return false;
    glm::dvec3 x = sign(h) * (f.positions[5] - f.positions[17]);
    const double width = glm::length(x);
    if (width <= epsilon) return false;
    x /= width;
    glm::dvec3 y = (f.positions[5] + f.positions[17]) * 0.5 - f.positions[0];
    y -= x * glm::dot(x, y);
    if (glm::length(y) <= epsilon) return false;
    y = glm::normalize(y);
    const glm::dvec3 z = glm::normalize(glm::cross(x, y));
    basis = glm::dmat3(x, y, z);
    return true;
}

// Horn's absolute-orientation quaternion, solved by deterministic symmetric
// Jacobi sweeps. Translation is deliberately fixed to the observed wrist; only
// one proper rotation and (optionally) one positive palm-only scale are fitted.
bool alignPalm(const HandCalibration& calibration, const HandObservation& observation,
               const HandFitOptions& options, HandPose& pose, bool& scaleClamped) {
    glm::dmat3 covariance(0.0);
    double referenceSquared = 0;
    for (std::size_t digit = 0; digit < 5; ++digit) if (seen(observation, roots[digit])) {
        const auto a = calibration.palmOffsets[digit];
        const auto b = observation.positions[roots[digit]] - pose.wrist;
        referenceSquared += glm::dot(a, a);
        // Stored as covariance[column][row]; S(row,col) = a(row)*b(col).
        for (std::size_t row = 0; row < 3; ++row) for (std::size_t col = 0; col < 3; ++col)
            covariance[col][row] += a[row] * b[col];
    }
    if (referenceSquared <= epsilon) return false;
    const auto s = [&](std::size_t row, std::size_t col) { return covariance[col][row]; };
    const double trace = s(0, 0) + s(1, 1) + s(2, 2);
    std::array<std::array<double, 4>, 4> matrix{{
        {trace, s(1, 2) - s(2, 1), s(2, 0) - s(0, 2), s(0, 1) - s(1, 0)},
        {s(1, 2) - s(2, 1), s(0, 0) - s(1, 1) - s(2, 2), s(0, 1) + s(1, 0), s(0, 2) + s(2, 0)},
        {s(2, 0) - s(0, 2), s(0, 1) + s(1, 0), -s(0, 0) + s(1, 1) - s(2, 2), s(1, 2) + s(2, 1)},
        {s(0, 1) - s(1, 0), s(0, 2) + s(2, 0), s(1, 2) + s(2, 1), -s(0, 0) - s(1, 1) + s(2, 2)}}};
    std::array<std::array<double, 4>, 4> eigenvectors{};
    for (std::size_t i = 0; i < 4; ++i) eigenvectors[i][i] = 1;
    double magnitude = 0;
    for (const auto& row : matrix) for (const double value : row) magnitude = std::max(magnitude, std::abs(value));
    if (!std::isfinite(magnitude) || magnitude <= epsilon) return false;
    for (std::size_t sweep = 0; sweep < 32; ++sweep) {
        bool changed = false;
        for (std::size_t p = 0; p < 4; ++p) for (std::size_t q = p + 1; q < 4; ++q) {
            const double off = matrix[p][q];
            if (std::abs(off) <= magnitude * 1e-14) continue;
            changed = true;
            const double tau = (matrix[q][q] - matrix[p][p]) / (2 * off);
            const double tangent = std::copysign(1.0 / (std::abs(tau) + std::hypot(1.0, tau)), tau);
            const double cosine = 1.0 / std::sqrt(1.0 + tangent * tangent);
            const double sine = tangent * cosine;
            matrix[p][p] -= tangent * off; matrix[q][q] += tangent * off;
            matrix[p][q] = matrix[q][p] = 0;
            for (std::size_t k = 0; k < 4; ++k) if (k != p && k != q) {
                const double kp = matrix[k][p], kq = matrix[k][q];
                matrix[k][p] = matrix[p][k] = cosine * kp - sine * kq;
                matrix[k][q] = matrix[q][k] = sine * kp + cosine * kq;
            }
            for (std::size_t k = 0; k < 4; ++k) {
                const double kp = eigenvectors[k][p], kq = eigenvectors[k][q];
                eigenvectors[k][p] = cosine * kp - sine * kq;
                eigenvectors[k][q] = sine * kp + cosine * kq;
            }
        }
        if (!changed) break;
    }
    std::size_t largest = 0;
    for (std::size_t i = 1; i < 4; ++i) if (matrix[i][i] > matrix[largest][largest]) largest = i;
    const auto quaternion = glm::normalize(glm::dquat(eigenvectors[0][largest], eigenvectors[1][largest],
                                                     eigenvectors[2][largest], eigenvectors[3][largest]));
    pose.basis = glm::mat3_cast(quaternion);
    if (options.estimateScaleFromPalm) {
        double numerator = 0;
        for (std::size_t digit = 0; digit < 5; ++digit) if (seen(observation, roots[digit]))
            numerator += glm::dot(pose.basis * calibration.palmOffsets[digit],
                                  observation.positions[roots[digit]] - pose.wrist);
        const double rawScale = numerator / referenceSquared;
        if (!std::isfinite(rawScale) || rawScale <= 0) return false;
        pose.scale = std::clamp(rawScale, options.minimumScale, options.maximumScale);
        scaleClamped = pose.scale != rawScale;
    }
    return true;
}
double median(std::vector<double> values) {
    if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) * 0.5;
}
double objective(const Points& positions, const HandObservation& observation, double normalizer) {
    double result = 0;
    for (std::size_t j = 0; j < HandLandmarkCount; ++j)
        if (seen(observation, j)) {
            const auto d = (positions[j] - observation.positions[j]) / normalizer;
            result += glm::dot(d, d);
        }
    return result;
}
bool solve(std::array<std::array<double, HandAngleCount>, HandAngleCount> a,
           HandAngles b, HandAngles& x) {
    for (std::size_t col = 0; col < HandAngleCount; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < HandAngleCount; ++row)
            if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
        if (std::abs(a[pivot][col]) < 1e-15) return false;
        std::swap(a[pivot], a[col]); std::swap(b[pivot], b[col]);
        const double divisor = a[col][col];
        for (std::size_t j = col; j < HandAngleCount; ++j) a[col][j] /= divisor;
        b[col] /= divisor;
        for (std::size_t row = 0; row < HandAngleCount; ++row) if (row != col) {
            const double factor = a[row][col];
            for (std::size_t j = col; j < HandAngleCount; ++j) a[row][j] -= factor * a[col][j];
            b[row] -= factor * b[col];
        }
    }
    x = b;
    return true;
}

HandAngles geometricSeed(const HandCalibration& calibration, const HandObservation& observation,
                         const HandPose& pose) {
    HandAngles result{};
    const auto local = glm::transpose(pose.basis);
    const double handed = sign(calibration.handedness);
    for (std::size_t digit = 0; digit < 5; ++digit) {
        const auto root = roots[digit];
        const auto angle = digit ? 5 + (digit - 1) * 4 : 0;
        const auto base = digit ? glm::dmat3(1.0) : rz(-std::atan2(handed * 0.8, 0.6));
        if (!seen(observation, root) || !seen(observation, root + 1)) continue;
        const auto direction = glm::transpose(base) * local *
                               (observation.positions[root + 1] - observation.positions[root]);
        result[angle + 1] = std::atan2(-direction.x, direction.y) / handed;
        const auto straightened = rz(-handed * result[angle + 1]) * direction;
        result[angle] = -std::atan2(straightened.z, straightened.y);
        auto hinge = pose.basis * base * rz(handed * result[angle + 1]) * glm::dvec3(1, 0, 0);
        for (std::size_t segment = 1; segment < 3; ++segment) {
            if (!seen(observation, root + segment - 1) || !seen(observation, root + segment) ||
                !seen(observation, root + segment + 1)) continue;
            const auto a = observation.positions[root + segment] - observation.positions[root + segment - 1];
            const auto b = observation.positions[root + segment + 1] - observation.positions[root + segment];
            const double value = -std::atan2(glm::dot(hinge, glm::cross(a, b)), glm::dot(a, b));
            result[angle + (digit ? 1 : 2) + segment] = value;
        }
    }
    return result;
}
} // namespace

const std::array<const char*, HandAngleCount>& handAngleNames() {
    static constexpr std::array<const char*, HandAngleCount> names{
        "thumb_cmc_flexion", "thumb_cmc_abduction", "thumb_cmc_twist", "thumb_mcp_flexion", "thumb_ip_flexion",
        "index_mcp_flexion", "index_mcp_abduction", "index_pip_flexion", "index_dip_flexion",
        "middle_mcp_flexion", "middle_mcp_abduction", "middle_pip_flexion", "middle_dip_flexion",
        "ring_mcp_flexion", "ring_mcp_abduction", "ring_pip_flexion", "ring_dip_flexion",
        "pinky_mcp_flexion", "pinky_mcp_abduction", "pinky_pip_flexion", "pinky_dip_flexion"};
    return names;
}

HandAngleLimits experimentalHandAngleLimits() {
    const auto rad = [](double degrees) { return degrees * pi / 180.0; };
    HandAngleLimits limits{};
    // Deliberately broad experimental ranges, not clinical or signer-specific ROM.
    limits[0] = {rad(-60), rad(90)};
    limits[1] = {rad(-65), rad(65)};
    limits[2] = {rad(-60), rad(60)}; // Thumb twist is weakly observed from landmarks.
    limits[3] = {rad(-20), rad(100)};
    limits[4] = {rad(-20), rad(100)};
    for (std::size_t digit = 0; digit < 4; ++digit) {
        const auto i = 5 + digit * 4;
        limits[i] = {rad(-30), rad(100)};
        limits[i + 1] = {rad(-40), rad(40)};
        limits[i + 2] = {rad(-15), rad(130)};
        limits[i + 3] = {rad(-20), rad(100)};
    }
    return limits;
}

HandCalibration calibrateHand(std::span<const HandObservation> frames, Handedness handedness) {
    HandCalibration result;
    result.handedness = handedness;
    std::array<std::array<std::vector<double>, 3>, 5> offsets, lengths;
    for (const auto& frame : frames) {
        for (std::size_t j = 0; j < HandLandmarkCount; ++j) if (frame.observed[j] && !finite(frame.positions[j])) {
            result.message = "An observed calibration point is nonfinite"; return result;
        }
        glm::dmat3 basis;
        if (palmBasis(frame, handedness, basis)) {
            ++result.palmFrameCount;
            for (std::size_t digit = 0; digit < 5; ++digit) if (seen(frame, roots[digit])) {
                const auto local = glm::transpose(basis) * (frame.positions[roots[digit]] - frame.positions[0]);
                for (std::size_t axis = 0; axis < 3; ++axis) offsets[digit][axis].push_back(local[axis]);
            }
        }
        for (std::size_t digit = 0; digit < 5; ++digit) for (std::size_t segment = 0; segment < 3; ++segment) {
            const auto joint = roots[digit] + segment;
            if (seen(frame, joint) && seen(frame, joint + 1)) {
                const double length = glm::length(frame.positions[joint + 1] - frame.positions[joint]);
                if (length > epsilon) lengths[digit][segment].push_back(length);
            }
        }
    }
    if (!result.palmFrameCount) { result.message = "No non-collinear observed wrist/index/pinky palm anchors"; return result; }
    for (std::size_t digit = 0; digit < 5; ++digit) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (offsets[digit][axis].empty()) { result.message = "A rigid palm root was never observed"; return result; }
            result.palmOffsets[digit][axis] = median(offsets[digit][axis]);
        }
        for (std::size_t segment = 0; segment < 3; ++segment) {
            result.boneObservationCounts[digit][segment] = lengths[digit][segment].size();
            if (lengths[digit][segment].empty()) { result.message = "A phalanx length was never observed"; return result; }
            result.boneLengths[digit][segment] = median(lengths[digit][segment]);
        }
    }
    result.valid = true;
    try { validateCalibration(result); }
    catch (const std::invalid_argument& e) { result.valid = false; result.message = e.what(); return result; }
    result.message = "Fixed per-clip median palm geometry and phalanx lengths";
    return result;
}

std::array<glm::dvec3, HandLandmarkCount> forwardHand(const HandCalibration& calibration, const HandPose& pose) {
    validateCalibration(calibration);
    if (!finite(pose.wrist) || !std::isfinite(pose.scale) || pose.scale <= 0)
        throw std::invalid_argument("Hand wrist/scale must be finite and scale positive");
    for (const double angle : pose.angles) if (!std::isfinite(angle))
        throw std::invalid_argument("Hand angles must be finite");
    for (std::size_t i = 0; i < 3; ++i)
        if (!finite(pose.basis[i])) throw std::invalid_argument("Hand basis must be finite");
    const auto gram = glm::transpose(pose.basis) * pose.basis;
    double orthogonality = 0;
    for (std::size_t c = 0; c < 3; ++c) for (std::size_t r = 0; r < 3; ++r)
        orthogonality = std::max(orthogonality, std::abs(gram[c][r] - (c == r ? 1.0 : 0.0)));
    if (orthogonality > 1e-6 || std::abs(glm::determinant(pose.basis) - 1.0) > 1e-6)
        throw std::invalid_argument("Hand basis must be a proper RH rotation");
    Points positions{};
    positions[0] = pose.wrist;
    const double handed = sign(calibration.handedness);
    for (std::size_t digit = 0; digit < 5; ++digit) {
        const auto root = roots[digit];
        const auto angle = digit ? 5 + (digit - 1) * 4 : 0;
        auto rotation = digit ? glm::dmat3(1.0) : rz(-std::atan2(handed * 0.8, 0.6));
        // The RH palm basis has +Z dorsally. Positive anatomical flexion bends
        // towards the palm (-local Z), for the right hand and its left mirror.
        rotation = rotation * rz(handed * pose.angles[angle + 1]) * rx(-pose.angles[angle]);
        if (!digit) rotation *= ry(handed * pose.angles[2]);
        positions[root] = pose.wrist + pose.basis * (calibration.palmOffsets[digit] * pose.scale);
        for (std::size_t segment = 0; segment < 3; ++segment) {
            if (segment) rotation *= rx(-pose.angles[angle + (digit ? 1 : 2) + segment]);
            positions[root + segment + 1] = positions[root + segment] + pose.basis * rotation *
                glm::dvec3(0, calibration.boneLengths[digit][segment] * pose.scale, 0);
        }
    }
    return positions;
}

HandFit fitHand(const HandCalibration& calibration, const HandObservation& observation,
                const HandFitOptions& options, const HandFit* previous) {
    HandFit result;
    result.observed = observation.observed;
    result.timestampMs = observation.timestampMs;
    for (std::size_t joint = 0; joint < HandLandmarkCount; ++joint)
        if (observation.observed[joint] && !finite(observation.positions[joint]))
            throw std::invalid_argument("An observed hand landmark is nonfinite");
    for (std::size_t joint = 0; joint < HandLandmarkCount; ++joint) if (seen(observation, joint)) {
        ++result.diagnostics.observedCount;
        if (joint != 0 && std::find(roots.begin(), roots.end(), joint) == roots.end())
            ++result.diagnostics.observedArticulatedCount;
    }
    const auto fail = [&](const std::string& message) {
        result.positions = observation.positions;
        result.diagnostics.message = message;
        return result;
    };
    if (!calibration.valid) return fail("Invalid per-clip calibration: " + calibration.message);
    validateCalibration(calibration);
    result.handModelSignature = signature(calibration);
    if (!std::isfinite(options.damping) || options.damping <= 0 ||
        !std::isfinite(options.convergenceTolerance) || options.convergenceTolerance <= 0 ||
        !std::isfinite(options.maxAngularVelocityRadPerSecond) || options.maxAngularVelocityRadPerSecond < 0 ||
        options.maxTemporalGapMs < 0 || options.maxIterations == 0 || options.maxIterations > 2048 ||
        !std::isfinite(options.minimumScale) || !std::isfinite(options.maximumScale) ||
        options.minimumScale <= 0 || options.minimumScale > options.maximumScale)
        throw std::invalid_argument("Invalid hand fitting options");
    for (const auto& limit : options.limits)
        if (!std::isfinite(limit.minimum) || !std::isfinite(limit.maximum) || limit.minimum > limit.maximum)
            throw std::invalid_argument("Invalid hand angle limits");
    if (!palmBasis(observation, calibration.handedness, result.pose.basis))
        return fail("Insufficient observed wrist/index/pinky palm anchors; raw frame retained");
    if (!result.diagnostics.observedArticulatedCount)
        return fail("No observed articulated finger points; raw frame retained");
    result.pose.wrist = observation.positions[0];
    if (!alignPalm(calibration, observation, options, result.pose, result.diagnostics.scaleClamped))
        return fail("Palm rigid alignment failed; raw frame retained");
    HandAngleLimits activeLimits = options.limits;
    // Subtract only after converting to long double: int64 epoch differences must
    // not overflow, and a non-increasing or large gap resets the temporal state.
    const long double elapsedMs = previous ? static_cast<long double>(observation.timestampMs) - previous->timestampMs : 0;
    if (previous && previous->valid) {
        // Reject corrupted caller-owned state rather than letting NaNs leak into bounds.
        (void)forwardHand(calibration, previous->pose);
    }
    const bool temporal = previous && previous->valid && previous->handModelSignature == result.handModelSignature &&
                          elapsedMs > 0 && elapsedMs <= options.maxTemporalGapMs;
    result.diagnostics.temporalReset = previous && !temporal;
    result.pose.angles = temporal ? previous->pose.angles : geometricSeed(calibration, observation, result.pose);
    if (temporal && options.maxAngularVelocityRadPerSecond > 0) {
        const double radius = options.maxAngularVelocityRadPerSecond * static_cast<double>(elapsedMs / 1000.0L);
        for (std::size_t angle = 0; angle < HandAngleCount; ++angle) {
            activeLimits[angle].minimum = std::max(activeLimits[angle].minimum, previous->pose.angles[angle] - radius);
            activeLimits[angle].maximum = std::min(activeLimits[angle].maximum, previous->pose.angles[angle] + radius);
            if (activeLimits[angle].minimum > activeLimits[angle].maximum)
                throw std::invalid_argument("Previous hand angles do not match current limits");
        }
    }
    for (std::size_t angle = 0; angle < HandAngleCount; ++angle)
        result.pose.angles[angle] = std::clamp(result.pose.angles[angle], activeLimits[angle].minimum, activeLimits[angle].maximum);
    double meanLength = 0;
    for (const auto& digit : calibration.boneLengths) for (const double length : digit) meanLength += length;
    const double normalizer = meanLength / 15.0 * result.pose.scale;
    auto current = forwardHand(calibration, result.pose);
    double error = objective(current, observation, normalizer);
    double damping = options.damping;
    for (std::size_t iteration = 0; iteration < options.maxIterations; ++iteration) {
        if (error <= options.convergenceTolerance * options.convergenceTolerance) break;
        ++result.diagnostics.iterations;
        std::array<Points, HandAngleCount> derivatives{};
        constexpr double delta = 1e-5;
        for (std::size_t angle = 0; angle < HandAngleCount; ++angle) {
            auto perturbed = result.pose;
            perturbed.angles[angle] += delta;
            const auto points = forwardHand(calibration, perturbed);
            for (std::size_t joint = 0; joint < HandLandmarkCount; ++joint)
                derivatives[angle][joint] = (points[joint] - current[joint]) / (delta * normalizer);
        }
        std::array<std::array<double, HandAngleCount>, HandAngleCount> normal{};
        HandAngles gradient{};
        for (std::size_t a = 0; a < HandAngleCount; ++a) {
            for (std::size_t joint = 0; joint < HandLandmarkCount; ++joint) if (seen(observation, joint)) {
                const auto residual = (observation.positions[joint] - current[joint]) / normalizer;
                gradient[a] += glm::dot(derivatives[a][joint], residual);
                for (std::size_t b = 0; b <= a; ++b)
                    normal[a][b] += glm::dot(derivatives[a][joint], derivatives[b][joint]);
            }
            for (std::size_t b = 0; b < a; ++b) normal[b][a] = normal[a][b];
            normal[a][a] += damping * (normal[a][a] + 1.0);
        }
        double gradientMaximum = 0;
        for (const double value : gradient) gradientMaximum = std::max(gradientMaximum, std::abs(value));
        if (gradientMaximum <= options.convergenceTolerance) break;
        HandAngles step{};
        if (!solve(normal, gradient, step)) break;
        bool accepted = false;
        double acceptedChange = 0;
        for (const double fraction : {1.0, 0.5, 0.25, 0.125, 0.0625}) {
            auto candidate = result.pose;
            double change = 0;
            for (std::size_t angle = 0; angle < HandAngleCount; ++angle) {
                candidate.angles[angle] = std::clamp(candidate.angles[angle] + fraction * std::clamp(step[angle], -0.4, 0.4),
                    activeLimits[angle].minimum, activeLimits[angle].maximum);
                change = std::max(change, std::abs(candidate.angles[angle] - result.pose.angles[angle]));
            }
            const auto candidatePoints = forwardHand(calibration, candidate);
            const double candidateError = objective(candidatePoints, observation, normalizer);
            if (candidateError < error) {
                result.pose = candidate; current = candidatePoints; error = candidateError;
                accepted = true; acceptedChange = change; damping = std::max(damping * 0.5, 1e-9); break;
            }
        }
        if (!accepted) { damping *= 10; if (damping > 1e8) break; }
        else if (acceptedChange < options.convergenceTolerance || error < options.convergenceTolerance * options.convergenceTolerance) break;
    }
    result.positions = current;
    double squareResidual = 0;
    for (std::size_t joint = 0; joint < HandLandmarkCount; ++joint) {
        result.inferred[joint] = !observation.observed[joint];
        if (seen(observation, joint)) {
            const double residual = glm::length(current[joint] - observation.positions[joint]);
            squareResidual += residual * residual;
            result.diagnostics.maxResidual = std::max(result.diagnostics.maxResidual, residual);
        }
    }
    result.diagnostics.rmsResidual = std::sqrt(squareResidual / result.diagnostics.observedCount);
    result.diagnostics.objective = error;
    for (std::size_t digit = 0; digit < 5; ++digit) for (std::size_t segment = 0; segment < 3; ++segment) {
        const auto joint = roots[digit] + segment;
        result.diagnostics.maxBoneLengthError = std::max(result.diagnostics.maxBoneLengthError,
            std::abs(glm::length(current[joint + 1] - current[joint]) - calibration.boneLengths[digit][segment] * result.pose.scale));
    }
    for (std::size_t angle = 0; angle < HandAngleCount; ++angle) {
        const double value = result.pose.angles[angle];
        const auto near = [value](double boundary) { return std::abs(value - boundary) <= 1e-6; };
        if (near(options.limits[angle].minimum) || near(options.limits[angle].maximum)) ++result.diagnostics.anglesAtLimit;
        if (temporal && options.maxAngularVelocityRadPerSecond > 0 &&
            ((activeLimits[angle].minimum > options.limits[angle].minimum && near(activeLimits[angle].minimum)) ||
             (activeLimits[angle].maximum < options.limits[angle].maximum && near(activeLimits[angle].maximum))))
            result.diagnostics.velocityLimited = true;
    }
    result.valid = true;
    result.diagnostics.message = "Bounded geometric fit; inferred points retain an unobserved mask";
    return result;
}
} // namespace container::asl
