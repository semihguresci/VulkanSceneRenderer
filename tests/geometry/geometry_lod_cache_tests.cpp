#include "Container/geometry/GeometryLodCache.h"
#include "Container/utility/ModelImportCancellation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>

namespace container::geometry {
namespace {

using util::DerivedAssetCacheStatus;
namespace fs = std::filesystem;

struct MeshFixture {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  std::array<PrimitiveRange, 2> primitives{};
};

MeshFixture makeGrids() {
  constexpr uint32_t side = 25;
  MeshFixture mesh;
  for (uint32_t grid = 0; grid < 2; ++grid) {
    const auto firstIndex = static_cast<uint32_t>(mesh.indices.size());
    const auto firstVertex = static_cast<uint32_t>(mesh.vertices.size());
    for (uint32_t y = 0; y < side; ++y) {
      for (uint32_t x = 0; x < side; ++x) {
        Vertex vertex;
        vertex.position = {2.0f * grid + float(x) / (side - 1), float(y) / (side - 1), 0.0f};
        vertex.normal = {0.0f, 0.0f, 1.0f};
        mesh.vertices.push_back(vertex);
      }
    }
    for (uint32_t y = 0; y + 1 < side; ++y) {
      for (uint32_t x = 0; x + 1 < side; ++x) {
        const auto a = firstVertex + y * side + x;
        mesh.indices.insert(mesh.indices.end(), {a, a + 1, a + side + 1, a, a + side + 1, a + side});
      }
    }
    mesh.primitives[grid] = {firstIndex, static_cast<uint32_t>(mesh.indices.size()) - firstIndex,
                             static_cast<int32_t>(grid)};
  }
  return mesh;
}

void expectSameGeometry(const PreparedGeometryLods& actual, const PreparedGeometryLods& expected) {
  EXPECT_EQ(actual.indices, expected.indices);
  ASSERT_EQ(actual.ranges.size(), expected.ranges.size());
  for (size_t i = 0; i < actual.ranges.size(); ++i) {
    const auto& a = actual.ranges[i];
    const auto& b = expected.ranges[i];
    EXPECT_EQ(a.firstIndex, b.firstIndex);
    EXPECT_EQ(a.indexCount, b.indexCount);
    ASSERT_EQ(a.levels.size(), b.levels.size());
    for (size_t j = 0; j < a.levels.size(); ++j) {
      EXPECT_EQ(a.levels[j].firstIndex, b.levels[j].firstIndex);
      EXPECT_EQ(a.levels[j].indexCount, b.levels[j].indexCount);
      EXPECT_EQ(a.levels[j].maxError, b.levels[j].maxError);
    }
  }
}

class GeometryLodCacheTests : public testing::Test {
 protected:
  void SetUp() override {
    static std::atomic_uint64_t serial{};
    parent_ = fs::weakly_canonical(fs::temp_directory_path());
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 32; ++attempt) {
      auto candidate = parent_ / ("container-geometry-lod-cache-" + std::to_string(stamp) +
                                  "-" + std::to_string(++serial));
      if (fs::create_directory(candidate)) {
        root_ = std::move(candidate);
        return;
      }
    }
    FAIL() << "Unable to create an isolated cache directory";
  }

  void TearDown() override {
    if (root_.empty())
      return;
    std::error_code error;
    const auto resolved = fs::weakly_canonical(root_, error);
    if (!error && resolved.parent_path() == parent_ && resolved.filename() == root_.filename() &&
        root_.filename().string().starts_with("container-geometry-lod-cache-"))
      fs::remove_all(resolved, error);
  }

  [[nodiscard]] fs::path soleCacheFile() const {
    const fs::directory_iterator files(root_ / "geometry-lod");
    EXPECT_NE(files, fs::directory_iterator{});
    return files == fs::directory_iterator{} ? fs::path{} : files->path();
  }

  fs::path parent_;
  fs::path root_;
};

TEST_F(GeometryLodCacheTests, WarmCacheReconstructsBothPrimitiveDomainsAndOriginalPrefix) {
  const auto mesh = makeGrids();
  const auto cold = prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_);
  ASSERT_EQ(cold.cache.status, DerivedAssetCacheStatus::Miss);
  EXPECT_EQ(cold.cache.key.size(), 64u);
  ASSERT_GT(cold.cache.bytesWritten, 0u);
  ASSERT_EQ(cold.ranges.size(), 2u);
  ASSERT_FALSE(cold.ranges[0].levels.empty());
  ASSERT_FALSE(cold.ranges[1].levels.empty());
  ASSERT_GT(cold.indices.size(), mesh.indices.size());
  EXPECT_TRUE(std::ranges::equal(std::span(cold.indices).first(mesh.indices.size()), mesh.indices));
  EXPECT_LT(cold.cache.bytesWritten, cold.indices.size() * sizeof(uint32_t));

