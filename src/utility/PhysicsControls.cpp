#include "Container/utility/PhysicsControls.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace container::ui {
namespace {

void normalizeOptions(PhysicsPanelOptions &options, bool existingBody) {
  using physics::PhysicsMotionType;
  if (options.motionType != PhysicsMotionType::Static &&
      options.motionType != PhysicsMotionType::Kinematic &&
      options.motionType != PhysicsMotionType::Dynamic)
    options.motionType = PhysicsMotionType::Dynamic;
  if (existingBody) {
    // Existing bodies can have valid API/capture values outside the convenient
    // limits used for new attachment drafts. Merely selecting/editing another
    // property must not change those values.
    if (!std::isfinite(options.mass) || options.mass <= 0 || !std::isfinite(1.0f / options.mass))
      options.mass = 1.0f;
    options.friction = std::isfinite(options.friction) ? std::max(options.friction, 0.0f) : 0.5f;
  } else {
    options.mass = std::isfinite(options.mass)
        ? std::clamp(options.mass, 0.001f, 1'000'000.0f) : 1.0f;
    options.friction = std::isfinite(options.friction)
        ? std::clamp(options.friction, 0.0f, 1.0f) : 0.5f;
  }
  options.restitution = std::isfinite(options.restitution)
      ? std::clamp(options.restitution, 0.0f, 1.0f) : 0.1f;
  options.gravityFactor = std::isfinite(options.gravityFactor)
      ? (existingBody ? options.gravityFactor : std::clamp(options.gravityFactor, -10.0f, 10.0f)) : 1.0f;
}

const char *motionName(physics::PhysicsMotionType motion) {
  switch (motion) {
  case physics::PhysicsMotionType::Static: return "Static";
  case physics::PhysicsMotionType::Kinematic: return "Kinematic";
  case physics::PhysicsMotionType::Dynamic: return "Dynamic";
  }
  return "Dynamic";
}

const char *jointName(physics::PhysicsJointType type) {
  switch (type) {
  case physics::PhysicsJointType::Fixed: return "Fixed";
  case physics::PhysicsJointType::Hinge: return "Hinge";
  case physics::PhysicsJointType::Slider: return "Slider";
  }
  return "Fixed";
}

bool finiteFloat(double value) {
  return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
}

std::string jointMotorSettings(const PhysicsPanelOptions &options,
                                 physics::PhysicsJointMotorSettings &motor) {
  using physics::PhysicsJointMotorMode;
  if (options.jointType == physics::PhysicsJointType::Fixed)
    return {};
  motor.mode = options.jointMotorMode;
  if (motor.mode == PhysicsJointMotorMode::Off)
    return {};
  if (!std::isfinite(options.jointMotorMaxForceOrTorque) ||
      options.jointMotorMaxForceOrTorque <= 0)
    return "Motor strength must be finite and positive.";
  motor.maxForceOrTorque = options.jointMotorMaxForceOrTorque;
  const bool hinge = options.jointType == physics::PhysicsJointType::Hinge;
  // Hidden drafts remain in options when the mode changes. Only send fields
  // used by the selected mode; inactive fields retain finite API defaults.
  if (motor.mode == PhysicsJointMotorMode::Velocity) {
    const double velocity = hinge ? glm::radians(options.hingeTargetVelocityDegrees)
                                  : options.sliderTargetVelocity;
    if (!finiteFloat(velocity))
      return "Motor velocity must be finite and representable.";
    motor.targetVelocity = static_cast<float>(velocity);
  } else {
    const double position = hinge ? glm::radians(options.hingeTargetPositionDegrees)
                                  : options.sliderTargetPosition;
    if (!finiteFloat(position) || !std::isfinite(options.jointMotorFrequencyHz) ||
        options.jointMotorFrequencyHz <= 0 ||
        !std::isfinite(options.jointMotorDampingRatio) || options.jointMotorDampingRatio < 0)
      return "Position motor needs a finite target, positive frequency and nonnegative damping.";
    motor.targetPosition = static_cast<float>(position);
    motor.frequencyHz = options.jointMotorFrequencyHz;
    motor.dampingRatio = options.jointMotorDampingRatio;
  }
  return {};
}

std::string jointSettings(const PhysicsPanelState &state,
                           const PhysicsPanelOptions &options,
                           physics::PhysicsJointSettings &settings) {
  using physics::PhysicsJointType;
  settings.type = options.jointType;
  settings.anchor = options.autoJointAnchor ? state.suggestedJointAnchor : options.jointAnchor;
  for (int axis = 0; axis < 3; ++axis)
    if (!std::isfinite(settings.anchor[axis])) return "Anchor must contain finite coordinates.";
  settings.collideConnected = options.jointCollideConnected;
  if (options.jointType == PhysicsJointType::Fixed) return {};
  const glm::dvec3 direction{options.jointAxis};
  const double length = glm::length(direction);
  if (!std::isfinite(length) || length <= 1e-6)
    return "Axis must be finite and nonzero.";
  settings.axis = glm::vec3{direction / length};
  const auto helper = std::abs(settings.axis.y) < .9f ? glm::vec3{0, 1, 0} : glm::vec3{1, 0, 0};
  settings.normal = glm::normalize(glm::cross(settings.axis, helper));
  const bool hinge = options.jointType == PhysicsJointType::Hinge;
  const double lower = hinge ? glm::radians(options.hingeLowerDegrees) : options.sliderLowerMeters;
  const double upper = hinge ? glm::radians(options.hingeUpperDegrees) : options.sliderUpperMeters;
  if (options.jointLimitsEnabled &&
      (!finiteFloat(lower) || !finiteFloat(upper) || lower >= upper || lower > 0 || upper < 0 ||
       (hinge && (options.hingeLowerDegrees < -180.0 || options.hingeUpperDegrees > 180.0))))
    return hinge ? "Hinge limits need a nonzero range containing zero within -180..180 degrees; use Fixed to lock motion."
                 : "Slider limits need a finite nonzero range containing zero; use Fixed to lock motion.";
  settings.limitsEnabled = options.jointLimitsEnabled;
  if (settings.limitsEnabled) {
    settings.lowerLimit = static_cast<float>(lower);
    settings.upperLimit = static_cast<float>(upper);
  }
  return {};
}

} // namespace

