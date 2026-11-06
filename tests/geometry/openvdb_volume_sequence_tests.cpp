#include "Container/geometry/OpenVdbVolumeSequence.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif

namespace {
using namespace container::geometry;

template<class Predicate>
bool waitFor(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
}

std::optional<OpenVdbAsyncSequenceFrame> waitFrame(OpenVdbAsyncVolumeSequence& sequence) {
  std::optional<OpenVdbAsyncSequenceFrame> result;
  (void)waitFor([&] { result = sequence.tryTakeFrame(); return result.has_value(); });
  return result;
}

class OpenVdbSequenceTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
        ("container-vdb-sequence-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
  }
  void TearDown() override {
    std::error_code error;
    // This dedicated temporary directory is created by this fixture alone.
    std::filesystem::remove_all(directory, error);
  }
  std::filesystem::path touch(const std::string& name) {
    const auto path = directory / name;
    std::ofstream(path) << "not a VDB payload";
    return path;
  }
  std::filesystem::path directory;
};

TEST_F(OpenVdbSequenceTest, DiscoveryUsesBoundedNumericOrderingAndLiteralPrefix) {
  for (const auto& name : {"smoke10.vdb", "smoke2.vdb", "smoke1.VDB", "smoke01.vdb",
                           "smoke1000000000000000000000000000000.vdb", "other3.vdb", "smoke3.txt"})
    touch(name);
  std::filesystem::create_directory(directory / "smoke4.vdb");
  const auto manifest = discoverOpenVdbVolumeSequence(directory, 25.0, {.filenamePrefix = "smoke"});
  ASSERT_EQ(manifest.frames.size(), 5u);
  EXPECT_EQ(manifest.frames[0].filename(), "smoke1.VDB");
  EXPECT_EQ(manifest.frames[1].filename(), "smoke01.vdb");
  EXPECT_EQ(manifest.frames[2].filename(), "smoke2.vdb");
  EXPECT_EQ(manifest.frames[3].filename(), "smoke10.vdb");
  EXPECT_EQ(manifest.frames[4].filename(), "smoke1000000000000000000000000000000.vdb");
  EXPECT_DOUBLE_EQ(manifest.durationSeconds(), 0.2);
  EXPECT_TRUE(manifest.frames[0].is_absolute());
  EXPECT_THROW((void)discoverOpenVdbVolumeSequence(directory, 25, {.maxFrames = 3}), std::invalid_argument);
}

TEST_F(OpenVdbSequenceTest, DiscoveryAndClocksRejectInvalidParameters) {
  EXPECT_THROW((void)discoverOpenVdbVolumeSequence(directory, 24), std::invalid_argument);
  touch("frame1.vdb");
  for (const auto fps : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()})
    EXPECT_THROW((void)discoverOpenVdbVolumeSequence(directory, fps), std::invalid_argument);
  EXPECT_THROW((void)discoverOpenVdbVolumeSequence(directory / "missing", 24), std::invalid_argument);
  EXPECT_THROW((void)discoverOpenVdbVolumeSequence(directory, 24, {.maxFrames = 0}), std::invalid_argument);
  EXPECT_THROW((void)OpenVdbSequencePlayback(OpenVdbSequenceManifest{}), std::invalid_argument);
}

