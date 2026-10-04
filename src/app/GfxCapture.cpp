#include "Container/app/GfxCapture.h"

#include <charconv>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace container::capture {
namespace {
uint32_t number(std::string_view text) {
  uint32_t result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size() || result == 0)
    throw std::invalid_argument("GFXReconstruct frames must be positive uint32 integers");
  return result;
}
void saveJson(const std::filesystem::path& path, const nlohmann::json& json) {
  std::ofstream stream(path);
  stream << json.dump(2) << '\n';
  if (!stream) throw std::runtime_error("Cannot write GFXReconstruct metadata: " + path.string());
}
} // namespace

uint32_t validateFrameRanges(const std::string& value) {
  if (value.empty()) throw std::invalid_argument("GFXReconstruct frame range is empty");
  uint32_t previous = 0;
  size_t offset = 0;
  do {
    const size_t comma = value.find(',', offset);
    const auto range = std::string_view(value).substr(offset, comma == std::string::npos ? comma : comma - offset);
    const auto dash = range.find('-');
    const auto first = number(range.substr(0, dash));
    const auto last = dash == std::string_view::npos ? first : number(range.substr(dash + 1));
    if (first <= previous || last < first)
      throw std::invalid_argument("GFXReconstruct frame ranges must ascend without overlap");
    previous = last;
    if (comma == std::string::npos) break;
    offset = comma + 1;
  } while (true);
  return previous;
}

Config loadSession(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream) throw std::runtime_error("Cannot open GFXReconstruct session: " + path.string());
  Config config;
  config.session = nlohmann::json::parse(stream);
  const auto& json = config.session;
  if (json.at("schemaVersion") != 1)
    throw std::invalid_argument("Unsupported GFXReconstruct session schema");
  config.sessionPath = std::filesystem::absolute(path);
  config.outputDirectory = json.at("outputDirectory").get<std::string>();
  if (!config.outputDirectory.is_absolute() || !std::filesystem::is_directory(config.outputDirectory))
    throw std::invalid_argument("GFXReconstruct output directory must exist and be absolute");
  config.mode = json.at("mode").get<std::string>();
  config.frames = json.at("frames").get<std::string>();
  config.trigger = json.at("trigger").get<std::string>();
  config.toolVersion = json.at("toolVersion").get<std::string>();
  config.stopAfterPresent = json.at("stopAfterPresent").get<uint64_t>();
  if (config.mode == "frames") {
    if (!config.trigger.empty() || config.stopAfterPresent != validateFrameRanges(config.frames))
      throw std::invalid_argument("Inconsistent GFXReconstruct bounded capture session");
  } else if (config.mode == "hotkey") {
    if (!config.frames.empty() || config.trigger.empty() || config.stopAfterPresent != 0)
      throw std::invalid_argument("Inconsistent GFXReconstruct hotkey session");
    if (config.trigger == "F6" || config.trigger == "F7" || config.trigger == "F8")
      throw std::invalid_argument("Capture hotkey conflicts with renderer debug controls; launch with --trigger F3");
#if defined(_WIN32)
    if (config.trigger == "F12")
      throw std::invalid_argument("F12 is reserved by the Windows debugger; create a new capture session with --trigger F3");
#endif
  } else if (config.mode != "all" || !config.frames.empty() || !config.trigger.empty() || config.stopAfterPresent != 0) {
    throw std::invalid_argument("Invalid GFXReconstruct capture mode");
  }
  // Prevent sidecars that describe a different configuration from the layer.
  for (const auto& [key, value] : json.at("environment").items()) {
    if (!key.starts_with("GFXRECON_")) throw std::invalid_argument("Invalid capture environment key");
    const char* actual = std::getenv(key.c_str());
    if (std::string(actual ? actual : "") != value.get<std::string>())
      throw std::invalid_argument("GFXReconstruct environment mismatch for " + key + "; use tools/gfxreconstruct.ps1 capture");
  }
  if (std::getenv("GFXRECON_DISABLE"))
    throw std::invalid_argument("GFXRECON_DISABLE prevents the requested capture layer from loading");
  const auto& environment = json.at("environment");
  const std::filesystem::path captureFile = json.value(
      "captureFile", (config.outputDirectory / "capture.gfxr").string());
  if (!captureFile.is_absolute() || captureFile.parent_path() != config.outputDirectory ||
      captureFile.extension() != ".gfxr")
    throw std::invalid_argument("Capture file must be a .gfxr file inside the session output directory");
  if (environment.at("GFXRECON_CAPTURE_FRAMES") != config.frames ||
      environment.at("GFXRECON_CAPTURE_TRIGGER") != config.trigger ||
      environment.at("GFXRECON_CAPTURE_FILE") != captureFile.string())
    throw std::invalid_argument("Capture settings do not match session metadata");
  return config;
}

void Journal::open(const Config& config, nlohmann::json runtime) {
  if (!config.enabled()) return;
  if (std::filesystem::exists(config.outputDirectory / "runtime.json") ||
      std::filesystem::exists(config.outputDirectory / "frames.jsonl"))
    throw std::invalid_argument("Capture metadata already exists; launch into a fresh output directory");
  runtime["captureSession"] = config.sessionPath.string();
  runtime["frameJournal"] = "frames.jsonl";
  runtime["frameNumbering"] = "presentCall is 1-based vkQueuePresentKHR call order, including failed calls; successfulSubmissions counts rendered vkQueueSubmit2 calls; tick may skip or retry. Replay frame 1 begins the selected trim, not renderer tick 1.";
  saveJson(config.outputDirectory / "runtime.json", runtime);
  stream_.open(config.outputDirectory / "frames.jsonl");
  if (!stream_) throw std::runtime_error("Cannot create GFXReconstruct frame journal");
}
void Journal::write(nlohmann::json value) {
  if (!enabled()) return;
  value["tick"] = tick_;
  value["successfulSubmissions"] = submissions_;
  value["presentCalls"] = presentCalls_;
  stream_ << value.dump() << '\n';
  stream_.flush(); // Retain the last completed boundary when investigating a crash.
  if (!stream_) throw std::runtime_error("GFXReconstruct frame journal write failed");
}
void Journal::tick(uint64_t tick, bool skipped) {
  tick_ = tick;
  write({{"type", "tick"}, {"skipped", skipped}});
}
void Journal::acquireFailed(int32_t result) {
  write({{"type", "acquireFailed"}, {"result", result}});
}
void Journal::submitted() {
  if (!enabled()) return;
  ++submissions_;
  write({{"type", "submit"}});
}
void Journal::presented(int32_t actualResult, int32_t effectiveResult,
                        uint32_t image, uint32_t slot, nlohmann::json telemetry) {
  if (!enabled()) return;
  ++presentCalls_;
  write({{"type", "present"}, {"actualResult", actualResult},
         {"effectiveResult", effectiveResult}, {"imageIndex", image},
         {"frameSlot", slot}, {"telemetry", std::move(telemetry)}});
}
} // namespace container::capture