  const auto warm = prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_);
  EXPECT_EQ(warm.cache.status, DerivedAssetCacheStatus::Hit);
  EXPECT_EQ(warm.cache.key, cold.cache.key);
  EXPECT_EQ(warm.cache.bytesRead, cold.cache.bytesWritten);
  EXPECT_EQ(warm.cache.bytesWritten, 0u);
  EXPECT_EQ(warm.cache.generationMilliseconds, 0.0);
  expectSameGeometry(warm, cold);
  expectSameGeometry(cold, prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives));
}

TEST_F(GeometryLodCacheTests, GeometryPrimitiveAndGeneratorParametersInvalidateTheKey) {
  auto mesh = makeGrids();
  EXPECT_EQ(prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_).cache.status,
            DerivedAssetCacheStatus::Miss);
  mesh.vertices[12].texCoord.x = 0.5f;
  EXPECT_EQ(prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_).cache.status,
            DerivedAssetCacheStatus::Miss);
  std::swap(mesh.indices[1], mesh.indices[2]);
  EXPECT_EQ(prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_).cache.status,
            DerivedAssetCacheStatus::Miss);
  mesh.primitives[0].materialIndex = 17;
  EXPECT_EQ(prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_).cache.status,
            DerivedAssetCacheStatus::Miss);
  const GeometryLodPreparationOptions options{.sourceIndexBudget = 120};
  EXPECT_EQ(prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_, options).cache.status,
            DerivedAssetCacheStatus::Miss);
  EXPECT_EQ(prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_, options).cache.status,
            DerivedAssetCacheStatus::Hit);
}

TEST_F(GeometryLodCacheTests, PersistsAnEmptyTailButSkipsOutOfBoundsPreparation) {
  const auto source = makeGrids();
  const auto indices = std::span(source.indices).first(3);
  const std::array primitives{PrimitiveRange{0, 3}};
  const auto cold = prepareGeometryLods(source.vertices, indices, primitives, root_);
  EXPECT_EQ(cold.cache.status, DerivedAssetCacheStatus::Miss);
  EXPECT_GT(cold.cache.bytesWritten, 0u);
  EXPECT_TRUE(cold.indices.empty());
  const auto warm = prepareGeometryLods(source.vertices, indices, primitives, root_);
  EXPECT_EQ(warm.cache.status, DerivedAssetCacheStatus::Hit);
  EXPECT_TRUE(warm.indices.empty());
  const auto unavailable = prepareGeometryLods(source.vertices, indices, primitives, root_,
                                                {.maxBufferIndexCount = 3});
  EXPECT_EQ(unavailable.cache.status, DerivedAssetCacheStatus::NotCacheable);
  EXPECT_EQ(unavailable.cache.bytesWritten, 0u);
}

TEST_F(GeometryLodCacheTests, TruncatedEntryFallsBackAndIsReplaced) {
  const auto mesh = makeGrids();
  const auto cold = prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_);
  ASSERT_GT(cold.cache.bytesWritten, 1u);
  const auto file = soleCacheFile();
  fs::resize_file(file, cold.cache.bytesWritten - 1);
  const auto repaired = prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_);
  EXPECT_EQ(repaired.cache.status, DerivedAssetCacheStatus::Corrupt);
  EXPECT_EQ(repaired.cache.bytesWritten, cold.cache.bytesWritten);
  expectSameGeometry(repaired, cold);
  EXPECT_EQ(prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_).cache.status,
            DerivedAssetCacheStatus::Hit);
}

