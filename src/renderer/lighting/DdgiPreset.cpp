#include "Container/renderer/lighting/DdgiPreset.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "Container/utility/AtomicFile.h"
#include "Container/utility/Platform.h"

namespace container::renderer {
namespace {
using Json = nlohmann::json;

[[noreturn]] void invalid(std::string_view message) {
  throw std::invalid_argument("DDGI preset: " + std::string(message));
}

void validateIdentity(std::string_view identity) {
  if (identity.empty() || identity.size() > kMaxDdgiSceneIdentityBytes ||
      identity.find('\0') != std::string_view::npos)
    invalid("scene identity must contain 1..4096 bytes without NUL");
}

void objectFields(const Json &value,
                  std::initializer_list<std::string_view> fields) {
  if (!value.is_object() || value.size() != fields.size())
    invalid("missing or unknown object field");
  for (const auto field : fields)
    if (!value.contains(std::string(field)))
      invalid("missing field " + std::string(field));
}

uint64_t unsignedInteger(const Json &value, uint64_t maximum,
                         std::string_view field) {
  if (!value.is_number_integer() ||
      (value.is_number_integer() && !value.is_number_unsigned() &&
       value.get<int64_t>() < 0))
    invalid(std::string(field) + " requires an unsigned integer");
  const auto result = value.get<uint64_t>();
  if (result > maximum)
    invalid(std::string(field) + " exceeds its integer range");
  return result;
}

uint32_t uint32(const Json &value, std::string_view field) {
  return static_cast<uint32_t>(
      unsignedInteger(value, std::numeric_limits<uint32_t>::max(), field));
}

float finiteFloat(const Json &value, std::string_view field) {
  if (!value.is_number())
    invalid(std::string(field) + " requires a number");
  const double result = value.get<double>();
  if (!std::isfinite(result) ||
      std::abs(result) > std::numeric_limits<float>::max())
    invalid(std::string(field) + " requires a finite float");
  return static_cast<float>(result);
}

bool boolean(const Json &value, std::string_view field) {
  if (!value.is_boolean())
    invalid(std::string(field) + " requires a boolean");
  return value.get<bool>();
}

Json volumeJson(const DdgiProbeVolumeDesc &volume) {
  return {{"origin", {volume.origin.x, volume.origin.y, volume.origin.z}},
          {"spacing", {volume.spacing.x, volume.spacing.y, volume.spacing.z}},
          {"counts", {volume.counts.x, volume.counts.y, volume.counts.z}}};
}

DdgiProbeVolumeDesc readVolume(const Json &value) {
  objectFields(value, {"origin", "spacing", "counts"});
  DdgiProbeVolumeDesc result;
  for (const auto field : {"origin", "spacing", "counts"}) {
    const auto &vector = value.at(field);
    if (!vector.is_array() || vector.size() != 3)
      invalid(std::string(field) + " requires exactly three components");
    for (int axis = 0; axis < 3; ++axis)
      if (std::string_view(field) == "counts")
        result.counts[axis] = uint32(vector[axis], field);
      else if (std::string_view(field) == "origin")
        result.origin[axis] = finiteFloat(vector[axis], field);
      else
        result.spacing[axis] = finiteFloat(vector[axis], field);
  }
  return result;
}

Json settingsJson(const DdgiSettings &settings) {
  Json volumes = Json::array();
  for (const auto &volume : settings.additionalVolumes)
    volumes.push_back(volumeJson(volume));
  return {
      {"enabled", settings.enabled},
      {"paused", settings.paused},
      {"volume", volumeJson(settings.volume)},
      {"additionalVolumes", std::move(volumes)},
      {"volumeBlendCells", settings.volumeBlendCells},
      {"thinGlassTransmission", settings.thinGlassTransmission},
      {"dielectricTransmission", settings.dielectricTransmission},
      {"cameraFollow", settings.cameraFollow},
      {"asyncCompute", settings.asyncCompute},
      {"resources",
       {{"irradianceInteriorTexels",
         settings.resources.irradianceInteriorTexels},
        {"visibilityInteriorTexels",
         settings.resources.visibilityInteriorTexels},
        {"updates",
         {{"maxProbesPerFrame", settings.resources.updates.maxProbesPerFrame},
          {"raysPerProbe", settings.resources.updates.raysPerProbe},
          {"maxRaysPerFrame", settings.resources.updates.maxRaysPerFrame}}}}},
      {"maxDistance", settings.maxDistance},
      {"hysteresis", settings.hysteresis},
      {"normalBias", settings.normalBias},
      {"viewBias", settings.viewBias},
      {"visibilityBias", settings.visibilityBias},
      {"intensity", settings.intensity},
      {"debugView", static_cast<uint32_t>(settings.debugView)},
      {"debugProbeRadius", settings.debugProbeRadius},
      {"payloadBudgetBytes", settings.payloadBudgetBytes},
      {"surfaceBudgetBytes", settings.surfaceBudgetBytes}};
}

DdgiSettings readSettings(const Json &value, uint32_t version) {
  // Version 1 remains readable with its opt-in extensions disabled. Validate
  // the legacy shape separately so migration cannot silently ignore fields.
  Json legacy = value;
  if (version == 2) {
    for (const auto field : {"dielectricTransmission", "cameraFollow", "asyncCompute"}) {
      if (!legacy.contains(field)) invalid("missing field " + std::string(field));
      legacy.erase(field);
    }
  }
  objectFields(legacy,
               {"enabled", "paused", "volume", "additionalVolumes",
                "volumeBlendCells", "thinGlassTransmission", "resources",
                "maxDistance", "hysteresis", "normalBias", "viewBias",
                "visibilityBias", "intensity", "debugView", "debugProbeRadius",
                "payloadBudgetBytes", "surfaceBudgetBytes"});
  DdgiSettings result;
  result.enabled = boolean(value.at("enabled"), "enabled");
  result.paused = boolean(value.at("paused"), "paused");
  result.volume = readVolume(value.at("volume"));
  const auto &volumes = value.at("additionalVolumes");
  if (!volumes.is_array() || volumes.size() > 3)
    invalid("additionalVolumes requires at most three volumes");
  for (const auto &volume : volumes)
    result.additionalVolumes.push_back(readVolume(volume));
  result.volumeBlendCells =
      finiteFloat(value.at("volumeBlendCells"), "volumeBlendCells");
  result.thinGlassTransmission =
      boolean(value.at("thinGlassTransmission"), "thinGlassTransmission");
  if (version == 2) {
    result.dielectricTransmission = boolean(value.at("dielectricTransmission"), "dielectricTransmission");
    result.cameraFollow = boolean(value.at("cameraFollow"), "cameraFollow");
    result.asyncCompute = boolean(value.at("asyncCompute"), "asyncCompute");
  }
  const auto &resources = value.at("resources");
  objectFields(resources, {"irradianceInteriorTexels",
                           "visibilityInteriorTexels", "updates"});
  result.resources.irradianceInteriorTexels = uint32(
      resources.at("irradianceInteriorTexels"), "irradianceInteriorTexels");
  result.resources.visibilityInteriorTexels = uint32(
      resources.at("visibilityInteriorTexels"), "visibilityInteriorTexels");
  const auto &updates = resources.at("updates");
  objectFields(updates,
               {"maxProbesPerFrame", "raysPerProbe", "maxRaysPerFrame"});
  result.resources.updates.maxProbesPerFrame =
      uint32(updates.at("maxProbesPerFrame"), "maxProbesPerFrame");
  result.resources.updates.raysPerProbe =
      uint32(updates.at("raysPerProbe"), "raysPerProbe");
  result.resources.updates.maxRaysPerFrame =
      uint32(updates.at("maxRaysPerFrame"), "maxRaysPerFrame");
  result.maxDistance = finiteFloat(value.at("maxDistance"), "maxDistance");
  result.hysteresis = finiteFloat(value.at("hysteresis"), "hysteresis");
  result.normalBias = finiteFloat(value.at("normalBias"), "normalBias");
  result.viewBias = finiteFloat(value.at("viewBias"), "viewBias");
  result.visibilityBias =
      finiteFloat(value.at("visibilityBias"), "visibilityBias");
  result.intensity = finiteFloat(value.at("intensity"), "intensity");
  result.debugView =
      static_cast<DdgiDebugView>(uint32(value.at("debugView"), "debugView"));
  result.debugProbeRadius =
      finiteFloat(value.at("debugProbeRadius"), "debugProbeRadius");
  result.payloadBudgetBytes = unsignedInteger(
      value.at("payloadBudgetBytes"), std::numeric_limits<uint64_t>::max(),
      "payloadBudgetBytes");
  result.surfaceBudgetBytes = unsignedInteger(
      value.at("surfaceBudgetBytes"), std::numeric_limits<uint64_t>::max(),
      "surfaceBudgetBytes");
  validateDdgiSettings(result);
  return result;
}

// Bound nesting before the recursive JSON parser sees untrusted input. Braces
// inside strings and escaped quotes have no effect on the depth limit.
void boundedJsonNesting(std::string_view input) {
  size_t depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (const char ch : input) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (ch == '\\')
        escaped = true;
      else if (ch == '"')
        quoted = false;
    } else if (ch == '"') {
      quoted = true;
    } else if (ch == '{' || ch == '[') {
      if (++depth > 16)
        invalid("JSON nesting exceeds 16 levels");
    } else if (ch == '}' || ch == ']') {
      if (!depth)
        invalid("unbalanced JSON nesting");
      --depth;
    }
  }
}