TEST(OpenVdbSequencePlaybackTest, LoopHoldSpeedPauseSeekAndBoundaryRounding) {
  OpenVdbSequenceManifest manifest{{"0.vdb", "1.vdb", "2.vdb", "3.vdb"}, 10.0};
  OpenVdbSequencePlayback playback(manifest);
  EXPECT_EQ(playback.frameIndex(), 0u);
  playback.advance(0.3);
  EXPECT_EQ(playback.frameIndex(), 3u);
  playback.advance(0.1);
  EXPECT_EQ(playback.frameIndex(), 0u);
  playback.setSpeed(2.0);
  playback.advance(0.1);
  EXPECT_EQ(playback.frameIndex(), 2u);
  playback.setPaused(true);
  playback.advance(10.0);
  EXPECT_EQ(playback.frameIndex(), 2u);
  playback.seekSeconds(0.1);
  EXPECT_EQ(playback.frameIndex(), 1u);
  playback.setPaused(false);
  playback.setSpeed(0.0);
  playback.advance(10.0);
  EXPECT_EQ(playback.frameIndex(), 1u);
  playback.setLoop(false);
  playback.setSpeed(1.0);
  playback.advance(10.0);
  EXPECT_EQ(playback.frameIndex(), 3u);
  EXPECT_DOUBLE_EQ(playback.positionSeconds(), 0.4);
  playback.setLoop(true);
  EXPECT_EQ(playback.frameIndex(), 0u);
  playback.advance(std::numeric_limits<double>::max());
  EXPECT_TRUE(std::isfinite(playback.positionSeconds()));
  EXPECT_LT(playback.frameIndex(), manifest.frames.size());
  EXPECT_THROW(playback.advance(-1.0), std::invalid_argument);
  EXPECT_THROW(playback.seekSeconds(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
  EXPECT_THROW(playback.setSpeed(-1.0), std::invalid_argument);
  EXPECT_THROW(playback.setSpeed(1001.0), std::invalid_argument);
}

TEST(OpenVdbSequencePlaybackTest, TinyPositiveSpeedAndHugeFiniteDurationStayFinite) {
  OpenVdbSequenceManifest manifest{{"0.vdb", "1.vdb"}, 2.0e-308};
  OpenVdbSequencePlayback playback(manifest, {.speed = 1.0e-308});
  playback.advance(std::numeric_limits<double>::max());
  EXPECT_TRUE(std::isfinite(playback.positionSeconds()));
  EXPECT_EQ(playback.frameIndex(), 0u);
}

TEST_F(OpenVdbSequenceTest, CacheValidatesDensePayloadBudgetBeforeLoading) {
  auto manifest = OpenVdbSequenceManifest{{touch("frame1.vdb")}, 24};
  OpenVdbVolumeOptions options;
  options.maxResolution = 16;
  EXPECT_THROW((void)OpenVdbVolumeSequence(manifest, options, {.maxCachedFrames = 4}), std::invalid_argument);
  EXPECT_THROW((void)OpenVdbVolumeSequence(manifest, options, {.maxCachedBytes = 4096}), std::invalid_argument);
  auto nativeOptions = options;
  nativeOptions.sparse = true;
  EXPECT_THROW((void)OpenVdbVolumeSequence(manifest, nativeOptions,
      {.maxCachedBytes = nativeOptions.maxInputBytes - 1}), std::invalid_argument);
  OpenVdbVolumeSequence cache(manifest, options);
  EXPECT_EQ(cache.cachedFrameCount(), 0u);
  EXPECT_THROW((void)cache.frame(1), std::out_of_range);
  try {
    (void)cache.frame(0);
    FAIL();
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("frame 0"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("frame1.vdb"), std::string::npos);
  }
  EXPECT_EQ(cache.cachedFrameCount(), 0u);
}

TEST_F(OpenVdbSequenceTest, AsyncErrorsAreExplicitAndPollingNeverSubstitutesSuccess) {
  auto sequence = std::make_unique<OpenVdbVolumeSequence>(
      OpenVdbSequenceManifest{{touch("bad.vdb")}, 24}, OpenVdbVolumeOptions{.maxResolution = 16});
  OpenVdbAsyncVolumeSequence async(std::move(sequence));
  EXPECT_EQ(async.manifest().frames.size(), 1u);
  const auto id = async.request(0);
  EXPECT_EQ(async.request(0), id);
  auto completion = waitFrame(async);
  ASSERT_TRUE(completion.has_value());
  EXPECT_EQ(completion->requestId, id);
  EXPECT_EQ(completion->frameIndex, 0u);
  EXPECT_FALSE(completion->volume);
  EXPECT_NE(completion->error.find("frame 0"), std::string::npos);
  EXPECT_NE(completion->error.find("bad.vdb"), std::string::npos);
  EXPECT_FALSE(async.tryTakeFrame().has_value());
  EXPECT_EQ(async.status().state, OpenVdbAsyncSequenceState::Failed);
  EXPECT_THROW((void)async.request(1), std::out_of_range);
  EXPECT_THROW(async.prefetch(1), std::out_of_range);
  async.requestStop();
  ASSERT_TRUE(waitFor([&] { return async.status().state == OpenVdbAsyncSequenceState::Stopped; }));
  EXPECT_THROW((void)async.request(0), std::logic_error);
}

TEST(OpenVdbSequenceAsyncTest, RepeatedIdleShutdownCannotLoseItsStopWake) {
  for (int attempt = 0; attempt < 64; ++attempt) {
    auto sequence = std::make_unique<OpenVdbVolumeSequence>(
        OpenVdbSequenceManifest{{"intentionally-not-loaded.vdb"}, 24},
        OpenVdbVolumeOptions{.maxResolution = 16});
    OpenVdbAsyncVolumeSequence async(std::move(sequence));
    if ((attempt % 2) == 0) async.requestStop();
    // Destruction also requests stop; no file read is required for idle work.
  }
}

TEST(OpenVdbSequenceAsyncTest, ConstructorRejectsMissingOwnershipAndInvalidResidentBudgets) {
  EXPECT_THROW((void)OpenVdbAsyncVolumeSequence(std::unique_ptr<OpenVdbVolumeSequence>{}),
               std::invalid_argument);
  const auto makeSequence = [] {
    return std::make_unique<OpenVdbVolumeSequence>(
        OpenVdbSequenceManifest{{"intentionally-not-loaded.vdb"}, 24},
        OpenVdbVolumeOptions{.maxResolution = 16});
  };
  EXPECT_THROW((void)OpenVdbAsyncVolumeSequence(makeSequence(), {.maxCachedFrames = 0}), std::invalid_argument);
  EXPECT_THROW((void)OpenVdbAsyncVolumeSequence(makeSequence(), {.maxCachedFrames = 4}), std::invalid_argument);
  EXPECT_THROW((void)OpenVdbAsyncVolumeSequence(makeSequence(), {.maxResidentFrames = 0}), std::invalid_argument);
  EXPECT_THROW((void)OpenVdbAsyncVolumeSequence(makeSequence(), {.maxResidentFrames = 4}), std::invalid_argument);
  EXPECT_THROW((void)OpenVdbAsyncVolumeSequence(makeSequence(),
      {.maxCachedFrames = 2, .maxResidentFrames = 1}), std::invalid_argument);
  EXPECT_THROW((void)OpenVdbAsyncVolumeSequence(makeSequence(), {.maxResidentBytes = 16383}),
               std::invalid_argument);
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbSequenceFileTest : public OpenVdbSequenceTest {
protected:
  void write(const std::string& name, float value, double offset,
             const std::string& gridName = "density", bool temperature = false) {
    openvdb::initialize();
    auto density = openvdb::FloatGrid::create(0.0f);
    density->setName(gridName);
    density->setGridClass(openvdb::GRID_FOG_VOLUME);
    density->fill(openvdb::CoordBBox(openvdb::Coord(-1), openvdb::Coord(1)), value, true);
    density->transform().postTranslate(openvdb::Vec3d(offset, 0, 0));
    openvdb::GridPtrVec grids{density};
    if (temperature) {
      auto heat = density->deepCopy();
      heat->setName("temperature");
      grids.push_back(heat);
    }
    openvdb::io::File file((directory / name).string());
    file.write(grids);
    file.close();
  }
};

TEST_F(OpenVdbSequenceFileTest, CacheEvictsLeastRecentlyUsedAndStreamsUnionBounds) {
  write("frame1.vdb", 1.0f, 0.0);
  write("frame2.vdb", 2.0f, 10.0);
  write("frame10.vdb", 3.0f, 20.0);
  const auto manifest = discoverOpenVdbVolumeSequence(directory, 24);
  OpenVdbVolumeSequence sequence(manifest, {.maxResolution = 16},
                                 {.maxCachedFrames = 2, .maxCachedBytes = 32768});
  EXPECT_FLOAT_EQ(sequence.frame(0).maxDensity, 1.0f);
  EXPECT_FLOAT_EQ(sequence.frame(1).maxDensity, 2.0f);
  EXPECT_EQ(sequence.cachedFrameCount(), 2u);
  EXPECT_EQ(sequence.cachedBytes(), 32768u);
  (void)sequence.frame(0); // Refresh its LRU position.
  EXPECT_FLOAT_EQ(sequence.frame(2).maxDensity, 3.0f);
  EXPECT_TRUE(sequence.isCached(0));
  EXPECT_FALSE(sequence.isCached(1));
  EXPECT_TRUE(sequence.isCached(2));
  const auto bounds = sequence.scanUnionBounds();
  EXPECT_FLOAT_EQ(bounds.minimum[0], -1.5f);
  EXPECT_FLOAT_EQ(bounds.maximum[0], 21.5f);
  EXPECT_FLOAT_EQ(bounds.minimum[1], -1.5f);
  EXPECT_FLOAT_EQ(bounds.maximum[2], 1.5f);
  EXPECT_LE(sequence.cachedFrameCount(), 2u);
  EXPECT_LE(sequence.cachedBytes(), 32768u);
  sequence.clearCache();
  EXPECT_EQ(sequence.cachedBytes(), 0u);
  EXPECT_EQ(sequence.cachedFrameCount(), 0u);
}

TEST_F(OpenVdbSequenceFileTest, FrameErrorsAndAutomaticChannelChangesAreExplicit) {
  write("frame1.vdb", 1.0f, 0.0);
  write("frame2.vdb", 2.0f, 0.0, "another-density");
  touch("frame3.vdb");
  OpenVdbVolumeSequence sequence(discoverOpenVdbVolumeSequence(directory, 24), {.maxResolution = 16});
  (void)sequence.frame(0);
  try {
    (void)sequence.frame(1);
    FAIL();
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("frame2.vdb"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("grid names differ"), std::string::npos);
  }
  EXPECT_THROW((void)sequence.frame(2), std::runtime_error);
  EXPECT_THROW((void)sequence.scanUnionBounds(), std::runtime_error);
  EXPECT_FALSE(sequence.isCached(1));
  EXPECT_FALSE(sequence.isCached(2));
}

TEST_F(OpenVdbSequenceFileTest, TemperatureChannelsShareCacheBudgetAndMustExistEveryFrame) {
  write("frame1.vdb", 1.0f, 0.0, "density", true);
  write("frame2.vdb", 2.0f, 0.0);
  OpenVdbVolumeSequence sequence(discoverOpenVdbVolumeSequence(directory, 24),
      {.maxResolution = 16, .temperatureGridName = "temperature"},
      {.maxCachedFrames = 2, .maxCachedBytes = 32768});
  const auto& first = sequence.frame(0);
  EXPECT_EQ(first.temperature.size(), first.density.size());
  EXPECT_EQ(sequence.cachedBytes(), 32768u);
  EXPECT_THROW((void)sequence.frame(1), std::runtime_error);
  EXPECT_EQ(sequence.cachedFrameCount(), 0u); // Old payload retired before the miss.
}

TEST_F(OpenVdbSequenceFileTest, AsyncTransfersWarmedArraysAndRetainsSuccessfulPrefetch) {
  write("frame1.vdb", 1.0f, 0.0);
  write("frame2.vdb", 2.0f, 0.0);
  auto sequence = std::make_unique<OpenVdbVolumeSequence>(
      discoverOpenVdbVolumeSequence(directory, 24), OpenVdbVolumeOptions{.maxResolution = 16});
  const auto* originalArray = sequence->frame(0).density.data();
  OpenVdbAsyncVolumeSequence async(std::move(sequence));
  (void)async.request(0);
  auto first = waitFrame(async);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first->volume);
  EXPECT_TRUE(first->error.empty());
  EXPECT_EQ(first->volume->density.data(), originalArray);
  first.reset();
  async.prefetch(1);
  ASSERT_TRUE(waitFor([&] { return async.status().cachedFrames == 2; }));
  EXPECT_EQ(async.cachedBytes(), 32768u);
  std::filesystem::remove(directory / "frame2.vdb");
  for (int repeat = 0; repeat < 50; ++repeat) async.prefetch(1);
  (void)async.request(1);
  auto second = waitFrame(async);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(second->volume) << second->error;
  EXPECT_FLOAT_EQ(second->volume->maxDensity, 2.0f);
  EXPECT_LE(async.status().residentFrames, 3u);
  EXPECT_LE(async.status().residentBytes, 512ull * 1024 * 1024);
}

TEST_F(OpenVdbSequenceFileTest, AsyncLatestRequestWinsWhileCallerLeaseBoundsMemory) {
  write("frame1.vdb", 1.0f, 0.0);
  write("frame2.vdb", 2.0f, 0.0);
  write("frame3.vdb", 3.0f, 0.0);
  auto sequence = std::make_unique<OpenVdbVolumeSequence>(
      discoverOpenVdbVolumeSequence(directory, 24), OpenVdbVolumeOptions{.maxResolution = 16},
      OpenVdbSequenceCacheOptions{.maxCachedFrames = 1, .maxCachedBytes = 16384});
  OpenVdbAsyncVolumeSequence async(std::move(sequence),
      {.maxCachedFrames = 1, .maxResidentFrames = 1, .maxResidentBytes = 16384});
  (void)async.request(0);
  auto first = waitFrame(async);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first->volume);
  (void)async.request(1);
  ASSERT_TRUE(waitFor([&] { return async.status().state == OpenVdbAsyncSequenceState::WaitingForMemory; }));
  const auto latest = async.request(2);
  ASSERT_TRUE(waitFor([&] {
    const auto status = async.status();
    return status.requestedFrame == 2 && status.state == OpenVdbAsyncSequenceState::WaitingForMemory;
  }));
  EXPECT_FALSE(async.tryTakeFrame().has_value());
  EXPECT_EQ(async.status().residentFrames, 1u);
  EXPECT_EQ(async.status().residentBytes, 16384u);
  first.reset(); // Release the external lease; this must wake the budget wait.
  auto completed = waitFrame(async);
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->requestId, latest);
  EXPECT_EQ(completed->frameIndex, 2u);
  ASSERT_TRUE(completed->volume) << completed->error;
  EXPECT_FLOAT_EQ(completed->volume->maxDensity, 3.0f);
  EXPECT_EQ(async.status().residentFrames, 1u);
  EXPECT_EQ(async.status().residentBytes, 16384u);
}

