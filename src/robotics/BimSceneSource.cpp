#include "Container/robotics/BimSceneSourceManifest.h"

#include "Container/robotics/RoboticsScene.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace container::robotics {

BimSceneSource makeBimSceneSource(std::vector<util::ContentFingerprint> layers,
                                  const RoboticsFrame& frame) {
  if (layers.empty() || !frame.metersPerRendererUnit)
    throw std::invalid_argument("BIM scene identity requires source bytes and metric units");
  const auto importScale = frame.importScale.value_or(1.0);
  // The authored scale in metres is constant even when the renderer enlarges
  // the imported geometry and compensates its metres-per-unit conversion.
  const auto metersPerUnscaledUnit = *frame.metersPerRendererUnit * importScale;
  if (!std::isfinite(importScale) || importScale <= 0.0 ||
      !std::isfinite(metersPerUnscaledUnit) || metersPerUnscaledUnit <= 0.0)
    throw std::invalid_argument("BIM scene identity has an invalid physical scale");
  return {std::move(layers), metersPerUnscaledUnit};
}

nlohmann::json bimSceneSourceJson(const BimSceneSource& source) {
  auto layers = nlohmann::json::array();
  for (const auto& layer : source.layers)
    layers.push_back({{"sha256", layer.digest}, {"byteSize", layer.byteSize}});
  return {{"layers", std::move(layers)},
          {"metersPerUnscaledRendererUnit", source.metersPerUnscaledRendererUnit}};
}

} // namespace container::robotics
