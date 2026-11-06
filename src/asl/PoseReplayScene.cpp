#include "Container/asl/PoseReplayScene.h"
#include "Container/common/CommonGLFW.h"
#include "Container/renderer/scene/ScenePrimitives.h"
#include "Container/utility/Material.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/VulkanMemoryManager.h"
#include "Container/utility/SceneManager.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

namespace container::asl {
namespace {
size_t groupFor(size_t i) { return i < 21 ? 0 : i < 42 ? 1 : i < 75 ? 2 : 3; }
float radiusFor(size_t i) { return i < 42 ? 0.008f : i < 75 ? 0.012f : 0.0025f; }
}

PoseReplayScene::PoseReplayScene(PoseReplay replay, scene::SceneGraph &graph, scene::SceneManager &manager,
                               double startMs, double speed, bool loop, bool handKinematics,
                               double handAspectRatio, double handAngularVelocity,
                               const std::filesystem::path &handReport)
    : replay_(std::move(replay)), graph_(graph), graphIdentity_(graph.identity()), loop_(loop) {
  if (handKinematics) {
    auto options = replayHandFitOptions();
    options.maxAngularVelocityRadPerSecond = handAngularVelocity;
    handReplay_.emplace(replay_, handAspectRatio, options);
    if (!handReport.empty()) {
      // C++23 noreplace makes the report opt-in and refuses to overwrite an
      // existing observation, report or unrelated user file, including races.
      std::ofstream report(handReport, std::ios::out | std::ios::binary | std::ios::noreplace);
      if (!report) throw std::runtime_error("Cannot create a new ASL hand fit report");
      report << handReplay_->fitDocument().dump(2) << '\n';
      report.close();
      if (!report) {
        std::error_code ignored;
        std::filesystem::remove(handReport, ignored);
        throw std::runtime_error("Cannot finish ASL hand fit report");
      }
    }
  } else if (!handReport.empty()) {
    throw std::invalid_argument("A hand fit report requires enabled hand kinematics");
  }
  // An explicit ASL scene hides the startup model; other scene payloads remain.
  const auto oldCount = graph_.nodeCount();
  for (uint32_t i = 0; i < oldCount; ++i) graph_.setVisible(i, false);
  const std::array<glm::vec4, 4> colors{{{0.1f, .65f, 1, 1}, {1, .45f, .08f, 1},
                                     {.25f, 1, .4f, 1}, {.85f, .55f, 1, 1}}};
  std::array<uint32_t, 4> materials{};
  for (size_t i = 0; i < materials.size(); ++i) {
    material::Material material{};
    material.baseColor = colors[i];
    material.unlit = true;
    materials[i] = manager.createMaterial(material);
  }
  manager.uploadMaterialResources();
  const auto sphere = manager.appendRuntimeMesh(renderer::makeScenePrimitiveSphere(
      {.radius = .5f, .latitudeSegments = 6, .longitudeSegments = 8}));
  const auto cube = manager.appendRuntimeMesh(renderer::makeScenePrimitiveCube());
  const auto root = graph_.createNode(glm::mat4(1), 0, false, scene::SceneGraph::kInvalidNode,
                                      "ASL observations (image XY; Z is not metric)");
  for (size_t i = 0; i < kLandmarkCount; ++i) {
    nodes_[i] = graph_.createNode(glm::mat4(1), materials[groupFor(i)], true, sphere,
                                 "ASL joint " + std::to_string(i));
    graph_.setParent(nodes_[i], root);
  }
  const auto addBone = [&](size_t from, size_t to) {
    const auto node = graph_.createNode(glm::mat4(1), materials[groupFor(from)], true, cube,
                                        "ASL bone " + std::to_string(from) + "-" + std::to_string(to));
    graph_.setParent(node, root);
    bones_.push_back({from, to, node});
  };
  for (const size_t base : {0u, 21u}) {
    for (const size_t tip : {1u, 5u, 9u, 13u, 17u}) {
      addBone(base, base + tip);
      for (size_t j = tip; j < tip + 3; ++j) addBone(base + j, base + j + 1);
    }
    addBone(base + 5, base + 9); addBone(base + 9, base + 13); addBone(base + 13, base + 17);
  }
  constexpr std::pair<size_t, size_t> body[]{{11,12},{11,13},{13,15},{15,17},{15,19},{15,21},
      {12,14},{14,16},{16,18},{16,20},{16,22},{11,23},{12,24},{23,24},{23,25},{24,26},
      {25,27},{26,28},{27,29},{28,30},{29,31},{30,32}};
  for (const auto [a,b] : body) addBone(42 + a, 42 + b);
  clock_.seek(startMs);
  clock_.setSpeed(speed);
  apply();
}

void PoseReplayScene::apply() {
  // A user model reload may replace the graph. Never mutate reused node IDs.
  if (graph_.identity() != graphIdentity_) return;
  const auto index = replay_.frameIndexAt(clock_.elapsedMs(), loop_);
  if (index == displayedFrame_) return;
  displayedFrame_ = index;
  const auto &frame = handReplay_ && showFitted_ ? handReplay_->frames()[index] : replay_.frames()[index];
  for (size_t i = 0; i < kLandmarkCount; ++i) {
    const auto &point = frame.points[i];
    graph_.setVisible(nodes_[i], point.valid);
    if (point.valid) {
      const auto transform = glm::scale(glm::translate(glm::mat4(1), imagePlanePosition(point.xyz)),
                                        glm::vec3(2 * radiusFor(i)));
      graph_.setLocalTransform(nodes_[i], transform);
    }
  }
  for (const auto &bone : bones_) {
    const auto &from = frame.points[bone.from], &to = frame.points[bone.to];
    const auto a = imagePlanePosition(from.xyz), b = imagePlanePosition(to.xyz);
    const auto delta = b - a;
    const float length = glm::length(delta);
    const bool visible = from.valid && to.valid && length > 1e-6f;
    graph_.setVisible(bone.node, visible);
    if (visible) {
      // The unit cube's +Y axis follows the image-plane segment. Positive
      // scales preserve winding; no projection-level Y flip is introduced.
      const auto rotation = glm::rotate(glm::mat4(1), std::atan2(-delta.x, delta.y), glm::vec3(0,0,1));
      const float thickness = bone.from < 42 ? .005f : .008f;
      graph_.setLocalTransform(bone.node, glm::scale(glm::translate(glm::mat4(1), (a+b)*.5f) * rotation,
                                                    glm::vec3(thickness, length, thickness)));
    }
  }
  graph_.updateWorldTransforms();
}

void PoseReplayScene::tick(double seconds, GLFWwindow *window) {
  if (graph_.identity() != graphIdentity_) return;
  if (window) {
    const bool pause = glfwGetKey(window, GLFW_KEY_F8) == GLFW_PRESS;
    const bool restart = glfwGetKey(window, GLFW_KEY_F9) == GLFW_PRESS;
    const bool compare = glfwGetKey(window, GLFW_KEY_F10) == GLFW_PRESS;
    if (pause && !pauseKey_) clock_.togglePause();
    if (restart && !restartKey_) clock_.restart();
    if (handReplay_ && compare && !compareKey_) {
      showFitted_ = !showFitted_;
      displayedFrame_ = std::numeric_limits<size_t>::max();
    }
    pauseKey_ = pause; restartKey_ = restart; compareKey_ = compare;
  }
  clock_.advance(seconds);
  apply();
  if (window) {
    auto title = "ASL pose | " + replay_.label() + " | " +
        std::to_string(displayedFrame_+1) + "/" + std::to_string(replay_.frames().size()) +
        (clock_.paused() ? " | paused" : " | playing") + " | image XY | F8 pause, F9 restart";
    if (handReplay_) {
      title += showFitted_ ? " | FK/IK" : " | raw comparison";
      if (showFitted_) {
        const auto &fits = handReplay_->fits()[displayedFrame_];
        for (size_t hand = 0; hand < 2; ++hand) {
          title += hand == 0 ? " L:" : " R:";
          if (fits[hand].valid) {
            std::ostringstream residual;
            residual << std::fixed << std::setprecision(4) << fits[hand].diagnostics.rmsResidual;
            title += "rms " + residual.str();
          } else title += fits[hand].diagnostics.observedCount == 0 ? "absent" : "raw fallback";
        }
      }
      title += " | F10 compare";
    }
    glfwSetWindowTitle(window, title.c_str());
  }
}
} // namespace container::asl