TEST_F(OpenVdbSequenceFileTest, AsyncNewRequestDiscardsAnUntakenOlderSuccess) {
  write("frame1.vdb", 1.0f, 0.0);
  write("frame2.vdb", 2.0f, 0.0);
  auto sequence = std::make_unique<OpenVdbVolumeSequence>(
      discoverOpenVdbVolumeSequence(directory, 24), OpenVdbVolumeOptions{.maxResolution = 16});
  OpenVdbAsyncVolumeSequence async(std::move(sequence));
  (void)async.request(0);
  ASSERT_TRUE(waitFor([&] { return async.status().state == OpenVdbAsyncSequenceState::Ready; }));
  const auto latest = async.request(1);
  auto completion = waitFrame(async);
  ASSERT_TRUE(completion.has_value());
  EXPECT_EQ(completion->frameIndex, 1u);
  EXPECT_EQ(completion->requestId, latest);
  ASSERT_TRUE(completion->volume) << completion->error;
  EXPECT_FLOAT_EQ(completion->volume->maxDensity, 2.0f);
}

TEST_F(OpenVdbSequenceFileTest, AsyncShutdownWakesBudgetWaitAndCallerFrameOutlivesWorker) {
  write("frame1.vdb", 1.0f, 0.0);
  write("frame2.vdb", 2.0f, 0.0);
  std::shared_ptr<const OpenVdbVolume> held;
  for (int repeat = 0; repeat < 8; ++repeat) {
    auto sequence = std::make_unique<OpenVdbVolumeSequence>(
        discoverOpenVdbVolumeSequence(directory, 24), OpenVdbVolumeOptions{.maxResolution = 16});
    auto async = std::make_unique<OpenVdbAsyncVolumeSequence>(std::move(sequence),
        OpenVdbAsyncSequenceOptions{.maxCachedFrames = 1, .maxResidentFrames = 1, .maxResidentBytes = 16384});
    (void)async->request(0);
    auto first = waitFrame(*async);
    ASSERT_TRUE(first.has_value());
    held = std::move(first->volume);
    ASSERT_TRUE(held);
    (void)async->request(1);
    ASSERT_TRUE(waitFor([&] { return async->status().state == OpenVdbAsyncSequenceState::WaitingForMemory; }));
    async.reset(); // Must stop/join while an external lease still uses the old budget.
    EXPECT_FLOAT_EQ(held->maxDensity, 1.0f);
    held.reset();
  }
}

