#pragma once

#include <Container/geometry/GltfAnimation.h>

#include <array>
#include <memory>
#include <span>
#include <string_view>

namespace container::geometry::gltf {

inline constexpr char kGltfInteractivityExtension[] = "KHR_interactivity";

class GltfInteractivity {
public:
  enum class NodeEvent { Select, HoverIn, HoverOut };
  using Point = std::array<double, 3>;

  // Compiles the default graph and throws a contextual invalid_argument for a
  // malformed graph or an operation outside the supported profile.
  explicit GltfInteractivity(const tinygltf::Model& source);
  ~GltfInteractivity();
  GltfInteractivity(const GltfInteractivity&) = delete;
  GltfInteractivity& operator=(const GltfInteractivity&) = delete;

  [[nodiscard]] bool available() const;
  [[nodiscard]] bool playing() const;
  [[nodiscard]] bool started() const;
  [[nodiscard]] double time() const;
  [[nodiscard]] size_t activeClips() const;
  [[nodiscard]] const std::string& diagnostic() const;
  [[nodiscard]] const std::vector<std::string>& customEvents() const;
  void setPlaying(bool);

  // Events are queued on the render owner, then dispatched in order before
  // the next tick. Node chains run from the nearest eligible node upward.
  bool queueCustomEvent(std::string_view id);
  bool queueNodeEvent(NodeEvent, std::vector<size_t> sourceChain,
                      std::optional<Point> point = {}, std::optional<Point> rayOrigin = {});
  AnimationDirtySet advance(tinygltf::Model&, const AnimationLibrary&, double deltaSeconds);
  AnimationDirtySet reset(tinygltf::Model&);

private:
  struct State;
  std::unique_ptr<State> state_;
};

// Metadata-only validation shares the runtime compiler and allocates no GPU
// resources. Optional unsupported graphs can be surfaced without rejecting the
// static scene, while required graphs fail at the owning load boundary.
void ValidateGltfInteractivity(const tinygltf::Model& source);

enum class InteractivityIndexDomain { Node, NodeWeights, Material, Light, Camera, Animation, Count };
struct InteractivityIndexMapping {
  // Empty map means identity for the source table. A negative entry denotes
  // a missing or ambiguous destination. The resulting table can be larger.
  std::span<const int32_t> oldToNew{};
  size_t destinationSize{};
};
using GltfInteractivityMappings = std::array<InteractivityIndexMapping,
    size_t(InteractivityIndexDomain::Count)>;

// Preserve all supported graphs and annotations in a detached extension.
// Authored pointer templates with an object-index placeholder require an
// identity mapping over the complete original and destination domains.
tinygltf::Value RemapGltfInteractivity(const tinygltf::Model& source,
    const GltfInteractivityMappings& mappings, const tinygltf::Model* projected = nullptr);
std::vector<size_t> GltfInteractivityTransformNodes(const tinygltf::Model& source);
// An unresolved clip input can refer to any clip and returns nullopt.
std::optional<std::vector<size_t>> GltfInteractivityAnimationClips(const tinygltf::Model& source);

} // namespace container::geometry::gltf