void drawPhysicsPanel(const PhysicsPanelState &state, PhysicsPanelOptions &options) {
  const bool existingBody = state.selectedHasBody &&
      state.selectedBodyId != physics::kInvalidPhysicsBodyId && state.selectedBodySettings.has_value();
  if (existingBody) {
    if (options.lastEditedBodyId != state.selectedBodyId) {
      const auto &settings = *state.selectedBodySettings;
      options.motionType = settings.motionType;
      options.mass = settings.mass;
      options.friction = settings.friction;
      options.restitution = settings.restitution;
      options.useCCD = settings.useCCD;
      options.gravityFactor = settings.gravityFactor;
      options.lastEditedBodyId = state.selectedBodyId;
    }
  } else {
    // Retain the draft for new attachments. Returning to an existing body
    // reloads that body's settings after visiting an unbound selection.
    options.lastEditedBodyId = physics::kInvalidPhysicsBodyId;
  }
  normalizeOptions(options, existingBody);
  if (!ImGui::TreeNode("Physics"))
    return;

  if (!state.available)
    ImGui::TextDisabled("Physics is disabled in this build.");
  ImGui::Text("Bodies: %zu (%zu active)", state.bodyCount, state.activeBodyCount);
  ImGui::Text("Simulation steps: %llu", static_cast<unsigned long long>(state.totalSteps));
  if (!state.selectionLabel.empty())
    ImGui::TextWrapped("Selection: %s", state.selectionLabel.c_str());
  if (state.selectedHasBody && state.selectedMotion)
    ImGui::Text("Selected body: %s", motionName(*state.selectedMotion));
  else if (!state.selectionAvailable)
    ImGui::TextDisabled("Select a scene object to attach a body.");

  ImGui::BeginDisabled(!state.available);
  int motion = static_cast<int>(options.motionType);
  if (ImGui::Combo("Motion", &motion, "Static\0Kinematic\0Dynamic\0"))
    options.motionType = static_cast<physics::PhysicsMotionType>(motion);
  ImGui::BeginDisabled(options.motionType != physics::PhysicsMotionType::Dynamic);
  ImGui::InputFloat("Mass (kg)", &options.mass, 1.0f, 10.0f, "%.3f");
  ImGui::EndDisabled();
  const float maximumFriction = existingBody && std::isfinite(state.selectedBodySettings->friction)
      ? std::max(1.0f, state.selectedBodySettings->friction) : 1.0f;
  ImGui::SliderFloat("Friction", &options.friction, 0.0f, maximumFriction, "%.2f");
  ImGui::SliderFloat("Restitution", &options.restitution, 0.0f, 1.0f, "%.2f");
  ImGui::BeginDisabled(options.motionType != physics::PhysicsMotionType::Dynamic);
  ImGui::Checkbox("Continuous collision", &options.useCCD);
  ImGui::InputFloat("Gravity factor", &options.gravityFactor, .1f, 1.0f, "%.2f");
  ImGui::EndDisabled();
  normalizeOptions(options, existingBody);

  ImGui::BeginDisabled(!state.selectionAvailable || !state.attachSelected);
  if (ImGui::Button(state.selectedHasBody ? "Replace body" : "Attach body")) {
    auto settings = existingBody ? *state.selectedBodySettings : physics::PhysicsBodySettings{};
    settings.motionType = options.motionType;
    settings.mass = options.mass;
    settings.friction = options.friction;
    settings.restitution = options.restitution;
    if (options.motionType == physics::PhysicsMotionType::Dynamic) {
      settings.useCCD = options.useCCD;
      settings.gravityFactor = options.gravityFactor;
    } else if (!existingBody || options.motionType != state.selectedBodySettings->motionType) {
      settings.useCCD = false;
      settings.gravityFactor = 1.0f;
    }
    state.attachSelected(settings);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!state.selectionAvailable || !state.selectedHasBody ||
                        !state.detachSelected);
  if (ImGui::Button("Detach body"))
    state.detachSelected();
  ImGui::EndDisabled();

  const bool noBodies = state.bodyCount == 0u;
  ImGui::BeginDisabled(noBodies || !state.setPlaying);
  if (ImGui::Button(state.playing ? "Pause" : "Play"))
    state.setPlaying(!state.playing);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(noBodies || !state.singleStep);
  if (ImGui::Button("Single step"))
    state.singleStep();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(noBodies || !state.reset);
  if (ImGui::Button("Reset"))
    state.reset();
  ImGui::EndDisabled();
  ImGui::BeginDisabled(noBodies || !state.clear);
  if (ImGui::Button("Clear bodies"))
    state.clear();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!state.createDemo);
  if (ImGui::Button("Create demo"))
    state.createDemo();
  ImGui::EndDisabled();
  ImGui::EndDisabled();

  drawPhysicsJointPanel(state, options);

  if (!state.error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.25f, 1.0f));
    ImGui::TextWrapped("%s", state.error.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::TreePop();
}