TEST_F(OpenVdbSequenceFileTest, AsyncPrefetchFailureIsLatchedThenReportedForItsForegroundRequest) {
  write("frame1.vdb", 1.0f, 0.0);
  touch("frame2.vdb");
  auto sequence = std::make_unique<OpenVdbVolumeSequence>(
      discoverOpenVdbVolumeSequence(directory, 24), OpenVdbVolumeOptions{.maxResolution = 16});
  OpenVdbAsyncVolumeSequence async(std::move(sequence));
  (void)async.request(0);
  auto first = waitFrame(async);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first->volume);
  first.reset();
  async.prefetch(1);
  ASSERT_TRUE(waitFor([&] { return !async.status().prefetchError.empty(); }));
  const auto error = async.status().prefetchError;
  write("frame2.vdb", 2.0f, 0.0);
  for (int repeat = 0; repeat < 50; ++repeat) async.prefetch(1);
  EXPECT_EQ(async.status().prefetchError, error);
  (void)async.request(1);
  auto failed = waitFrame(async);
  ASSERT_TRUE(failed.has_value());
  EXPECT_FALSE(failed->volume);
  EXPECT_EQ(failed->frameIndex, 1u);
  EXPECT_NE(failed->error.find("frame2.vdb"), std::string::npos);
  // An explicit new foreground request after consuming the failure may retry.
  failed.reset();
  (void)async.request(1);
  auto retried = waitFrame(async);
  ASSERT_TRUE(retried.has_value());
  ASSERT_TRUE(retried->volume) << retried->error;
  EXPECT_FLOAT_EQ(retried->volume->maxDensity, 2.0f);
}

