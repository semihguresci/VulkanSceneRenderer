#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

namespace container::ui::bcf {
struct BcfMarkup;
struct BcfViewpointMarkup;
}

namespace container::renderer {

inline constexpr uint32_t kInvalidBimCoordinationOverlayObjectIndex =
    std::numeric_limits<uint32_t>::max();

enum class BimCoordinationOverlayKind : uint32_t {
  Space = 0,
  MepXray = 1,
  Clash = 2,
  IssuePin = 3,
};

struct BimCoordinationOverlayBounds {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  bool valid{false};
};

struct BimCoordinationOverlayElement {
  uint32_t objectIndex{kInvalidBimCoordinationOverlayObjectIndex};
  std::string ifcClass{};
  std::string type{};
  std::string name{};
  std::string guid{};
  BimCoordinationOverlayBounds bounds{};
};

struct BimCoordinationOverlayClashPair {
  uint32_t primaryObjectIndex{kInvalidBimCoordinationOverlayObjectIndex};
  uint32_t secondaryObjectIndex{kInvalidBimCoordinationOverlayObjectIndex};
  bool hasPosition{false};
  glm::vec3 position{0.0f};
  std::string label{};
};

struct BimCoordinationOverlayIssuePin {
  glm::vec3 position{0.0f};
  glm::vec3 color{0.12f, 0.48f, 1.0f};
  std::string label{};
  std::string ifcGuid{};
  std::string sourceId{};
  uint32_t primaryObjectIndex{kInvalidBimCoordinationOverlayObjectIndex};
  uint32_t secondaryObjectIndex{kInvalidBimCoordinationOverlayObjectIndex};
};

struct BimCoordinationOverlayMarker {
  BimCoordinationOverlayKind kind{BimCoordinationOverlayKind::IssuePin};
  glm::vec3 position{0.0f};
  glm::vec3 color{1.0f};
  std::string label{};
  std::string ifcGuid{};
  std::string sourceId{};
  uint32_t primaryObjectIndex{kInvalidBimCoordinationOverlayObjectIndex};
  uint32_t secondaryObjectIndex{kInvalidBimCoordinationOverlayObjectIndex};
};

struct BimCoordinationOverlayElementRecord {
  BimCoordinationOverlayKind kind{BimCoordinationOverlayKind::Space};
  uint32_t objectIndex{kInvalidBimCoordinationOverlayObjectIndex};
  BimCoordinationOverlayBounds bounds{};
  glm::vec3 color{1.0f};
  float opacity{1.0f};
  bool transparent{true};
  std::string label{};
};

struct BimCoordinationOverlayBuildOptions {
  bool spacesEnabled{true};
  bool mepXrayEnabled{true};
  bool clashesEnabled{true};
  bool issuePinsEnabled{true};
};

struct BimCoordinationOverlayInputs {
  std::span<const BimCoordinationOverlayElement> elements{};
  std::span<const BimCoordinationOverlayClashPair> clashPairs{};
  std::span<const BimCoordinationOverlayIssuePin> issuePins{};
  BimCoordinationOverlayBuildOptions options{};
};

struct BimCoordinationOverlayResult {
  std::vector<BimCoordinationOverlayMarker> markers{};
  std::vector<BimCoordinationOverlayElementRecord> elementOverlays{};

  [[nodiscard]] bool empty() const {
    return markers.empty() && elementOverlays.empty();
  }
};

struct BimCoordinationOverlayMarkerWirePrimitive {
  BimCoordinationOverlayKind kind{BimCoordinationOverlayKind::IssuePin};
  glm::vec3 color{1.0f};
  std::string label{};
  uint32_t primaryObjectIndex{kInvalidBimCoordinationOverlayObjectIndex};
  uint32_t secondaryObjectIndex{kInvalidBimCoordinationOverlayObjectIndex};
  std::vector<glm::vec3> positions{};
  std::vector<uint32_t> indices{};
};

[[nodiscard]] bool isBimCoordinationSpaceClass(std::string_view value);
[[nodiscard]] bool isBimCoordinationMepXrayClass(std::string_view value);

[[nodiscard]] BimCoordinationOverlayResult
buildBimCoordinationOverlay(const BimCoordinationOverlayInputs &inputs);

[[nodiscard]] std::vector<BimCoordinationOverlayMarkerWirePrimitive>
buildBimCoordinationOverlayMarkerWirePrimitives(
    std::span<const BimCoordinationOverlayMarker> markers,
    float markerRadius);

[[nodiscard]] std::vector<BimCoordinationOverlayIssuePin>
exportBcfPinsAsIssueOverlayMarkers(
    const container::ui::bcf::BcfMarkup &markup);

[[nodiscard]] std::vector<BimCoordinationOverlayIssuePin>
exportBcfPinsAsIssueOverlayMarkers(
    const container::ui::bcf::BcfViewpointMarkup &markup);

} // namespace container::renderer
