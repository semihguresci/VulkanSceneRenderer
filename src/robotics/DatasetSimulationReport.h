#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>

namespace container::robotics {

enum class DatasetReportStream { Base, Imu, Lidar };

struct DatasetSimulationReport {
  std::string sourceNamespace;
  std::string robotId;
  uint32_t ticksPerSecond{};
  uint64_t tickCount{};
  uint64_t imuSampleCount{};
  uint64_t lidarScanCount{};
  nlohmann::json sceneSource;
  nlohmann::json imuCalibration;
  nlohmann::json lidarCalibration;
};

using DatasetReportObserver = std::function<void(DatasetReportStream, const nlohmann::json&)>;

// Emits each complete observation in input order. The callback's JSON is valid
// for that call. A late error can follow earlier callbacks, so consumers commit
// staged output only after the scan and their own validation succeed.
DatasetSimulationReport scanDatasetSimulationReport(const std::filesystem::path& path,
                                                     const DatasetReportObserver& observe);
DatasetSimulationReport scanDatasetSimulationReport(std::istream& input,
                                                     const DatasetReportObserver& observe);

// Camera playback accepts the minimal version-1 report and discards non-base
// streams. The returned frequency is available after the last callback.
uint32_t scanSimulationBaseHistory(std::istream& input,
                                   const std::function<void(const nlohmann::json&)>& observe);

} // namespace container::robotics
