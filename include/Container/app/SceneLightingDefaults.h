#pragma once

#include <cstdint>

namespace container::app {

struct SceneLightingValues {
  float directionalIntensity{2.0f};
  float environmentIntensity{1.0f};
  float bounceIntensity{1.0f};
  bool bloomEnabled{true};
  uint32_t localShadowLayerBudget{8};
};

struct SceneLightingOverrides {
  bool directionalIntensity{false};
  bool environmentIntensity{false};
  bool bloomEnabled{false};
};

// Scene presets own only values that the user has left at their automatic
// defaults. Once a field is edited, subsequent scene loads preserve that field.
class SceneLightingDefaults {
public:
  [[nodiscard]] SceneLightingValues apply(
      SceneLightingValues current, const SceneLightingValues &defaults,
      const SceneLightingOverrides &overrides = {}) {
    update(current.directionalIntensity, defaults.directionalIntensity,
           previous_.directionalIntensity, automatic_.directionalIntensity,
           overrides.directionalIntensity);
    update(current.environmentIntensity, defaults.environmentIntensity,
           previous_.environmentIntensity, automatic_.environmentIntensity,
           overrides.environmentIntensity);
    update(current.bounceIntensity, defaults.bounceIntensity,
           previous_.bounceIntensity, automaticBounce_, false);
    update(current.bloomEnabled, defaults.bloomEnabled, previous_.bloomEnabled,
           automatic_.bloomEnabled, overrides.bloomEnabled);
    update(current.localShadowLayerBudget, defaults.localShadowLayerBudget,
           previous_.localShadowLayerBudget, automaticShadowBudget_, false);
    previous_ = current;
    initialized_ = true;
    return current;
  }

private:
  template <class T>
  void update(T &current, T desired, T previous, bool &automatic, bool locked) {
    if (locked || (initialized_ && current != previous))
      automatic = false;
    if (automatic)
      current = desired;
  }

  bool initialized_{false};
  SceneLightingValues previous_{};
  SceneLightingOverrides automatic_{true, true, true};
  bool automaticBounce_{true};
  bool automaticShadowBudget_{true};
};

} // namespace container::app
