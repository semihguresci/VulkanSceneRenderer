#include "Container/renderer/bim/BimDerivedCache.h"

#include "Container/utility/ContentFingerprint.h"
#include "Container/utility/ModelImportCancellation.h"
#include "Container/utility/Platform.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace container::renderer {
namespace {

using util::DerivedAssetCacheStatus;

constexpr uint32_t kBimDerivedVersion = 1;
constexpr size_t kMaximumPayload = 128u * 1024u * 1024u;

void require(bool valid) {
  if (!valid) {
    throw std::invalid_argument("Invalid BIM derived cache data");
  }
}

std::string digest(std::span<const std::byte> bytes, std::stop_token stop) {
  return util::FingerprintContent(
      {reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()}, stop).digest;
}

class Writer {
 public:
  void integer(uint32_t value) {
    require(data.size() <= kMaximumPayload - sizeof(value));
    for (unsigned shift = 0; shift < 32; shift += 8) {
      data.push_back(static_cast<char>(value >> shift));
    }
  }

  void count(size_t value) {
    require(value <= std::numeric_limits<uint32_t>::max());
    integer(static_cast<uint32_t>(value));
  }

  void real(float value) { integer(std::bit_cast<uint32_t>(value)); }

  void vec(glm::vec3 value) {
    real(value.x);
    real(value.y);
    real(value.z);
  }

  void text(std::string_view value) {
    count(value.size());
    require(value.size() <= kMaximumPayload - data.size());
    data.append(value);
  }

  std::string data;
};

class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}

  uint32_t integer() {
    require(bytes_.size() >= sizeof(uint32_t));
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes_.front())) << shift;
      bytes_.remove_prefix(1);
    }
    return value;
  }

  uint32_t count(size_t itemBytes) {
    const auto value = integer();
    require(value <= bytes_.size() / itemBytes);
    return value;
  }

  bool boolean() {
    const auto value = integer();
    require(value <= 1);
    return value != 0;
  }

  float real() {
    const auto value = std::bit_cast<float>(integer());
    require(std::isfinite(value));
    return value;
  }

  glm::vec3 vec() { return {real(), real(), real()}; }

  std::string text() {
    const auto length = count(1);
    std::string result(bytes_.substr(0, length));
    bytes_.remove_prefix(length);
    return result;
  }

  bool empty() const { return bytes_.empty(); }

 private:
  std::string_view bytes_;
};

std::string sourceKey(const BimManager::PreparedScene &prepared,
                      const std::filesystem::path &sourcePath,
                      bool generateGeometryLods, std::stop_token stop) {
  const auto &model = prepared.model;
  Writer key;
  key.text("bim-derived");
  key.integer(kBimDerivedVersion);
  key.integer(generateGeometryLods);
  key.text(util::FingerprintFileContent(sourcePath, stop).digest);
  if (sourcePath != prepared.assetPath) {
    key.text(util::FingerprintFileContent(prepared.assetPath, stop).digest);
  }
  key.text(digest(std::as_bytes(std::span(model.vertices)), stop));
  key.text(digest(std::as_bytes(std::span(model.indices)), stop));

  // Resolve USD/IFC dependencies through the importer before fingerprinting.
  // These are the parsed inputs consumed by the derived geometry algorithms.
  key.count(model.meshRanges.size());
  for (const auto &range : model.meshRanges) {
    util::checkModelImportCancellation(stop);
    key.integer(range.meshId);
    key.integer(range.firstIndex);
    key.integer(range.indexCount);
    key.vec(range.boundsCenter);
    key.real(range.boundsRadius);
  }
  key.count(model.meshletClusters.size());
  for (const auto &cluster : model.meshletClusters) {
    util::checkModelImportCancellation(stop);
    key.integer(cluster.meshId);
    key.integer(cluster.firstIndex);
    key.integer(cluster.indexCount);
    key.integer(cluster.triangleCount);
    key.integer(cluster.lodLevel);
    key.vec(cluster.boundsCenter);
    key.real(cluster.boundsRadius);
  }
  key.count(model.elements.size());
  for (const auto &element : model.elements) {
    util::checkModelImportCancellation(stop);
    key.integer(element.meshId);
    key.integer(static_cast<uint32_t>(element.geometryKind));
    key.integer(element.materialIndex);
    for (int column = 0; column < 4; ++column) {
      for (int row = 0; row < 4; ++row) {
        key.real(element.transform[column][row]);
      }
    }
    key.text(element.type);
    key.text(element.objectType);
    key.text(element.displayName);
    key.text(element.storeyId);
    key.text(element.storeyName);
  }
  if (generateGeometryLods) {
    key.count(model.materials.size());
    for (const auto &material : model.materials) {
      util::checkModelImportCancellation(stop);
      key.integer(!material.texturePaths.normal.empty());
      key.integer(!material.texturePaths.clearcoatNormal.empty());
      key.integer(material.pbr.normalTextureIndex);
      key.integer(material.pbr.clearcoatNormalTextureIndex);
      key.integer(material.pbr.heightTextureIndex);
      key.real(material.pbr.anisotropyStrength);
    }
  }
  return digest(std::as_bytes(std::span(key.data)), stop);
}

