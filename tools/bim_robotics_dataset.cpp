#include "Container/robotics/RoboticsDataset.h"
#include "Container/utility/Platform.h"

#include <charconv>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

constexpr std::string_view usage =
    "Usage: BimRoboticsDataset record CAMERA_DIRECTORY SIMULATION.json NEW_DIRECTORY\n"
    "                                 [--ticks-per-chunk N]\n"
    "       BimRoboticsDataset verify DATASET_DIRECTORY\n"
    "       BimRoboticsDataset replay DATASET_DIRECTORY\n";

template <class Char>
int run(int count, Char** args) {
  namespace fs = std::filesystem;
  namespace robotics = container::robotics;
  const auto operation = count > 1 ? fs::path(args[1]) : fs::path{};
  if (count == 2 && operation == "--help") {
    std::cout << usage;
    return 0;
  }

  try {
    if (operation == "record" && (count == 5 || (count == 7 && fs::path(args[5]) == "--ticks-per-chunk"))) {
      robotics::RoboticsDatasetWriteOptions options;
      if (count == 7) {
        const auto value = container::util::pathToUtf8(fs::path(args[6]));
        const auto* end = value.data() + value.size();
        const auto parsed = std::from_chars(value.data(), end, options.ticksPerChunk);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
          throw std::invalid_argument("--ticks-per-chunk requires an integer in [1, 1024]");
      }
      robotics::writeRoboticsDataset(fs::path(args[2]), fs::path(args[3]), fs::path(args[4]), options);
      std::cout << "Recorded dataset at " << container::util::pathToUtf8(fs::path(args[4])) << '\n';
      return 0;
    }
    if (count == 3 && operation == "verify") {
      const auto dataset = robotics::readRoboticsDataset(fs::path(args[2]));
      std::cout << dataset.manifest.dump(2) << '\n';
      return 0;
    }
    if (count == 3 && operation == "replay") {
      (void)robotics::readRoboticsDataset(fs::path(args[2]), [](const nlohmann::json& row) {
        if (!(std::cout << row.dump() << '\n')) throw std::runtime_error("could not write replay output");
      });
      return 0;
    }
  } catch (const std::exception& error) {
    std::cerr << "BimRoboticsDataset: " << error.what() << '\n';
    return 1;
  }
  std::cerr << usage;
  return 2;
}

} // namespace

#ifdef _WIN32
int wmain(int count, wchar_t** args) { return run(count, args); }
#else
int main(int count, char** args) { return run(count, args); }
#endif