#if defined(CONTAINER_HAS_NANOVDB) && CONTAINER_HAS_NANOVDB
TEST_F(OpenVdbSequenceFileTest, AsyncNativePayloadLeaseCountsTheSerializedArrays) {
  write("frame1.vdb", 1.0f, 0.0, "density", true);
  auto sequence = std::make_unique<OpenVdbVolumeSequence>(
      discoverOpenVdbVolumeSequence(directory, 24),
      OpenVdbVolumeOptions{.maxResolution = 16, .maxInputBytes = 64ull * 1024 * 1024,
                          .temperatureGridName = "temperature", .sparse = true},
      OpenVdbSequenceCacheOptions{.maxCachedFrames = 2, .maxCachedBytes = 128ull * 1024 * 1024});
  OpenVdbAsyncVolumeSequence async(std::move(sequence));
  (void)async.request(0);
  auto completion = waitFrame(async);
  ASSERT_TRUE(completion.has_value());
  ASSERT_TRUE(completion->volume) << completion->error;
  const auto& volume = *completion->volume;
  ASSERT_FALSE(volume.sparseDensity.empty());
  ASSERT_FALSE(volume.sparseTemperature.empty());
  EXPECT_TRUE(volume.density.empty());
  const uint64_t bytes = uint64_t(volume.sparseDensity.capacity() +
                                  volume.sparseTemperature.capacity()) * sizeof(uint32_t);
  EXPECT_EQ(async.status().residentBytes, bytes);
  EXPECT_EQ(async.status().residentFrames, 1u);
}