std::string normalizedPath(const std::filesystem::path &path) {
  if (path.empty())
    return {};
  const auto rawBytes = path.generic_u8string();
  const std::string raw(reinterpret_cast<const char *>(rawBytes.data()),
                        rawBytes.size());
  if (raw.starts_with("__") && raw.ends_with("__"))
    return raw;
  auto asset = path;
  std::error_code existsError;
  if (asset.is_relative() && !std::filesystem::exists(asset, existsError)) {
    const auto besideExecutable =
        container::util::executableDirectory() / asset;
    existsError.clear();
    if (std::filesystem::exists(besideExecutable, existsError))
      asset = besideExecutable;
  }
  const auto absolute = std::filesystem::absolute(asset).lexically_normal();
  std::error_code error;
  auto resolved = std::filesystem::weakly_canonical(absolute, error);
  if (error)
    resolved = absolute;
  const auto utf8 = resolved.generic_u8string();
  std::string result(reinterpret_cast<const char *>(utf8.data()), utf8.size());
#ifdef _WIN32
  for (char &ch : result)
    if (ch >= 'A' && ch <= 'Z')
      ch = static_cast<char>(ch + ('a' - 'A'));
#endif
  return result;
}

std::string scaleText(float scale) {
  if (!std::isfinite(scale) || scale <= 0.f)
    invalid("scene import scales must be finite and positive");
  std::array<char, 64> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), scale,
                    std::chars_format::hex);
  if (error != std::errc{})
    invalid("failed to encode scene scale");
  return {buffer.data(), end};
}

} // namespace

