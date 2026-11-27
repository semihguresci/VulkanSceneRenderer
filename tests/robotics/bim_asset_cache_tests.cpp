#include "Container/robotics/BimAssetCache.h"
#include "Container/robotics/RoboticsSceneManifest.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace {

using namespace container::robotics;
using container::util::DerivedAssetCacheStatus;
using container::util::FingerprintFileContent;
namespace fs = std::filesystem;

constexpr std::string_view lowerSlabGuid = "0000000000000000000017";
constexpr std::string_view upperSlabGuid = "0000000000000000000053";
constexpr std::string_view wallGuid = "0000000000000000000027";
constexpr std::string_view openingGuid = "0000000000000000000034";
constexpr std::string_view lowerWestGuid = "0000000000000000000087";

const BimAssetPreprocessOptions defaultOptions{.scene = {.sourceNamespace = "cache-tests"}};

fs::path fixture(std::string_view name = "room-connectivity.ifc") {
  return fs::path(CONTAINER_ROBOTICS_FIXTURES) / fs::path(name);
}

std::string readBytes(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot read BIM cache test input");
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void writeBytes(const fs::path& path, std::string_view bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  stream.close();
  if (!stream) throw std::runtime_error("Cannot write BIM cache test input");
}

const SemanticEntity& byGuid(const BimPreprocessedAssets& assets, std::string_view guid) {
  const auto found = std::ranges::find(assets.scene.entities, guid, &SemanticEntity::guid);
  if (found == assets.scene.entities.end())
    throw std::runtime_error("Missing BIM cache test entity " + std::string(guid));
  return *found;
}

const BimCollider& colliderFor(const BimPreprocessedAssets& assets, std::string_view guid) {
  const auto found = std::ranges::find(assets.collision.colliders, byGuid(assets, guid).id,
                                      &BimCollider::entityId);
  if (found == assets.collision.colliders.end())
    throw std::runtime_error("Missing BIM cache test collider " + std::string(guid));
  return *found;
}

void expectExactAssets(const BimPreprocessedAssets& expected, const BimPreprocessedAssets& actual) {
  EXPECT_EQ(actual.cacheKey, expected.cacheKey);
  EXPECT_EQ(actual.producerRevision, expected.producerRevision);
  ASSERT_EQ(actual.sourceLayers.size(), expected.sourceLayers.size());
  for (size_t i = 0; i < expected.sourceLayers.size(); ++i) {
    EXPECT_EQ(actual.sourceLayers[i].path, expected.sourceLayers[i].path);
    EXPECT_EQ(actual.sourceLayers[i].fingerprint, expected.sourceLayers[i].fingerprint);
  }
  EXPECT_EQ(serializeRoboticsScene(actual.scene), serializeRoboticsScene(expected.scene));

  const auto& a = actual.renderModel;
  const auto& e = expected.renderModel;
  EXPECT_EQ(a.indices, e.indices);
  ASSERT_EQ(a.vertices.size(), e.vertices.size());
  for (size_t i = 0; i < e.vertices.size(); ++i) {
    const auto& v = a.vertices[i];
    const auto& expectedVertex = e.vertices[i];
    EXPECT_EQ(std::tie(v.position, v.normal, v.tangent, v.texCoord, v.texCoord1, v.color, v.colorAlpha),
              std::tie(expectedVertex.position, expectedVertex.normal, expectedVertex.tangent,
                       expectedVertex.texCoord, expectedVertex.texCoord1, expectedVertex.color,
                       expectedVertex.colorAlpha));
  }
  ASSERT_EQ(a.meshRanges.size(), e.meshRanges.size());
  for (size_t i = 0; i < e.meshRanges.size(); ++i) {
    const auto& mesh = a.meshRanges[i];
    const auto& expectedMesh = e.meshRanges[i];
    EXPECT_EQ(std::tie(mesh.meshId, mesh.firstIndex, mesh.indexCount, mesh.boundsCenter, mesh.boundsRadius),
              std::tie(expectedMesh.meshId, expectedMesh.firstIndex, expectedMesh.indexCount,
                       expectedMesh.boundsCenter, expectedMesh.boundsRadius));
  }
  ASSERT_EQ(a.elements.size(), e.elements.size());
  for (size_t i = 0; i < e.elements.size(); ++i) {
    const auto& element = a.elements[i];
    const auto& expectedElement = e.elements[i];
    EXPECT_EQ(std::tie(element.meshId, element.geometryKind, element.sourceId, element.semanticSourceId),
              std::tie(expectedElement.meshId, expectedElement.geometryKind, expectedElement.sourceId,
                       expectedElement.semanticSourceId));
    EXPECT_EQ(element.transform, expectedElement.transform);
    EXPECT_EQ(element.color, expectedElement.color);
  }

  ASSERT_EQ(actual.collision.colliders.size(), expected.collision.colliders.size());
  for (size_t i = 0; i < expected.collision.colliders.size(); ++i) {
    const auto& collider = actual.collision.colliders[i];
    const auto& expectedCollider = expected.collision.colliders[i];
    EXPECT_EQ(std::tie(collider.entityId, collider.representationId, collider.sourceMeshId, collider.kind,
                       collider.originMeters, collider.halfExtents, collider.supportCandidate),
              std::tie(expectedCollider.entityId, expectedCollider.representationId, expectedCollider.sourceMeshId,
                       expectedCollider.kind, expectedCollider.originMeters, expectedCollider.halfExtents,
                       expectedCollider.supportCandidate));
    ASSERT_EQ(bool(collider.mesh), bool(expectedCollider.mesh));
    if (collider.mesh) {
      EXPECT_EQ(collider.mesh->indices, expectedCollider.mesh->indices);
      EXPECT_EQ(collider.mesh->vertices, expectedCollider.mesh->vertices);
    }
  }
  ASSERT_EQ(actual.walkableSurfaces.size(), expected.walkableSurfaces.size());
  for (size_t i = 0; i < expected.walkableSurfaces.size(); ++i) {
    const auto& surface = actual.walkableSurfaces[i];
    const auto& expectedSurface = expected.walkableSurfaces[i];
    EXPECT_EQ(std::tie(surface.entityId, surface.representationId, surface.triangleIndex,
                       surface.vertices, surface.normal),
              std::tie(expectedSurface.entityId, expectedSurface.representationId, expectedSurface.triangleIndex,
                       expectedSurface.vertices, expectedSurface.normal));
  }
}

bool wallBlocksHeadHeight(const BimPreprocessedAssets& assets) {
  const glm::dvec3 start(-1, 0, 2.9), direction(1, 0, 0);
  const auto& wall = colliderFor(assets, wallGuid);
  const auto& mesh = *wall.mesh;
  for (size_t i = 0; i < mesh.indices.size(); i += 3) {
    const auto a = glm::dvec3(mesh.vertices[mesh.indices[i]]);
    const auto ab = glm::dvec3(mesh.vertices[mesh.indices[i + 1]]) - a;
    const auto ac = glm::dvec3(mesh.vertices[mesh.indices[i + 2]]) - a;
    const auto edge = glm::cross(direction, ac);
    const double determinant = glm::dot(ab, edge);
    if (std::abs(determinant) < 1e-9)
      continue;
    const auto fromTriangle = start - wall.originMeters - a;
    const double u = glm::dot(fromTriangle, edge) / determinant;
    const auto cross = glm::cross(fromTriangle, ab);
    const double v = glm::dot(direction, cross) / determinant;
    const double distance = glm::dot(ac, cross) / determinant;
    if (u >= 0 && v >= 0 && u + v <= 1 && distance >= 0 && distance <= 2)
      return true;
  }
  return false;
}

class BimAssetCache : public testing::Test {
protected:
  void SetUp() override {
    static std::atomic_uint64_t serial{};
    parent_ = fs::weakly_canonical(fs::temp_directory_path());
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 32; ++attempt) {
      auto candidate = parent_ / ("container-bim-assets-" + std::to_string(stamp) +
                                  "-" + std::to_string(++serial));
      if (fs::create_directory(candidate)) {
        root_ = std::move(candidate);
        return;
      }
    }
    FAIL() << "Unable to create an isolated BIM asset cache directory";
  }

  void TearDown() override {
    if (root_.empty()) return;
    std::error_code error;
    const auto resolved = fs::weakly_canonical(root_, error);
    if (!error && resolved.parent_path() == parent_ && resolved.filename() == root_.filename() &&
        root_.filename().string().starts_with("container-bim-assets-"))
      fs::remove_all(resolved, error);
  }

  BimPreprocessedAssets load(const fs::path& source, const BimAssetPreprocessOptions& options = defaultOptions) const {
    return loadBimPreprocessedAssets(source, root_ / "cache", options);
  }

  fs::path entry(const BimPreprocessedAssets& assets) const {
    return root_ / "cache" / (assets.cacheKey + ".bimassets");
  }

  fs::path parent_;
  fs::path root_;
};