TEST_F(OpenVdbSequenceFileTest, NativeDensityAndTemperaturePayloadsAreCountedInTheLru) {
  write("frame1.vdb", 1.0f, 0.0, "density", true);
  write("frame2.vdb", 2.0f, 0.0, "density", true);
  write("frame3.vdb", 3.0f, 0.0, "density", true);
  OpenVdbVolumeSequence sequence(discoverOpenVdbVolumeSequence(directory, 24),
      {.maxResolution = 16, .maxInputBytes = 64ull * 1024 * 1024,
       .temperatureGridName = "temperature", .sparse = true},
      {.maxCachedFrames = 2, .maxCachedBytes = 128ull * 1024 * 1024});
  const auto& first = sequence.frame(0);
  EXPECT_TRUE(first.density.empty());
  EXPECT_TRUE(first.temperature.empty());
  ASSERT_FALSE(first.sparseDensity.empty());
  ASSERT_FALSE(first.sparseTemperature.empty());
  const uint64_t firstBytes = uint64_t(first.sparseDensity.capacity() +
                                      first.sparseTemperature.capacity()) * sizeof(uint32_t);
  EXPECT_EQ(sequence.cachedBytes(), firstBytes);
  (void)sequence.frame(1);
  EXPECT_EQ(sequence.cachedFrameCount(), 2u);
  EXPECT_GT(sequence.cachedBytes(), firstBytes);
  (void)sequence.frame(0);
  (void)sequence.frame(2);
  EXPECT_TRUE(sequence.isCached(0));
  EXPECT_FALSE(sequence.isCached(1));
  EXPECT_TRUE(sequence.isCached(2));
  EXPECT_LE(sequence.cachedBytes(), 128ull * 1024 * 1024);
}
#endif
#endif
} // namespace
