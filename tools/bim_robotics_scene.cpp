#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/robotics/RoboticsSceneManifest.h"

#include <algorithm>
#include <charconv>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view usage =
    "Usage: BimRoboticsScene INPUT.ifc|INPUT.ifcx OUTPUT.json SOURCE_NAMESPACE "
    "[--metres-per-renderer-unit N]\n";

template <class Char>
int run(int count, Char** args) {
  if (count == 2 && std::filesystem::path(args[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  if (count != 4 && (count != 6 || std::filesystem::path(args[4]) != "--metres-per-renderer-unit")) {
    std::cerr << usage;
    return 2;
  }

  try {
    const std::filesystem::path input(args[1]), output(args[2]);
    std::string extension = input.extension().string();
    std::ranges::transform(extension, extension.begin(), [](char ch) {
      return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    });
    if (extension != ".ifc" && extension != ".ifcx")
      throw std::invalid_argument("Input must be an IFC STEP or IFCX model");
    if (output.extension() != ".json")
      throw std::invalid_argument("Output must use the .json extension");

    const auto key = std::filesystem::path(args[3]).u8string();
    container::robotics::RoboticsSceneOptions options;
    options.sourceNamespace.assign(key.begin(), key.end());
    if (count == 6) {
      const auto encoded = std::filesystem::path(args[5]).u8string();
      const std::string number(encoded.begin(), encoded.end());
      double scale{};
      const auto* end = number.data() + number.size();
      const auto parsed = std::from_chars(number.data(), end, scale);
      if (parsed.ec != std::errc{} || parsed.ptr != end)
        throw std::invalid_argument("Invalid metre scale");
      options.metersPerRendererUnit = scale;
    }
    const auto model = extension == ".ifc" ? container::geometry::ifc::LoadFromFile(input)
                                             : container::geometry::ifcx::LoadFromFile(input);
    const auto scene = container::robotics::buildRoboticsScene(model, options);
    container::robotics::writeRoboticsSceneManifest(output, scene);
    std::cout << "Exported " << scene.entities.size() << " semantic entities\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "BimRoboticsScene: " << error.what() << '\n';
    return 1;
  }
}

}  // namespace

#ifdef _WIN32
int wmain(int count, wchar_t** args) { return run(count, args); }
#else
int main(int count, char** args) { return run(count, args); }
#endif
