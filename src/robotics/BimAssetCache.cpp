#include "Container/robotics/BimAssetCache.h"

#include "BimAssetCacheCodec.h"
#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/robotics/RoboticsSceneManifest.h"
#include "Container/utility/ModelImportCancellation.h"
#include "Container/utility/Platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <variant>

namespace container::robotics {
namespace {

// Bump when changing the on-disk schema or portable preprocessing semantics.
constexpr uint32_t formatVersion = 2;
constexpr std::string_view producerRevision = CONTAINER_BIM_PREPROCESSOR_REVISION;

using SourceData = std::variant<std::string, geometry::ifcx::SourceSnapshot>;

util::ContentFingerprint fingerprint(std::string_view bytes, std::stop_token stop) {
  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  return util::FingerprintContent({data, bytes.size()}, stop);
}

std::string readNativeSource(const std::filesystem::path& file, std::stop_token stop) {
  std::ifstream input(file, std::ios::binary);
  if (!input)
    throw std::runtime_error("Could not open IFC source " + util::pathToUtf8(file));
  std::array<char, 64 * 1024> buffer;
  std::string result;
  do {
    util::checkModelImportCancellation(stop);
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    result.append(buffer.data(), static_cast<size_t>(input.gcount()));
  } while (input);
  if (input.bad() || !input.eof())
    throw std::runtime_error("Could not read IFC source " + util::pathToUtf8(file));
  return result;
}

SourceData prepareSource(const std::filesystem::path& path,
                         std::vector<BimAssetSourceLayer>& layers, std::stop_token stop) {
  std::string extension = path.extension().string();
  std::ranges::transform(extension, extension.begin(), [](char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
  });
  if (extension == ".ifc") {
    auto text = readNativeSource(path, stop);
    layers.push_back({path, fingerprint(text, stop)});
    return text;
  }
  if (extension == ".ifcx") {
    auto snapshot = geometry::ifcx::PrepareSource(path, stop);
    layers.reserve(snapshot.layers.size());
    for (const auto& layer : snapshot.layers)
      layers.push_back({layer.path, layer.fingerprint});
    return snapshot;
  }
  throw std::invalid_argument("BIM preprocessing expects a native IFC or IFCX file");
}

std::string makeKey(const std::vector<BimAssetSourceLayer>& layers, bool ifcx,
                    const BimAssetPreprocessOptions& options, std::stop_token stop) {
  asset_cache::Writer writer;
  writer.text("container.bim-preprocessed-assets");
  writer.u32(formatVersion);
  writer.text(producerRevision);
  writer.u32(ifcx);
  writer.count(layers.size());
  for (const auto& layer : layers) {
    writer.text(layer.fingerprint.digest);
    writer.u64(layer.fingerprint.byteSize);
    // IFCX composition and relative names can depend on the chosen layer path.
    if (ifcx)
      writer.text(util::pathToUtf8(layer.path));
  }
  writer.text(options.scene.sourceNamespace);
  writer.u32(options.scene.metersPerRendererUnit.has_value());
  if (options.scene.metersPerRendererUnit)
    writer.f64(*options.scene.metersPerRendererUnit);
  writer.u64(options.cook.maxColliders);
  writer.u64(options.cook.maxTriangleReferences);
  writer.f64(options.maxSlopeDegrees);
  return fingerprint(writer.data, stop).digest;
}

void decode(std::string_view payload, bool ifcx, const BimAssetPreprocessOptions& options,
            BimPreprocessedAssets& result) {
  asset_cache::Reader reader(payload);
  asset_cache::require(reader.text() == result.producerRevision);
  asset_cache::require(reader.count(3 * sizeof(uint32_t)) == result.sourceLayers.size());
  for (const auto& layer : result.sourceLayers) {
    asset_cache::require(reader.text() == layer.fingerprint.digest);
    asset_cache::require(reader.u64() == layer.fingerprint.byteSize);
    asset_cache::require(reader.text() == (ifcx ? util::pathToUtf8(layer.path) : std::string{}));
  }
  auto scene = parseRoboticsScene(reader.text());
  asset_cache::require(scene.sourceNamespace == options.scene.sourceNamespace);
  if (options.scene.metersPerRendererUnit)
    asset_cache::require(scene.frame.metersPerRendererUnit == options.scene.metersPerRendererUnit);
  auto model = asset_cache::readRender(reader);
  CookedBimCollision collision;
  std::vector<BimWalkableSurface> surfaces;
  asset_cache::readCollision(reader, scene, options.maxSlopeDegrees, collision, surfaces);
  asset_cache::require(reader.empty());
  result.scene = std::move(scene);
  result.renderModel = std::move(model);
  result.collision = std::move(collision);
  result.walkableSurfaces = std::move(surfaces);
}

} // namespace

BimPreprocessedAssets loadBimPreprocessedAssets(
    const std::filesystem::path& source, const std::filesystem::path& cacheDirectory,
    const BimAssetPreprocessOptions& options, std::stop_token stop) {
  if (options.scene.sourceNamespace.empty())
    throw std::invalid_argument("BIM preprocessing requires a source namespace");
  if (options.scene.metersPerRendererUnit &&
      (!std::isfinite(*options.scene.metersPerRendererUnit) || *options.scene.metersPerRendererUnit <= 0))
    throw std::invalid_argument("BIM preprocessing requires a finite positive metre scale");
  if (!options.cook.maxColliders || !options.cook.maxTriangleReferences ||
      !std::isfinite(options.maxSlopeDegrees) || options.maxSlopeDegrees < 0 ||
      options.maxSlopeDegrees >= 90)
    throw std::invalid_argument("BIM preprocessing requires positive cook limits and a slope in [0, 90) degrees");

  BimPreprocessedAssets result;
  result.producerRevision = producerRevision;
  const auto path = std::filesystem::absolute(source).lexically_normal();
  auto input = prepareSource(path, result.sourceLayers, stop);
  const auto* ifcx = std::get_if<geometry::ifcx::SourceSnapshot>(&input);
  result.cacheKey = makeKey(result.sourceLayers, ifcx != nullptr, options, stop);
  const auto cacheFile = cacheDirectory.empty()
      ? std::filesystem::path{}
      : cacheDirectory / (result.cacheKey + ".bimassets");

  {
    const auto cached = util::readDerivedAssetCache(
        cacheFile, formatVersion, result.cacheKey, asset_cache::maxPayloadBytes, stop);
    result.cacheStatus = cached.status;
    if (cached.status == util::DerivedAssetCacheStatus::Hit) {
      try {
        decode(cached.payload, ifcx != nullptr, options, result);
        util::checkModelImportCancellation(stop);
        return result;
      } catch (const util::ModelImportCancelled&) {
        throw;
      } catch (const std::exception&) {
        result.cacheStatus = util::DerivedAssetCacheStatus::Corrupt;
      }
    }
    using enum util::DerivedAssetCacheStatus;
    if (result.cacheStatus == Corrupt)
      result.cacheDiagnostic = "BIM cache entry failed integrity or schema validation and was rebuilt.";
    if (result.cacheStatus == IoError)
      result.cacheDiagnostic = "BIM cache entry could not be read. Assets were built from the source.";
  }

  result.renderModel = ifcx
      ? geometry::ifcx::LoadFromSnapshot(*ifcx, 1.f, stop)
      : geometry::ifc::LoadFromStep(std::get<std::string>(input), 1.f, stop);
  result.scene = buildRoboticsScene(result.renderModel, options.scene);
  result.collision = cookBimCollisionGeometry(result.renderModel, result.scene, options.cook);
  result.walkableSurfaces = extractWalkableSurfaces(result.collision, options.maxSlopeDegrees);
  util::checkModelImportCancellation(stop);
  if (!cacheFile.empty()) {
    asset_cache::Writer payload;
    try {
      payload.text(result.producerRevision);
      payload.count(result.sourceLayers.size());
      for (const auto& layer : result.sourceLayers) {
        payload.text(layer.fingerprint.digest);
        payload.u64(layer.fingerprint.byteSize);
        payload.text(ifcx ? util::pathToUtf8(layer.path) : std::string{});
      }
      payload.text(serializeRoboticsScene(result.scene));
      asset_cache::writeRender(payload, result.renderModel);
      asset_cache::writeCollision(payload, result.collision, result.walkableSurfaces);
    } catch (const std::invalid_argument&) {
      result.cacheStatus = util::DerivedAssetCacheStatus::NotCacheable;
      result.cacheDiagnostic = "Built assets exceed the supported BIM cache format. No entry was written.";
      return result;
    }
    result.cacheBytesWritten =
        util::writeDerivedAssetCache(cacheFile, formatVersion, result.cacheKey, payload.data, stop);
    if (!result.cacheBytesWritten) {
      result.cacheStatus = util::DerivedAssetCacheStatus::IoError;
      result.cacheDiagnostic = "Assets were built from the source. The BIM cache entry could not be written.";
    }
  }
  return result;
}

} // namespace container::robotics