TEST_F(BimAssetCache, BothUnitFixturesRoundTripExactAssetsAndAgreeInMetres) {
  const auto mmSource = fixture();
  const auto metreSource = fixture("room-connectivity-metres.ifc");
  const auto mmFingerprint = FingerprintFileContent(mmSource);
  const auto metreFingerprint = FingerprintFileContent(metreSource);
  const auto mmTime = fs::last_write_time(mmSource);
  const auto metreTime = fs::last_write_time(metreSource);

  const auto mmCold = load(mmSource);
  const auto metreCold = load(metreSource);
  ASSERT_EQ(mmCold.cacheStatus, DerivedAssetCacheStatus::Miss);
  ASSERT_EQ(metreCold.cacheStatus, DerivedAssetCacheStatus::Miss);
  EXPECT_NE(mmCold.cacheKey, metreCold.cacheKey);
  EXPECT_GT(mmCold.cacheBytesWritten, 0u);
  EXPECT_GT(metreCold.cacheBytesWritten, 0u);
  EXPECT_TRUE(fs::is_regular_file(entry(mmCold)));
  EXPECT_TRUE(fs::is_regular_file(entry(metreCold)));
  ASSERT_EQ(mmCold.sourceLayers.size(), 1u);
  ASSERT_EQ(metreCold.sourceLayers.size(), 1u);
  EXPECT_EQ(mmCold.sourceLayers.front().fingerprint, mmFingerprint);
  EXPECT_EQ(metreCold.sourceLayers.front().fingerprint, metreFingerprint);

  const auto mm = load(mmSource);
  const auto metre = load(metreSource);
  ASSERT_EQ(mm.cacheStatus, DerivedAssetCacheStatus::Hit);
  ASSERT_EQ(metre.cacheStatus, DerivedAssetCacheStatus::Hit);
  EXPECT_EQ(mm.cacheBytesWritten, 0u);
  EXPECT_EQ(metre.cacheBytesWritten, 0u);
  expectExactAssets(mmCold, mm);
  expectExactAssets(metreCold, metre);
  for (const auto* assets : {&mmCold, &metreCold, &mm, &metre}) {
    const auto& lower = colliderFor(*assets, lowerSlabGuid);
    const auto& upper = colliderFor(*assets, upperSlabGuid);
    ASSERT_TRUE(lower.mesh && upper.mesh);
    EXPECT_EQ(lower.mesh.get(), upper.mesh.get());
    EXPECT_NEAR(upper.originMeters.z - lower.originMeters.z, 2.5, 1e-6);
    ASSERT_FALSE(assets->walkableSurfaces.empty());
    for (const auto& surface : assets->walkableSurfaces) {
      ASSERT_TRUE(surface.entityId == lower.entityId || surface.entityId == upper.entityId);
      for (const auto& point : surface.vertices)
        EXPECT_NEAR(point.z, surface.entityId == lower.entityId ? 1.2 : 3.7, 1e-6);
    }
  }

  ASSERT_TRUE(mm.scene.frame.sourceMetersPerUnit && metre.scene.frame.sourceMetersPerUnit);
  EXPECT_DOUBLE_EQ(*mm.scene.frame.sourceMetersPerUnit, .001);
  EXPECT_DOUBLE_EQ(*metre.scene.frame.sourceMetersPerUnit, 1.0);
  const auto& westMm = byGuid(mm, lowerWestGuid);
  const auto& westMetre = byGuid(metre, lowerWestGuid);
  EXPECT_EQ(westMm.id, westMetre.id);
  ASSERT_TRUE(westMm.roboticsWorldFromLocal && westMetre.roboticsWorldFromLocal);
  const auto& boundsMm = byGuid(mm, openingGuid).roboticsBounds;
  const auto& boundsMetre = byGuid(metre, openingGuid).roboticsBounds;
  ASSERT_TRUE(boundsMm && boundsMetre);
  for (glm::length_t axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR((*westMm.roboticsWorldFromLocal)[3][axis], (*westMetre.roboticsWorldFromLocal)[3][axis], 1e-6);
    EXPECT_NEAR(boundsMm->minimum[axis], boundsMetre->minimum[axis], 1e-6);
    EXPECT_NEAR(boundsMm->maximum[axis], boundsMetre->maximum[axis], 1e-6);
  }
  EXPECT_FALSE(wallBlocksHeadHeight(mm));
  EXPECT_FALSE(wallBlocksHeadHeight(metre));
  EXPECT_EQ(FingerprintFileContent(mmSource), mmFingerprint);
  EXPECT_EQ(FingerprintFileContent(metreSource), metreFingerprint);
  EXPECT_EQ(fs::last_write_time(mmSource), mmTime);
  EXPECT_EQ(fs::last_write_time(metreSource), metreTime);
}