bool contained(uint32_t first, uint32_t count, size_t begin, size_t end) {
  return first >= begin && first <= end && count <= end - first;
}

void writeOverlay(Writer &out, const BimFloorPlanOverlayData &overlay, std::stop_token stop) {
  out.integer(overlay.firstIndex);
  out.integer(overlay.indexCount);
  out.vec(overlay.boundsCenter);
  out.real(overlay.boundsRadius);
  out.count(overlay.storeyDrawCommands.size());
  for (const auto &[storey, draws] : overlay.storeyDrawCommands) {
    util::checkModelImportCancellation(stop);
    out.text(storey);
    out.count(draws.size());
    for (const auto &draw : draws) {
      out.integer(draw.firstIndex);
      out.integer(draw.indexCount);
    }
  }
}

BimFloorPlanOverlayData readOverlay(Reader &in, size_t begin, size_t end, std::stop_token stop) {
  BimFloorPlanOverlayData overlay;
  overlay.firstIndex = in.integer();
  overlay.indexCount = in.integer();
  overlay.boundsCenter = in.vec();
  overlay.boundsRadius = in.real();
  require(overlay.boundsRadius >= 0 && overlay.indexCount % 2u == 0 &&
          contained(overlay.firstIndex, overlay.indexCount, begin, end));
  const auto storeys = in.count(8);
  for (uint32_t i = 0; i < storeys; ++i) {
    util::checkModelImportCancellation(stop);
    auto key = in.text();
    auto [storey, inserted] = overlay.storeyDrawCommands.try_emplace(std::move(key));
    require(inserted && !storey->first.empty());
    const auto count = in.count(8);
    require(count == 1 && overlay.indexCount > 0);
    const auto firstIndex = in.integer();
    const auto indexCount = in.integer();
    require(indexCount > 0 && indexCount % 2u == 0 && indexCount <= overlay.indexCount &&
            contained(firstIndex, indexCount, begin, end) && (firstIndex - begin) % 2u == 0u);
    storey->second.push_back({.firstIndex = firstIndex, .indexCount = indexCount});
  }
  return overlay;
}

struct GeometrySuffix {
  size_t sourceIndexCount{};
  std::vector<geometry::Vertex> vertices;
  std::vector<uint32_t> indices;
  BimFloorPlanOverlayData ground, elevated;
  std::vector<BimMeshletClusterMetadata> meshlets;
  std::unordered_map<uint32_t, std::vector<geometry::GeometryLodRange>> lods;
  std::vector<BimElementBounds> bounds;
};