TEST_F(GeometryLodCacheTests, VerifiedEnvelopeCannotLoadIndicesFromAnotherPrimitive) {
  const auto mesh = makeGrids();
  const auto cold = prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_);
  ASSERT_GT(cold.cache.bytesWritten, 0u);
  const auto file = soleCacheFile();
  const auto key = file.stem().string();
  auto record = util::readDerivedAssetCache(file, 1, key, cold.cache.bytesWritten);
  ASSERT_EQ(record.status, DerivedAssetCacheStatus::Hit);
  // Two aggregate counts, two primitive fields, and two level fields precede
  // the first reduced index. Give it a valid vertex from the other primitive.
  constexpr size_t firstReducedIndex = 6 * sizeof(uint32_t);
  ASSERT_GE(record.payload.size(), firstReducedIndex + sizeof(uint32_t));
  const auto wrongVertex = mesh.indices[mesh.primitives[1].firstIndex];
  for (size_t byte = 0; byte < sizeof(uint32_t); ++byte)
    record.payload[firstReducedIndex + byte] = static_cast<char>(wrongVertex >> (8 * byte));
  ASSERT_GT(util::writeDerivedAssetCache(file, 1, key, record.payload), 0u);

  const auto repaired = prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_);
  EXPECT_EQ(repaired.cache.status, DerivedAssetCacheStatus::Corrupt);
  expectSameGeometry(repaired, cold);
}

TEST_F(GeometryLodCacheTests, CacheIoFailureKeepsGeneratedGeometry) {
  const auto mesh = makeGrids();
  const auto disabled = prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives);
  EXPECT_EQ(disabled.cache.status, DerivedAssetCacheStatus::Disabled);
  const auto invalidDirectory = root_ / "ordinary-file";
  std::ofstream(invalidDirectory) << "cache path cannot be a file";
  const auto recovered = prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, invalidDirectory);
  EXPECT_EQ(recovered.cache.bytesWritten, 0u);
  expectSameGeometry(recovered, disabled);
}

TEST_F(GeometryLodCacheTests, ChecksVersionKeyChecksumAndPhysicalPayloadLimit) {
  const auto file = root_ / "envelope.bin";
  const std::string key(64, 'a');
  constexpr std::string_view payload{"valid cache payload"};
  const auto written = util::writeDerivedAssetCache(file, 12, key, payload);
  ASSERT_GT(written, payload.size());
  const auto loaded = util::readDerivedAssetCache(file, 12, key, payload.size());
  ASSERT_EQ(loaded.status, DerivedAssetCacheStatus::Hit);
  EXPECT_EQ(loaded.payload, payload);
  EXPECT_EQ(loaded.bytesRead, written);
  EXPECT_EQ(util::readDerivedAssetCache(file, 13, key, payload.size()).status, DerivedAssetCacheStatus::Corrupt);
  EXPECT_EQ(util::readDerivedAssetCache(file, 12, std::string(64, 'b'), payload.size()).status,
            DerivedAssetCacheStatus::Corrupt);
  EXPECT_EQ(util::readDerivedAssetCache(file, 12, key, payload.size() - 1).status,
            DerivedAssetCacheStatus::Corrupt);
  {
    std::fstream corrupt(file, std::ios::binary | std::ios::in | std::ios::out);
    corrupt.seekp(-1, std::ios::end);
    corrupt.put('!');
    ASSERT_TRUE(corrupt);
  }
  EXPECT_EQ(util::readDerivedAssetCache(file, 12, key, payload.size()).status, DerivedAssetCacheStatus::Corrupt);
}

TEST_F(GeometryLodCacheTests, ConcurrentWritersPublishOneCompleteEntry) {
  const auto file = root_ / "concurrent.bin";
  const std::string key(64, 'a');
  const std::string first(128 * 1024, 'x');
  const std::string second(128 * 1024, 'y');
  auto firstWrite = std::async(std::launch::async, [&] {
    return util::writeDerivedAssetCache(file, 1, key, first);
  });
  auto secondWrite = std::async(std::launch::async, [&] {
    return util::writeDerivedAssetCache(file, 1, key, second);
  });
  ASSERT_GT(firstWrite.get(), 0u);
  ASSERT_GT(secondWrite.get(), 0u);
  const auto record = util::readDerivedAssetCache(file, 1, key, first.size());
  ASSERT_EQ(record.status, DerivedAssetCacheStatus::Hit);
  EXPECT_TRUE(record.payload == first || record.payload == second);
}

TEST_F(GeometryLodCacheTests, PropagatesCancellationBeforeWriting) {
  const auto mesh = makeGrids();
  std::stop_source stop;
  ASSERT_TRUE(stop.request_stop());
  EXPECT_THROW((void)prepareGeometryLods(mesh.vertices, mesh.indices, mesh.primitives, root_, {},
                                         stop.get_token()), util::ModelImportCancelled);
  EXPECT_TRUE(fs::is_empty(root_));
}

} // namespace
} // namespace container::geometry