TEST_F(BimAssetCache, SourceBytesInvalidateGeometryAtIdenticalSizeAndTimestamp) {
  const auto copy = root_ / "same-time.ifc";
  fs::copy_file(fixture(), copy);
  const auto original = load(copy);
  ASSERT_EQ(original.cacheStatus, DerivedAssetCacheStatus::Miss);
  ASSERT_EQ(load(copy).cacheStatus, DerivedAssetCacheStatus::Hit);
  EXPECT_FALSE(wallBlocksHeadHeight(original));
  const auto timestamp = fs::last_write_time(copy);
  const auto fingerprint = FingerprintFileContent(copy);

  auto bytes = readBytes(copy);
  constexpr std::string_view aperture = "#31=IFCEXTRUDEDAREASOLID(#30,#7,#5,2100.);";
  const auto position = bytes.find(aperture);
  ASSERT_NE(position, std::string::npos);
  bytes.replace(position, aperture.size(), "#31=IFCEXTRUDEDAREASOLID(#30,#7,#5,1500.);");
  writeBytes(copy, bytes);
  fs::last_write_time(copy, timestamp);
  EXPECT_EQ(fs::last_write_time(copy), timestamp);
  EXPECT_EQ(fs::file_size(copy), fingerprint.byteSize);
  const auto changedFingerprint = FingerprintFileContent(copy);
  EXPECT_NE(changedFingerprint, fingerprint);

  const auto changed = load(copy);
  ASSERT_EQ(changed.cacheStatus, DerivedAssetCacheStatus::Miss);
  EXPECT_NE(changed.cacheKey, original.cacheKey);
  ASSERT_EQ(changed.sourceLayers.size(), 1u);
  EXPECT_EQ(changed.sourceLayers.front().fingerprint, changedFingerprint);
  EXPECT_EQ(byGuid(changed, wallGuid).id, byGuid(original, wallGuid).id);
  EXPECT_TRUE(wallBlocksHeadHeight(changed));
  const auto& opening = byGuid(changed, openingGuid).roboticsBounds;
  ASSERT_TRUE(opening);
  EXPECT_NEAR(opening->maximum.z, 2.7, 1e-6);
  const auto warm = load(copy);
  ASSERT_EQ(warm.cacheStatus, DerivedAssetCacheStatus::Hit);
  expectExactAssets(changed, warm);
  EXPECT_TRUE(wallBlocksHeadHeight(warm));
  EXPECT_EQ(FingerprintFileContent(copy), changedFingerprint);
  EXPECT_EQ(fs::last_write_time(copy), timestamp);
}

