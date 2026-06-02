#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace container::renderer {

enum class TechniqueDebugControlKind {
  Toggle,
  Integer,
  Float,
  Enum,
  Action,
};

struct TechniqueDebugOption {
  std::string id{};
  std::string label{};
};

struct TechniqueDebugControl {
  std::string id{};
  std::string label{};
  TechniqueDebugControlKind kind{TechniqueDebugControlKind::Toggle};
  double minValue{0.0};
  double maxValue{1.0};
  double defaultValue{0.0};
  std::vector<TechniqueDebugOption> options{};
};

struct TechniqueDebugPanel {
  std::string id{};
  std::string title{};
  std::vector<TechniqueDebugControl> controls{};
};

struct TechniqueDisplayModeOption {
  std::string id{};
  std::string label{};
  uint32_t value{0};
};

struct TechniqueDebugModel {
  std::string techniqueName{};
  std::string displayName{};
  std::vector<TechniqueDisplayModeOption> displayModes{};
  std::vector<TechniqueDebugPanel> panels{};
};

struct RenderGraphPassDebugState {
  std::string passName{};
  bool enabled{true};
  bool active{true};
  bool locked{false};
  bool autoDisabled{false};
  std::string skipReason{};
  std::string dependencyNote{};
};

struct RenderGraphDebugModel {
  std::vector<RenderGraphPassDebugState> passes{};
};

struct SceneProviderDebugState {
  std::string providerId{};
  std::string displayName{};
  std::string kind{};
  std::size_t elementCount{0};
};

struct SceneDebugModel {
  std::vector<SceneProviderDebugState> providers{};
};

}  // namespace container::renderer