std::string encodeDdgiPreset(const DdgiPreset &preset) {
  validateIdentity(preset.sceneIdentity);
  validateDdgiSettings(preset.settings);
  try {
    const Json json{{"version", 2},
                    {"sceneIdentity", preset.sceneIdentity},
                    {"settings", settingsJson(preset.settings)}};
    auto result = json.dump(2) + '\n';
    if (result.size() > kMaxDdgiPresetBytes)
      invalid("encoded JSON exceeds 64 KiB");
    return result;
  } catch (const Json::exception &error) {
    invalid(error.what());
  }
}

DdgiPreset decodeDdgiPreset(std::string_view input) {
  if (input.empty() || input.size() > kMaxDdgiPresetBytes)
    invalid("JSON must contain 1..65536 bytes");
  boundedJsonNesting(input);
  try {
    std::vector<std::set<std::string>> objectKeys;
    const auto rejectDuplicates = [&objectKeys](int, Json::parse_event_t event,
                                                Json &parsed) {
      if (event == Json::parse_event_t::object_start)
        objectKeys.emplace_back();
      else if (event == Json::parse_event_t::key) {
        if (objectKeys.empty() ||
            !objectKeys.back().insert(parsed.get<std::string>()).second)
          invalid("duplicate object key");
      } else if (event == Json::parse_event_t::object_end) {
        if (!objectKeys.empty())
          objectKeys.pop_back();
      }
      return true;
    };
    const auto json = Json::parse(input.begin(), input.end(), rejectDuplicates);
    objectFields(json, {"version", "sceneIdentity", "settings"});
    const auto version = uint32(json.at("version"), "version");
    if (version != 1 && version != 2)
      invalid("unsupported version");
    if (!json.at("sceneIdentity").is_string())
      invalid("sceneIdentity requires a string");
    DdgiPreset result;
    result.sceneIdentity = json.at("sceneIdentity").get<std::string>();
    validateIdentity(result.sceneIdentity);
    result.settings = readSettings(json.at("settings"), version);
    return result;
  } catch (const Json::exception &error) {
    invalid(error.what());
  }
}