TEST_F(BimAssetCache, CorruptEntryRecooksAndOutputOptionsHaveDistinctKeys) {
  const auto source = fixture();
  const auto original = load(source);
  ASSERT_EQ(original.cacheStatus, DerivedAssetCacheStatus::Miss);
  ASSERT_TRUE(fs::is_regular_file(entry(original)));
  writeBytes(entry(original), "truncated BIM cache entry");

  const auto repaired = load(source);
  ASSERT_EQ(repaired.cacheStatus, DerivedAssetCacheStatus::Corrupt);
  EXPECT_GT(repaired.cacheBytesWritten, 0u);
  expectExactAssets(original, repaired);
  EXPECT_EQ(load(source).cacheStatus, DerivedAssetCacheStatus::Hit);

  auto renamedOptions = defaultOptions;
  renamedOptions.scene.sourceNamespace = "renamed-room-fixture";
  const auto renamed = load(source, renamedOptions);
  ASSERT_EQ(renamed.cacheStatus, DerivedAssetCacheStatus::Miss);
  EXPECT_NE(renamed.cacheKey, original.cacheKey);
  EXPECT_NE(byGuid(renamed, lowerWestGuid).id, byGuid(original, lowerWestGuid).id);
  EXPECT_EQ(load(source, renamedOptions).cacheStatus, DerivedAssetCacheStatus::Hit);

  auto scaledOptions = defaultOptions;
  scaledOptions.scene.metersPerRendererUnit = .5;
  const auto scaled = load(source, scaledOptions);
  ASSERT_EQ(scaled.cacheStatus, DerivedAssetCacheStatus::Miss);
  EXPECT_NE(scaled.cacheKey, original.cacheKey);
  const auto& bounds = byGuid(scaled, openingGuid).roboticsBounds;
  ASSERT_TRUE(bounds);
  EXPECT_NEAR(bounds->maximum.z, 1.65, 1e-6);
  EXPECT_EQ(load(source, scaledOptions).cacheStatus, DerivedAssetCacheStatus::Hit);

  auto slopeOptions = defaultOptions;
  slopeOptions.maxSlopeDegrees = 20;
  const auto slope = load(source, slopeOptions);
  ASSERT_EQ(slope.cacheStatus, DerivedAssetCacheStatus::Miss);
  EXPECT_NE(slope.cacheKey, original.cacheKey);
  EXPECT_EQ(load(source, slopeOptions).cacheStatus, DerivedAssetCacheStatus::Hit);
}