GeometrySuffix decode(std::string_view payload, const geometry::dotbim::Model &model,
                      bool generateGeometryLods, std::stop_token stop) {
  Reader in(payload);
  GeometrySuffix result;
  result.sourceIndexCount = in.integer();
  const auto vertexCount = in.count(12);
  constexpr size_t indexLimit = std::numeric_limits<uint32_t>::max();
  require(model.vertices.size() <= indexLimit && vertexCount <= indexLimit - model.vertices.size());
  result.vertices.reserve(vertexCount);
  for (uint32_t i = 0; i < vertexCount; ++i) {
    if (i % 4096u == 0u) util::checkModelImportCancellation(stop);
    result.vertices.push_back({.position = in.vec()});
  }
  const auto indexCount = in.count(4);
  const size_t base = model.indices.size();
  require(base <= indexLimit && indexCount <= indexLimit - base);
  const size_t end = base + indexCount;
  require(result.sourceIndexCount >= base && result.sourceIndexCount <= end &&
          (result.sourceIndexCount - base) % 2u == 0u);
  result.indices.reserve(indexCount);
  for (uint32_t i = 0; i < indexCount; ++i) {
    if (i % 4096u == 0u) util::checkModelImportCancellation(stop);
    const auto index = in.integer();
    // The source suffix contains only floor-plan vertices. The generated LOD
    // suffix addresses the original vertices so material seams remain intact.
    if (base + i < result.sourceIndexCount) {
      require(index >= model.vertices.size() && index - model.vertices.size() < vertexCount);
    } else {
      require(generateGeometryLods && index < model.vertices.size());
    }
    result.indices.push_back(index);
  }
  result.ground = readOverlay(in, base, result.sourceIndexCount, stop);
  result.elevated = readOverlay(in, base, result.sourceIndexCount, stop);
  require(result.ground.firstIndex == base &&
          result.elevated.firstIndex == base + result.ground.indexCount);

  std::unordered_map<uint32_t, uint32_t> meshIndexCounts;
  for (const auto &range : model.meshRanges) meshIndexCounts.try_emplace(range.meshId, range.indexCount);
  const auto clusterCount = in.count(40);
  result.meshlets.reserve(clusterCount);
  for (uint32_t i = 0; i < clusterCount; ++i) {
    if (i % 4096u == 0u) util::checkModelImportCancellation(stop);
    BimMeshletClusterMetadata cluster{
        .meshId = in.integer(),
        .firstIndex = in.integer(),
        .indexCount = in.integer(),
        .triangleCount = in.integer(),
        .lodLevel = in.integer(),
        .boundsCenter = in.vec(),
        .boundsRadius = in.real(),
        .estimated = in.boolean(),
    };
    require(meshIndexCounts.contains(cluster.meshId) && cluster.boundsRadius >= 0.0f &&
            contained(cluster.firstIndex, cluster.indexCount, 0, base));
    if (!result.meshlets.empty()) {
      const auto &previous = result.meshlets.back();
      require(std::tie(previous.meshId, previous.lodLevel, previous.firstIndex) <=
              std::tie(cluster.meshId, cluster.lodLevel, cluster.firstIndex));
    }
    result.meshlets.push_back(cluster);
  }

  std::vector<geometry::GeometryLodRange> ordered;
  const auto meshCount = in.count(8);
  require(generateGeometryLods || meshCount == 0u);
  for (uint32_t i = 0; i < meshCount; ++i) {
    util::checkModelImportCancellation(stop);
    const auto mesh = in.integer();
    const auto maximum = meshIndexCounts.find(mesh);
    require(maximum != meshIndexCounts.end());
    auto [row, inserted] = result.lods.try_emplace(mesh);
    const auto levelCount = in.count(12);
    require(inserted && levelCount > 0 && levelCount <= 2);
    uint32_t previousCount = maximum->second;
    for (uint32_t level = 0; level < levelCount; ++level) {
      const geometry::GeometryLodRange range{in.integer(), in.integer(), in.real()};
      require(range.maxError >= 0.f && range.indexCount > 0u && range.indexCount % 3u == 0u &&
              range.indexCount < previousCount &&
              contained(range.firstIndex, range.indexCount, result.sourceIndexCount, end));
      row->second.push_back(range);
      ordered.push_back(range);
      previousCount = range.indexCount;
    }
  }
  std::ranges::sort(ordered, {}, &geometry::GeometryLodRange::firstIndex);
  size_t covered = result.sourceIndexCount;
  for (const auto &range : ordered) {
    require(range.firstIndex == covered);
    covered += range.indexCount;
  }
  require(covered == end);

  const auto boundsCount = in.count(4);
  require(boundsCount == model.elements.size());
  result.bounds.reserve(boundsCount);
  for (uint32_t i = 0; i < boundsCount; ++i) {
    if (i % 4096u == 0u) util::checkModelImportCancellation(stop);
    BimElementBounds bounds;
    bounds.valid = in.boolean();
    if (bounds.valid) {
      bounds.min = in.vec();
      bounds.max = in.vec();
      require(glm::all(glm::lessThanEqual(bounds.min, bounds.max)));
      bounds.center = (bounds.min + bounds.max) * 0.5f;
      bounds.size = bounds.max - bounds.min;
      bounds.radius = glm::length(bounds.size) * 0.5f;
      bounds.floorElevation = bounds.min.y;
      require(std::isfinite(bounds.radius) && std::isfinite(bounds.center.x) &&
              std::isfinite(bounds.center.y) && std::isfinite(bounds.center.z));
    }
    result.bounds.push_back(bounds);
  }
  require(in.empty());
  return result;
}

