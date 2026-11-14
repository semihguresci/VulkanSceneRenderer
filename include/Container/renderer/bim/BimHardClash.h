#pragma once

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace container::renderer {

inline constexpr uint8_t kBimHardClashGroupA = 1;
inline constexpr uint8_t kBimHardClashGroupB = 2;

// CPU snapshot of one BIM element, independent of the live scene or GPU buffers.
// Coordinates use renderer world units and triangles use outward winding.
// Use one entry per object index and combine A/B memberships in groups.
struct BimHardClashElement {
  uint32_t objectIndex{};
  uint8_t groups{};
  std::vector<glm::dvec3> vertices;
  std::vector<std::array<uint32_t, 3>> triangles;
  std::string unavailableReason;
};

struct BimHardClashHit {
  uint32_t firstObjectIndex{}, secondObjectIndex{};
  double volume{}; // world units cubed
  glm::dvec3 position{}; // centre of the intersection bounds for the viewport marker
  glm::dvec3 boundsMin{}, boundsMax{}; // union of the two element bounds
};

struct BimHardClashSkipped {
  uint32_t objectIndex{};
  std::string reason;
};

struct BimHardClashResult {
  std::vector<BimHardClashHit> hits;
  std::vector<BimHardClashSkipped> skipped;
  std::vector<std::array<uint32_t, 2>> failedPairs;
  size_t checkedPairs{}; // completed solid intersections, including empty intersections
  bool cancelled{};
  bool pairLimitReached{};
};

struct BimHardClashLimits {
  size_t maxElements{100'000};
  size_t maxVerticesPerElement{2'000'000};
  size_t maxTrianglesPerElement{1'000'000};
  size_t maxTriangles{8'000'000};
  size_t maxCandidateTests{10'000'000};
  size_t maxBooleanPairs{100'000};
};

// Only A/B pairs are checked. Group overlap cannot duplicate pairs or compare
// an element against itself. Sort reports by original BIM object indices.
// A cancelled job returns an empty result with cancelled set.
[[nodiscard]] BimHardClashResult FindBimHardClashes(
    std::span<const BimHardClashElement>, std::stop_token stop = {},
    const BimHardClashLimits& limits = {});

}  // namespace container::renderer
