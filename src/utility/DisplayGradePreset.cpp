#include "Container/renderer/effects/DisplayGradePreset.h"
#include "Container/utility/AtomicFile.h"
#include "PresetJson.h"

#include <cmath>

namespace container::renderer {
namespace {

using Json = nlohmann::json;
constexpr std::array<std::string_view, 7> kToneMapperNames{
    "aces", "reinhard", "clip", "filmic", "uncharted", "agx", "neutral"};

[[noreturn]] void invalid(std::string_view reason) {
  throw std::invalid_argument("Display grading preset: " + std::string(reason));
}

float readNumber(const Json &value, float min, float max, std::string_view field) {
  if (!value.is_number())
    invalid(std::string(field) + " requires a number");
  const double number = value.get<double>();
  if (!std::isfinite(number) || number < min || number > max)
    invalid(std::string(field) + " is outside its supported range");
  return static_cast<float>(number);
}

std::array<float, 3> readTone(const Json &value, std::string_view field) {
  if (!value.is_array() || value.size() != 3)
    invalid(std::string(field) + " requires three channels");
  return {readNumber(value[0], 0.0f, 1.0f, field),
          readNumber(value[1], 0.0f, 1.0f, field),
          readNumber(value[2], 0.0f, 1.0f, field)};
}

DisplayGrade decodeGrade(const Json &json) {
  if (!json.is_object() || json.size() != 9)
    invalid("expected all nine version 1 fields");
  const auto &version = json.at("version");
  if (!version.is_number_integer() || version != 1)
    invalid("unsupported version");
  return {
      .toneMapper = parseToneMapper(json.at("toneMap").get<std::string>()),
      .saturation = readNumber(json.at("saturation"), 0.0f, 2.0f, "saturation"),
      .contrast = readNumber(json.at("contrast"), 0.5f, 2.0f, "contrast"),
      .temperatureKelvin = readNumber(json.at("temperatureKelvin"), 2000.0f, 15000.0f, "temperatureKelvin"),
      .tintDuv = readNumber(json.at("tintDuv"), -0.025f, 0.025f, "tintDuv"),
      .vibrance = readNumber(json.at("vibrance"), -1.0f, 1.0f, "vibrance"),
      .shadowTone = readTone(json.at("shadowTone"), "shadowTone"),
      .highlightTone = readTone(json.at("highlightTone"), "highlightTone"),
  };
}

} // namespace

ToneMapper parseToneMapper(std::string_view name) {
  const auto found = std::ranges::find(kToneMapperNames, name);
  if (found == kToneMapperNames.end())
    invalid("unknown tone map " + std::string(name));
  return static_cast<ToneMapper>(found - kToneMapperNames.begin());
}

DisplayGrade loadDisplayGrade(const std::filesystem::path &path) {
  try {
    return decodeGrade(container::util::readPresetJson(path, "Display grading preset"));
  } catch (const Json::exception &error) {
    invalid(error.what());
  }
}

void saveDisplayGrade(const std::filesystem::path &path, const DisplayGrade &grade) {
  if (path.empty() || path.filename().empty())
    invalid("save path must name a file");
  const auto mode = static_cast<size_t>(grade.toneMapper);
  if (mode >= kToneMapperNames.size())
    invalid("unknown tone map");
  const Json json{{"version", 1}, {"toneMap", kToneMapperNames[mode]},
                  {"saturation", grade.saturation}, {"contrast", grade.contrast},
                  {"temperatureKelvin", grade.temperatureKelvin}, {"tintDuv", grade.tintDuv},
                  {"vibrance", grade.vibrance}, {"shadowTone", grade.shadowTone},
                  {"highlightTone", grade.highlightTone}};
  (void)decodeGrade(json);
  container::util::writeFileAtomically(path, json.dump(2) + '\n');
}

} // namespace container::renderer
