#include "Container/robotics/BimAssetCache.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using Json = nlohmann::json;
namespace robotics = container::robotics;

constexpr std::string_view usage =
    "Usage: BimRoboticsAssets INPUT.ifc|INPUT.ifcx CACHE_DIR SOURCE_NAMESPACE "
    "[--metres-per-renderer-unit N] [--render-glb OUTPUT.glb]\n";

template <class Char>
int run(int count, Char** args) {
  if (count == 2 && std::filesystem::path(args[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  if (count < 4 || count > 8 || count % 2 != 0) {
    std::cerr << usage;
    return 2;
  }
  try {
    robotics::BimAssetPreprocessOptions options;
    options.scene.sourceNamespace = container::util::pathToUtf8(std::filesystem::path(args[3]));
    std::filesystem::path preview;
    for (int i = 4; i < count; i += 2) {
      const auto flag = std::filesystem::path(args[i]);
      if (flag == "--metres-per-renderer-unit" && !options.scene.metersPerRendererUnit) {
        const auto text = container::util::pathToUtf8(std::filesystem::path(args[i + 1]));
        double scale{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), scale);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
          throw std::invalid_argument("Invalid metre scale");
        options.scene.metersPerRendererUnit = scale;
      } else if (flag == "--render-glb" && preview.empty()) {
        preview = std::filesystem::path(args[i + 1]);
        if (preview.extension() != ".glb")
          throw std::invalid_argument("The render preview must use the .glb extension");
      } else {
        throw std::invalid_argument("Unknown or repeated BIM asset option");
      }
    }

    const auto assets = robotics::loadBimPreprocessedAssets(
        std::filesystem::path(args[1]), std::filesystem::path(args[2]), options);
    if (!preview.empty())
      robotics::writeBimCachedRenderPreview(preview, assets.renderModel, assets.scene);

    Json layers = Json::array();
    for (const auto& layer : assets.sourceLayers)
      layers.push_back({{"path", container::util::pathToUtf8(layer.path)},
                        {"sha256", layer.fingerprint.digest},
                        {"bytes", layer.fingerprint.byteSize}});
    const auto& frame = assets.scene.frame;
    const auto scale = [](const std::optional<double>& value) -> Json {
      return value ? Json(*value) : Json(nullptr);
    };
    Json report{{"schema", "container.bim-preprocessed-assets"}, {"version", 1},
                {"sourceNamespace", assets.scene.sourceNamespace},
                {"producerRevision", assets.producerRevision},
                {"cacheKey", assets.cacheKey},
                {"cacheStatus", container::util::derivedAssetCacheStatusName(assets.cacheStatus)},
                {"cacheBytesWritten", assets.cacheBytesWritten},
                {"cacheDiagnostic", assets.cacheDiagnostic}, {"sourceLayers", std::move(layers)},
                {"units", {{"source", frame.sourceUnits}, {"provenance", frame.unitProvenance},
                           {"sourceMetersPerUnit", scale(frame.sourceMetersPerUnit)},
                           {"metersPerRendererUnit", scale(frame.metersPerRendererUnit)}}},
                {"semanticEntities", assets.scene.entities.size()},
                {"renderOccurrences", assets.renderModel.elements.size()},
                {"renderVertices", assets.renderModel.vertices.size()},
                {"collisionShapes", assets.collision.colliders.size()},
                {"walkableTriangles", assets.walkableSurfaces.size()}};
    std::cout << report.dump(2) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "BimRoboticsAssets: " << error.what() << '\n';
    return 1;
  }
}

} // namespace

#ifdef _WIN32
int wmain(int count, wchar_t** args) { return run(count, args); }
#else
int main(int count, char** args) { return run(count, args); }
#endif