void drawPhysicsJointPanel(const PhysicsPanelState &state, PhysicsPanelOptions &options) {
  using physics::PhysicsJointType;
  using physics::PhysicsJointMotorMode;
  if (!ImGui::TreeNode("Joints")) return;
  if (options.jointType != PhysicsJointType::Fixed && options.jointType != PhysicsJointType::Hinge &&
      options.jointType != PhysicsJointType::Slider)
    options.jointType = PhysicsJointType::Hinge;
  if (options.jointMotorMode != PhysicsJointMotorMode::Off &&
      options.jointMotorMode != PhysicsJointMotorMode::Velocity &&
      options.jointMotorMode != PhysicsJointMotorMode::Position)
    options.jointMotorMode = PhysicsJointMotorMode::Off;

  ImGui::TextWrapped("Choose a first body or world, then select an attached body.");
  ImGui::TextWrapped("First body: %s", state.jointFirstChosen ? state.jointFirstBodyLabel.c_str() : "None");
  ImGui::BeginDisabled(!state.available || !state.selectedHasBody || !state.chooseFirstJointBody);
  if (ImGui::Button("Use selected as first")) state.chooseFirstJointBody();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!state.available || !state.chooseWorldJointBody);
  if (ImGui::Button("Use world")) state.chooseWorldJointBody();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!state.available || !state.jointFirstChosen || !state.clearFirstJointBody);
  if (ImGui::Button("Clear first")) state.clearFirstJointBody();
  ImGui::EndDisabled();
  ImGui::TextWrapped("Second body: %s", state.selectedHasBody && !state.selectionLabel.empty()
                                          ? state.selectionLabel.c_str() : "Select an attached body");

  ImGui::BeginDisabled(!state.available);
  int type = static_cast<int>(options.jointType);
  if (ImGui::Combo("Joint type", &type, "Fixed\0Hinge\0Slider\0"))
    options.jointType = static_cast<PhysicsJointType>(type);
  ImGui::Checkbox("Auto anchor between bodies", &options.autoJointAnchor);
  if (!options.autoJointAnchor)
    ImGui::InputScalarN("World anchor", ImGuiDataType_Double, &options.jointAnchor.x, 3, nullptr, nullptr, "%.3f");
  if (options.jointType != PhysicsJointType::Fixed) {
    const bool hinge = options.jointType == PhysicsJointType::Hinge;
    ImGui::InputFloat3("World axis", &options.jointAxis.x, "%.3f");
    ImGui::Checkbox("Joint limits", &options.jointLimitsEnabled);
    if (options.jointLimitsEnabled) {
      if (hinge) {
        ImGui::InputDouble("Lower angle (deg)", &options.hingeLowerDegrees, 0, 0, "%.2f");
        ImGui::InputDouble("Upper angle (deg)", &options.hingeUpperDegrees, 0, 0, "%.2f");
      } else {
        ImGui::InputDouble("Lower distance (m)", &options.sliderLowerMeters, 0, 0, "%.3f");
        ImGui::InputDouble("Upper distance (m)", &options.sliderUpperMeters, 0, 0, "%.3f");
      }
    }
    int motor = static_cast<int>(options.jointMotorMode);
    if (ImGui::Combo("Joint motor", &motor, "Off\0Velocity\0Position\0"))
      options.jointMotorMode = static_cast<PhysicsJointMotorMode>(motor);
    if (options.jointMotorMode != PhysicsJointMotorMode::Off) {
      if (options.jointMotorMode == PhysicsJointMotorMode::Velocity)
        ImGui::InputDouble(hinge ? "Target speed (deg/s)" : "Target speed (m/s)",
            hinge ? &options.hingeTargetVelocityDegrees : &options.sliderTargetVelocity, 0, 0, "%.3f");
      else
        ImGui::InputDouble(hinge ? "Target angle (deg)" : "Target distance (m)",
            hinge ? &options.hingeTargetPositionDegrees : &options.sliderTargetPosition, 0, 0, "%.3f");
      ImGui::InputFloat(hinge ? "Maximum torque (N m)" : "Maximum force (N)",
                         &options.jointMotorMaxForceOrTorque, 0, 0, "%.3f");
      if (options.jointMotorMode == PhysicsJointMotorMode::Position) {
        ImGui::InputFloat("Motor frequency (Hz)", &options.jointMotorFrequencyHz, 0, 0, "%.3f");
        ImGui::InputFloat("Motor damping ratio", &options.jointMotorDampingRatio, 0, 0, "%.3f");
      }
    }
  }
  ImGui::Checkbox("Connected bodies collide", &options.jointCollideConnected);
  physics::PhysicsJointSettings settings;
  const auto geometryError = jointSettings(state, options, settings);
  const auto motorError = jointMotorSettings(options, settings.motor);
  const auto &error = geometryError.empty() ? motorError : geometryError;
  ImGui::BeginDisabled(!state.canConnectJoint || !state.connectJoint || !error.empty());
  if (ImGui::Button("Connect bodies")) state.connectJoint(settings);
  ImGui::EndDisabled();
  if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
  else if (!state.jointConnectionHint.empty()) ImGui::TextWrapped("%s", state.jointConnectionHint.c_str());
  ImGui::EndDisabled();

  ImGui::Text("Joints: %zu", state.joints.size());
  // A callback can replace the frontend's live joint list. This state snapshot
  // remains valid, and each remove control is scoped by the full 64-bit ID.
  for (const auto &joint : state.joints) {
    const auto id = std::to_string(joint.id);
    ImGui::PushID(id.c_str());
    ImGui::BeginDisabled(!state.available || !state.removeJoint);
    if (ImGui::SmallButton("Remove joint")) state.removeJoint(joint.id);
    ImGui::EndDisabled();
    if (joint.type != PhysicsJointType::Fixed) {
      ImGui::SameLine();
      ImGui::BeginDisabled(!state.available || !state.setJointMotor ||
                            joint.type != options.jointType || !motorError.empty());
      if (ImGui::SmallButton("Apply motor")) state.setJointMotor(joint.id, settings.motor);
      ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (joint.type == PhysicsJointType::Hinge)
      ImGui::Text("%s / %s / %.2f deg", joint.label.c_str(), jointName(joint.type), glm::degrees(joint.coordinate));
    else if (joint.type == PhysicsJointType::Slider)
      ImGui::Text("%s / %s / %.3f m", joint.label.c_str(), jointName(joint.type), joint.coordinate);
    else ImGui::Text("%s / %s", joint.label.c_str(), jointName(joint.type));
    ImGui::PopID();
  }
  ImGui::TreePop();
}

} // namespace container::ui
