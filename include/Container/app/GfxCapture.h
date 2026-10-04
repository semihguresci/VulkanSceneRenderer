#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace container::capture {

inline constexpr const char* kLayerName = "VK_LAYER_LUNARG_gfxreconstruct";

// The launcher configures the child before the loader is used. A session file
// makes that opt-in explicit and keeps external tools out of ordinary startup.
struct Config {
  std::filesystem::path sessionPath, outputDirectory;
  std::string mode, frames, trigger, toolVersion;
  uint64_t stopAfterPresent{0};
  nlohmann::json session;
  [[nodiscard]] bool enabled() const { return !sessionPath.empty(); }
};

uint32_t validateFrameRanges(const std::string& value);
Config loadSession(const std::filesystem::path& path);

// CPU journal counters are deliberately independent of swapchain image indices
// and the temporal history index. Present calls include failed/suboptimal calls.
class Journal {
public:
  void open(const Config& config, nlohmann::json runtime);
  void tick(uint64_t tick, bool skipped);
  void acquireFailed(int32_t result);
  void submitted();
  void presented(int32_t actualResult, int32_t effectiveResult,
                 uint32_t image, uint32_t slot, nlohmann::json telemetry);
  [[nodiscard]] uint64_t presentCalls() const { return presentCalls_; }
  [[nodiscard]] bool enabled() const { return stream_.is_open(); }
private:
  void write(nlohmann::json value);
  std::ofstream stream_;
  uint64_t tick_{0}, submissions_{0}, presentCalls_{0};
};

} // namespace container::capture