std::string encode(const BimManager::PreparedScene &prepared,
                   std::span<const BimElementBounds> bounds, std::stop_token stop) {
  Writer out;
  out.count(prepared.sourceIndexCount);
  const auto vertices = std::span(prepared.uploadVertices).subspan(prepared.model.vertices.size());
  out.count(vertices.size());
  for (size_t i = 0; i < vertices.size(); ++i) {
    if (i % 4096u == 0u) util::checkModelImportCancellation(stop);
    out.vec(vertices[i].position);
  }
  const auto indices = std::span(prepared.uploadIndices).subspan(prepared.model.indices.size());
  out.count(indices.size());
  for (size_t i = 0; i < indices.size(); ++i) {
    if (i % 4096u == 0u) util::checkModelImportCancellation(stop);
    out.integer(indices[i]);
  }
  writeOverlay(out, prepared.floorPlanGround, stop);
  writeOverlay(out, prepared.floorPlanSourceElevation, stop);

  out.count(prepared.meshletClusters.size());
  for (size_t i = 0; i < prepared.meshletClusters.size(); ++i) {
    if (i % 4096u == 0u) util::checkModelImportCancellation(stop);
    const auto &cluster = prepared.meshletClusters[i];
    out.integer(cluster.meshId);
    out.integer(cluster.firstIndex);
    out.integer(cluster.indexCount);
    out.integer(cluster.triangleCount);
    out.integer(cluster.lodLevel);
    out.vec(cluster.boundsCenter);
    out.real(cluster.boundsRadius);
    out.integer(cluster.estimated);
  }

  std::vector<uint32_t> ids;
  ids.reserve(prepared.geometryLodRanges.size());
  for (const auto &[mesh, ranges] : prepared.geometryLodRanges) ids.push_back(mesh);
  std::ranges::sort(ids);
  out.count(ids.size());
  for (const auto mesh : ids) {
    util::checkModelImportCancellation(stop);
    out.integer(mesh);
    const auto &ranges = prepared.geometryLodRanges.at(mesh);
    out.count(ranges.size());
    for (const auto &range : ranges) {
      out.integer(range.firstIndex);
      out.integer(range.indexCount);
      out.real(range.maxError);
    }
  }

  out.count(bounds.size());
  for (size_t i = 0; i < bounds.size(); ++i) {
    if (i % 4096u == 0u) util::checkModelImportCancellation(stop);
    out.integer(bounds[i].valid);
    if (bounds[i].valid) {
      out.vec(bounds[i].min);
      out.vec(bounds[i].max);
    }
  }
  return std::move(out.data);
}

} // namespace

bool restoreBimDerivedCache(
    BimManager::PreparedScene &prepared, const std::filesystem::path &directory,
    const std::filesystem::path &sourcePath, bool generateGeometryLods,
    std::vector<BimElementBounds> &bounds, std::stop_token stop) {
  if (directory.empty()) {
    return false;
  }
  auto &report = prepared.derivedCache;
  try {
    report.key = sourceKey(prepared, sourcePath, generateGeometryLods, stop);
    auto pathKey = util::pathToUtf8(std::filesystem::weakly_canonical(sourcePath));
    pathKey += generateGeometryLods ? "|lod" : "|source";
    const auto stem = digest(std::as_bytes(std::span(pathKey)), stop);
    report.file = directory / ("bim-derived-v" + std::to_string(kBimDerivedVersion) + "-" + stem + ".bin");
  } catch (const util::ModelImportCancelled &) {
    throw;
  } catch (const std::exception &) {
    report.status = DerivedAssetCacheStatus::NotCacheable;
    return false;
  }

  auto read = util::readDerivedAssetCache(report.file, kBimDerivedVersion, report.key,
      kMaximumPayload, stop, util::DerivedAssetCacheKeyMismatch::Miss);
  report.status = read.status;
  report.bytesRead = read.bytesRead;
  if (read.status != DerivedAssetCacheStatus::Hit) {
    return false;
  }
  try {
    auto suffix = decode(read.payload, prepared.model, generateGeometryLods, stop);
    util::checkModelImportCancellation(stop);
    prepared.uploadVertices.reserve(prepared.uploadVertices.size() + suffix.vertices.size());
    prepared.uploadIndices.reserve(prepared.uploadIndices.size() + suffix.indices.size());
    prepared.uploadVertices.insert(prepared.uploadVertices.end(), suffix.vertices.begin(), suffix.vertices.end());
    prepared.uploadIndices.insert(prepared.uploadIndices.end(), suffix.indices.begin(), suffix.indices.end());
    prepared.sourceIndexCount = suffix.sourceIndexCount;
    prepared.floorPlanGround = std::move(suffix.ground);
    prepared.floorPlanSourceElevation = std::move(suffix.elevated);
    prepared.meshletClusters = std::move(suffix.meshlets);
    prepared.geometryLodRanges = std::move(suffix.lods);
    bounds = std::move(suffix.bounds);
    return true;
  } catch (const util::ModelImportCancelled &) {
    throw;
  } catch (const std::exception &) {
    report.status = DerivedAssetCacheStatus::Corrupt;
    return false;
  }
}

void saveBimDerivedCache(BimManager::PreparedScene &prepared,
                         std::span<const BimElementBounds> bounds, std::stop_token stop) {
  auto &report = prepared.derivedCache;
  if (report.file.empty() || report.status == DerivedAssetCacheStatus::Hit) {
    return;
  }
  try {
    const auto payload = encode(prepared, bounds, stop);
    report.bytesWritten = util::writeDerivedAssetCache(
        report.file, kBimDerivedVersion, report.key, payload, stop);
  } catch (const util::ModelImportCancelled &) {
    throw;
  } catch (const std::exception &) {
    // An uncacheable payload still uses the valid cold import.
  }
}

} // namespace container::renderer
