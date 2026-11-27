#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/robotics/RoboticsOpenUsd.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
namespace robotics = container::robotics;

constexpr std::string_view usage =
    "Usage: BimRoboticsUsd INPUT.ifc|INPUT.ifcx OUTPUT.usda SOURCE_NAMESPACE "
    "[--import-scale N] [--metres-per-renderer-unit N]\n";

std::string utf8(const fs::path& argument) {
  const auto text = argument.u8string();
  return {text.begin(), text.end()};
}

template <class Number>
Number positiveNumber(const fs::path& value, std::string_view option) {
  const auto text = utf8(value);
  Number number{};
  const auto end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, number);
  if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(number) || number <= 0)
    throw std::invalid_argument(std::string(option) + " requires a finite positive number");
  return number;
}

template <class Char>
int run(int count, Char** args) {
  if (count == 2 && fs::path(args[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  if (count < 4 || count > 8 || count % 2 != 0) {
    std::cerr << usage;
    return 2;
  }

  try {
    const fs::path input(args[1]), output(args[2]);
    auto extension = utf8(input.extension());
    std::ranges::transform(extension, extension.begin(), [](char ch) {
      return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    });
    if (extension != ".ifc" && extension != ".ifcx")
      throw std::invalid_argument("Input must be an IFC STEP or IFCX model");
    if (output.extension() != ".usda")
      throw std::invalid_argument("Output must use the .usda extension");

    robotics::RoboticsSceneOptions options;
    options.sourceNamespace = utf8(fs::path(args[3]));
    std::optional<float> importScale;
    for (int i = 4; i < count; i += 2) {
      const auto flag = utf8(fs::path(args[i]));
      const fs::path value(args[i + 1]);
      if (flag == "--import-scale" && !importScale)
        importScale = positiveNumber<float>(value, flag);
      else if (flag == "--metres-per-renderer-unit" && !options.metersPerRendererUnit)
        options.metersPerRendererUnit = positiveNumber<double>(value, flag);
      else
        throw std::invalid_argument("Unknown or repeated option " + flag);
    }

    const auto scale = importScale.value_or(1.0f);
    const auto model = extension == ".ifc"
        ? container::geometry::ifc::LoadFromFile(input, scale)
        : container::geometry::ifcx::LoadFromFile(input, scale);
    const auto written = robotics::writeRoboticsOpenUsd(output, model, options);
    std::cout << "Exported " << written.entityCount << " entities, " << written.meshCount
              << " visual meshes and " << written.colliderCount << " collision proxies\n";
    if (!written.diagnostics.empty())
      std::cout << written.diagnostics.size() << " diagnostics are available in /World.robotics:diagnostics\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "BimRoboticsUsd: " << error.what() << '\n';
    return 1;
  }
}

} // namespace

#ifdef _WIN32
int wmain(int count, wchar_t** args) { return run(count, args); }
#else
int main(int count, char** args) { return run(count, args); }
#endif
