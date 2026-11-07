#include "Container/renderer/lighting/SkyPreset.h"
#include "Container/utility/AtomicFile.h"
#include "PresetJson.h"

#include <cmath>
#include <limits>
#include <utility>

namespace container::renderer {
namespace {

using Json = nlohmann::json;
constexpr std::array<std::string_view, 3> kModes{"plain", "gradient", "atmosphere"};
using AirField = std::pair<const char*, float AtmosphereSettings::*>;
constexpr std::array kAirFields{
    AirField{"sunAzimuth", &AtmosphereSettings::sunAzimuthDegrees},
    AirField{"sunElevation", &AtmosphereSettings::sunElevationDegrees},
    AirField{"solarIrradiance", &AtmosphereSettings::solarIrradiance},
    AirField{"planetRadiusKm", &AtmosphereSettings::planetRadiusKm},
    AirField{"atmosphereHeightKm", &AtmosphereSettings::atmosphereHeightKm},
    AirField{"observerHeightKm", &AtmosphereSettings::observerHeightKm},
    AirField{"rayleigh", &AtmosphereSettings::rayleighScale},
    AirField{"mie", &AtmosphereSettings::mieScale},
    AirField{"ozone", &AtmosphereSettings::ozoneScale},
};
using ClockField = std::pair<const char*, int SolarTime::*>;
constexpr std::array kClockFields{
    ClockField{"year", &SolarTime::year}, ClockField{"month", &SolarTime::month},
    ClockField{"day", &SolarTime::day}, ClockField{"hour", &SolarTime::hour},
    ClockField{"minute", &SolarTime::minute},
    ClockField{"utcOffsetMinutes", &SolarTime::utcOffsetMinutes},
};

[[noreturn]] void invalid(std::string_view reason) {
  throw std::invalid_argument("Sky preset: " + std::string(reason));
}

float readFloat(const Json& value) {
  if (!value.is_number())
    invalid("expected a number");
  const auto number = value.get<double>();
  if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
    invalid("number is outside the supported range");
  return static_cast<float>(number);
}

std::array<float, 3> readColor(const Json& value) {
  if (!value.is_array() || value.size() != 3)
    invalid("sky colors require three channels");
  const std::array color{readFloat(value[0]), readFloat(value[1]), readFloat(value[2])};
  if (!std::ranges::all_of(color, [](float c) { return c >= 0.0f && c <= 1.0f; }))
    invalid("sky color channels must be in [0,1]");
  return color;
}

SkySettings decode(const Json& value) {
  constexpr size_t fixedFields = 9; // Version, mode, colors, solar link, location switch and coordinates.
  if (!value.is_object() || !value.at("version").is_number_integer() ||
      value.at("version").get<double>() < 1 || value.at("version").get<double>() > 4)
    invalid("unsupported version");
  const auto version = value.at("version").get<int>();
  const bool hasAerialScale = version >= 2;
  const bool hasGradientShape = version >= 3;
  const bool hasMultipleScattering = version >= 4;
  if (value.size() != fixedFields + kAirFields.size() + kClockFields.size() +
                      (hasAerialScale ? 1 : 0) + (hasGradientShape ? 3 : 0) + (hasMultipleScattering ? 1 : 0))
    invalid("expected all fields for the preset version");
  SkySettings sky;
  sky.mode = parseProceduralSkyMode(value.at("mode").get<std::string>());
  sky.horizon = readColor(value.at("horizon"));
  sky.zenith = readColor(value.at("zenith"));
  sky.ground = readColor(value.at("ground"));
  if (hasGradientShape) {
    sky.zenithCurve = readFloat(value.at("zenithCurve"));
    sky.groundCurve = readFloat(value.at("groundCurve"));
    const auto& up = value.at("gradientUp");
    if (!up.is_array() || up.size() != 3)
      invalid("gradient up requires three components");
    sky.gradientUp = {readFloat(up[0]), readFloat(up[1]), readFloat(up[2])};
  }
  auto& air = sky.atmosphere;
  for (const auto& [field, member] : kAirFields)
    air.*member = readFloat(value.at(field));
  if (hasAerialScale)
    air.aerialMetersPerUnit = readFloat(value.at("aerialMetersPerUnit"));
  if (hasMultipleScattering)
    air.multipleScattering = value.at("multipleScattering").get<bool>();
  air.lightScene = value.at("lightScene").get<bool>();
  auto& local = air.localSun;
  local.enabled = value.at("useLocation").get<bool>();
  local.latitudeDegrees = readFloat(value.at("latitude"));
  local.longitudeDegrees = readFloat(value.at("longitude"));
  for (const auto& [field, member] : kClockFields) {
    const auto& item = value.at(field);
    if (!item.is_number_integer() || item.get<double>() < std::numeric_limits<int>::min() ||
        item.get<double>() > std::numeric_limits<int>::max())
      invalid("invalid integer field " + std::string(field));
    local.*member = item.get<int>();
  }
  if (!validAtmosphere(air) || !validSolarTime(local) || !validGradient(sky))
    invalid("invalid gradient, atmosphere or solar location settings");
  return sky;
}

} // namespace

SkyMode parseProceduralSkyMode(std::string_view name) {
  const auto mode = std::ranges::find(kModes, name);
  if (mode == kModes.end())
    invalid("expected plain, gradient, or atmosphere");
  return static_cast<SkyMode>(1 + (mode - kModes.begin()));
}

SkySettings loadSkyPreset(const std::filesystem::path& path) {
  try {
    return decode(container::util::readPresetJson(path, "Sky preset"));
  } catch (const Json::exception& error) {
    invalid(error.what());
  }
}

void saveSkyPreset(const std::filesystem::path& path, const SkySettings& sky) {
  if (path.empty() || path.filename().empty())
    invalid("save path must name a file");
  const auto mode = static_cast<size_t>(sky.mode);
  if (!mode || mode > kModes.size())
    invalid("select a procedural sky mode");
  const auto& air = sky.atmosphere;
  const auto& local = air.localSun;
  Json value{{"version", 4}, {"mode", kModes[mode - 1]},
             {"horizon", sky.horizon}, {"zenith", sky.zenith}, {"ground", sky.ground},
             {"lightScene", air.lightScene}, {"useLocation", local.enabled},
             {"latitude", local.latitudeDegrees}, {"longitude", local.longitudeDegrees},
             {"aerialMetersPerUnit", air.aerialMetersPerUnit},
             {"multipleScattering", air.multipleScattering},
             {"zenithCurve", sky.zenithCurve}, {"groundCurve", sky.groundCurve},
             {"gradientUp", sky.gradientUp}};
  for (const auto& [field, member] : kAirFields)
    value[field] = air.*member;
  for (const auto& [field, member] : kClockFields)
    value[field] = local.*member;
  (void)decode(value);
  container::util::writeFileAtomically(path, value.dump(2) + '\n');
}

} // namespace container::renderer