TEST_F(BimAssetCache, ChangedIfcXDependencyInvalidatesRootAndCookedTriangles) {
  const auto source = root_ / "overlay.ifcx";
  const auto layer = root_ / "geometry.ifcx";
  writeBytes(source, R"json({"imports":[{"uri":"geometry.ifcx"}],"data":[]})json");
  writeBytes(layer, R"json({"data":[
    {"path":"wall","children":{"Body":"mesh"},"attributes":{
      "bsi::ifc::class":"IfcWall","bsi::ifc::globalId":"layer-wall"}},
    {"path":"mesh","attributes":{"usd::usdgeom::mesh":{
      "faceVertexIndices":[0,1,2],"points":[[0,0,0],[1,0,0],[0,1,0]]}}}
  ]})json");
  auto options = defaultOptions;
  options.scene.metersPerRendererUnit = 1.0;
  const auto original = load(source, options);
  ASSERT_EQ(original.cacheStatus, DerivedAssetCacheStatus::Miss);
  ASSERT_EQ(original.sourceLayers.size(), 2u);
  EXPECT_EQ(load(source, options).cacheStatus, DerivedAssetCacheStatus::Hit);

  const auto timestamp = fs::last_write_time(layer);
  const auto rootFingerprint = FingerprintFileContent(source);
  const auto layerFingerprint = FingerprintFileContent(layer);
  auto changedBytes = readBytes(layer);
  constexpr std::string_view originalPoint = "[1,0,0]";
  const auto point = changedBytes.find(originalPoint);
  ASSERT_NE(point, std::string::npos);
  changedBytes.replace(point, originalPoint.size(), "[3,0,0]");
  writeBytes(layer, changedBytes);
  fs::last_write_time(layer, timestamp);
  ASSERT_EQ(fs::last_write_time(layer), timestamp);
  ASSERT_EQ(fs::file_size(layer), layerFingerprint.byteSize);
  const auto changedFingerprint = FingerprintFileContent(layer);
  ASSERT_NE(changedFingerprint, layerFingerprint);

  const auto changed = load(source, options);
  ASSERT_EQ(changed.cacheStatus, DerivedAssetCacheStatus::Miss);
  EXPECT_NE(changed.cacheKey, original.cacheKey);
  EXPECT_EQ(FingerprintFileContent(source), rootFingerprint);
  const auto recordedLayer = std::ranges::find_if(changed.sourceLayers, [&](const auto& input) {
    return input.path.filename() == layer.filename();
  });
  ASSERT_NE(recordedLayer, changed.sourceLayers.end());
  EXPECT_EQ(recordedLayer->fingerprint, changedFingerprint);
  const auto& first = colliderFor(original, "layer-wall");
  const auto& second = colliderFor(changed, "layer-wall");
  EXPECT_EQ(first.entityId, second.entityId);
  ASSERT_TRUE(first.mesh && second.mesh);
  const auto minY = [](const BimCollider& collider) {
    double result = std::numeric_limits<double>::infinity();
    for (const auto& vertex : collider.mesh->vertices)
      result = std::min(result, collider.originMeters.y + vertex.y);
    return result;
  };
  EXPECT_NEAR(minY(first), -1.0, 1e-6);
  EXPECT_NEAR(minY(second), -3.0, 1e-6);
  const auto warm = load(source, options);
  ASSERT_EQ(warm.cacheStatus, DerivedAssetCacheStatus::Hit);
  expectExactAssets(changed, warm);
}

} // namespace