DdgiPreset loadDdgiPreset(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    throw std::runtime_error("DDGI preset: cannot open the requested file");
  const auto size = file.tellg();
  if (size <= 0 || size > static_cast<std::streamoff>(kMaxDdgiPresetBytes))
    invalid("file must contain 1..65536 bytes");
  std::string contents(static_cast<size_t>(size), '\0');
  file.seekg(0);
  file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!file || file.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("DDGI preset: file changed or could not be read");
  return decodeDdgiPreset(contents);
}

void saveDdgiPreset(const std::filesystem::path &path,
                    const DdgiPreset &preset) {
  const auto contents = encodeDdgiPreset(preset);
  if (path.empty() || path.filename().empty())
    invalid("save path must name a file");
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  container::util::writeFileAtomically(path, contents);
}

std::string ddgiSceneIdentity(const std::filesystem::path &primaryPath,
                              const std::filesystem::path &auxiliaryPath,
                              float primaryScale, float auxiliaryScale) {
  if (primaryPath.empty() && auxiliaryPath.empty())
    invalid("scene identity requires at least one model");
  const Json identity{
      {"primary", normalizedPath(primaryPath)},
      {"auxiliary", normalizedPath(auxiliaryPath)},
      {"primaryScale", primaryPath.empty() ? "" : scaleText(primaryScale)},
      {"auxiliaryScale",
       auxiliaryPath.empty() ? "" : scaleText(auxiliaryScale)}};
  auto result = identity.dump();
  validateIdentity(result);
  return result;
}

std::filesystem::path ddgiDefaultPresetDirectory() {
#ifdef _WIN32
  const auto environmentPath = [](const wchar_t *name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length || length > 32768)
      return std::filesystem::path{};
    std::wstring value(length, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), length);
    if (!copied || copied >= length)
      return std::filesystem::path{};
    value.resize(copied);
    return std::filesystem::path(value);
  };
  auto directory = environmentPath(L"LOCALAPPDATA");
  if (directory.empty()) {
    directory = environmentPath(L"USERPROFILE");
    if (!directory.empty())
      directory /= "AppData/Local";
  }
#else
  std::filesystem::path directory;
  if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    directory = xdg;
  if (!directory.is_absolute()) {
    directory.clear();
    if (const char *home = std::getenv("HOME"); home && *home)
      directory = std::filesystem::path(home) / ".config";
  }
#endif
  if (!directory.is_absolute())
    throw std::runtime_error(
        "DDGI preset: no absolute user configuration directory is available");
  return directory.lexically_normal() / "VulkanSceneRenderer" / "ddgi";
}

std::filesystem::path
ddgiScenePresetPath(std::string_view sceneIdentity,
                    const std::filesystem::path &storageDirectory) {
  validateIdentity(sceneIdentity);
  if (storageDirectory.empty())
    invalid("preset storage directory must not be empty");
  // FNV-1a is deterministic across processes and platforms. Identity is also
  // checked when loading, so a hash collision never applies another scene.
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char byte : sceneIdentity) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  std::array<char, 16> digits{};
  digits.fill('0');
  std::array<char, 16> encoded{};
  const auto [end, error] =
      std::to_chars(encoded.data(), encoded.data() + encoded.size(), hash, 16);
  if (error != std::errc{})
    invalid("failed to encode preset identity hash");
  const size_t count = static_cast<size_t>(end - encoded.data());
  std::copy_n(encoded.data(), count, digits.data() + digits.size() - count);
  return storageDirectory /
         ("scene-" + std::string(digits.data(), digits.size()) + ".json");
}

} // namespace container::renderer
