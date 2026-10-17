// Opt-in CPU microbenchmark; intentionally excluded from CTest. Build the same
// source against captured pre-change provider headers with
// CONTAINER_BIM_CACHE_BASELINE to measure the production copy/build path.
// Input generation, cache installation, warmup, sample storage and JSON output
// are outside the measured calls. Allocation instrumentation is enabled only
// for a separate warmed pass, so allocator counters do not distort timings.
#include "Container/renderer/core/RenderExtraction.h"
#include "Container/renderer/scene/SceneProviderSynchronizer.h"
#include "Container/scene/MeshSceneProviderAssetAdapter.h"

#ifndef CONTAINER_BIM_CACHE_BASELINE
#include "Container/renderer/bim/BimStatisticsCache.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace allocation_tracking {
thread_local bool enabled = false;
thread_local std::uint64_t calls = 0;
thread_local std::uint64_t bytes = 0;

void record(std::size_t size) noexcept {
  if (enabled) {
    ++calls;
    bytes += size;
  }
}

void *allocate(std::size_t size) {
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    record(size);
    return memory;
  }
  throw std::bad_alloc();
}

void *allocateAligned(std::size_t size, std::size_t alignment) {
#ifdef _WIN32
  void *memory = _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
  void *memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? 1 : size) != 0) {
    memory = nullptr;
  }
#endif
  if (!memory)
    throw std::bad_alloc();
  record(size);
  return memory;
}

void freeAligned(void *memory) noexcept {
#ifdef _WIN32
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}
} // namespace allocation_tracking

void *operator new(std::size_t size) {
  return allocation_tracking::allocate(size);
}
void *operator new[](std::size_t size) {
  return allocation_tracking::allocate(size);
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}
void *operator new(std::size_t size, std::align_val_t alignment) {
  return allocation_tracking::allocateAligned(
      size, static_cast<std::size_t>(alignment));
}
void *operator new[](std::size_t size, std::align_val_t alignment) {
  return allocation_tracking::allocateAligned(
      size, static_cast<std::size_t>(alignment));
}
void operator delete(void *memory, std::align_val_t) noexcept {
  allocation_tracking::freeAligned(memory);
}
void operator delete[](void *memory, std::align_val_t) noexcept {
  allocation_tracking::freeAligned(memory);
}
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
  allocation_tracking::freeAligned(memory);
}
void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept {
  allocation_tracking::freeAligned(memory);
}
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
void *operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size, alignment);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *memory, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  allocation_tracking::freeAligned(memory);
}
void operator delete[](void *memory, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  allocation_tracking::freeAligned(memory);
}

namespace {
namespace scene = container::scene;
namespace renderer = container::renderer;
using Clock = std::chrono::steady_clock;
volatile std::uint64_t checksumSink = 0;

void consume(std::uint64_t checksum) { checksumSink = checksumSink ^ checksum; }

class AllocationScope {
public:
  AllocationScope() {
    allocation_tracking::calls = 0;
    allocation_tracking::bytes = 0;
    allocation_tracking::enabled = true;
  }
  ~AllocationScope() { allocation_tracking::enabled = false; }
  AllocationScope(const AllocationScope &) = delete;
  AllocationScope &operator=(const AllocationScope &) = delete;
};

struct Measurement {
  std::string_view path;
  std::size_t elements = 0;
  std::size_t iterations = 0;
  std::size_t allocationIterations = 0;
  std::array<double, 7> microsecondsPerCall{};
  std::uint64_t allocationCalls = 0;
  std::uint64_t allocatedBytes = 0;
};

template <class Callable>
Measurement measure(std::string_view path, std::size_t count, Callable &&call) {
  for (std::size_t i = 0; i < 8; ++i)
    consume(call());
  const auto calibrationStart = Clock::now();
  for (std::size_t i = 0; i < 8; ++i)
    consume(call());
  const double calibrationUs =
      std::chrono::duration<double, std::micro>(Clock::now() - calibrationStart)
          .count() /
      8;
  // Keep each warmed repeat around 3 ms for stable small-scene measurements,
  // while bounding expensive Debug baseline runs and total memory traffic.
  Measurement result{.path = path, .elements = count};
  result.iterations = static_cast<std::size_t>(std::clamp(
      std::ceil(3000.0 / std::max(calibrationUs, 0.001)), 8.0, 8192.0));
  for (double &sample : result.microsecondsPerCall) {
    const auto start = Clock::now();
    for (std::size_t i = 0; i < result.iterations; ++i)
      consume(call());
    sample = std::chrono::duration<double, std::micro>(Clock::now() - start)
                 .count() /
             static_cast<double>(result.iterations);
  }
  result.allocationIterations = std::min<std::size_t>(result.iterations, 256);
  {
    const AllocationScope scope;
    for (std::size_t i = 0; i < result.allocationIterations; ++i)
      consume(call());
    result.allocationCalls = allocation_tracking::calls;
    result.allocatedBytes = allocation_tracking::bytes;
  }
  return result;
}

class ProviderFixture {
public:
  explicit ProviderFixture(std::size_t count) : primitives_(count) {
    std::vector<scene::SceneProviderTriangleBatch> batches;
    batches.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      primitives_[i] = {.firstIndex = static_cast<std::uint32_t>(i * 3),
                        .indexCount = 3,
                        .materialIndex = static_cast<std::int32_t>(i % 16)};
      batches.push_back({.firstIndex = primitives_[i].firstIndex,
                         .indexCount = 3,
                         .materialIndex = primitives_[i].materialIndex,
                         .doubleSided = i % 7 == 0,
                         .transparent = i % 4 == 0});
    }
    batches_ = std::move(batches);
  }

  std::uint64_t meshFrame() {
    const scene::MeshSceneProviderAssetAdapterInput input{
        .primitiveRanges = primitives_,
        .materialCount = 16,
        .instanceCount = primitives_.size(),
        .bounds = {.min = glm::vec3(-1), .max = glm::vec3(1), .valid = true},
        .materialFactsAt =
            [](std::uint32_t index) {
              return scene::MeshSceneProviderMaterialFacts{
                  .transparent = index % 4 == 0, .doubleSided = index % 7 == 0};
            },
    };
    ++instanceRevision_;
#ifdef CONTAINER_BIM_CACHE_BASELINE
    const scene::MeshSceneAsset asset =
        scene::buildMeshSceneProviderAsset(input);
#else
    const scene::MeshSceneAsset &asset = meshCache_.get(
        input, {.geometry = 1, .materials = 1, .instances = instanceRevision_},
        this);
#endif
    renderer::SceneProviderSyncInput sync;
    sync.mesh = {
        .available = true,
        .primitiveCount = asset.primitiveCount,
        .materialCount = asset.materialCount,
        .instanceCount = asset.instanceCount,
        .triangleBatches = asset.triangleBatches,
        .bounds = asset.bounds,
        .geometryRevision = 1,
        .materialRevision = 1,
        .instanceRevision = instanceRevision_,
        .displayName = "Synthetic mesh",
    };
    synchronizer_.sync(registry_, sync);
    return extractionChecksum();
  }

  std::uint64_t bimProviderFrame() {
    renderer::SceneProviderSyncInput sync;
    sync.bim = {
        .available = true,
        .elementCount = primitives_.size(),
        .meshPrimitiveCount = primitives_.size(),
        .meshOpaqueBatchCount =
            primitives_.size() - (primitives_.size() + 3) / 4,
        .meshTransparentBatchCount = (primitives_.size() + 3) / 4,
        .triangleBatches = batches_,
        .bounds = {.min = glm::vec3(-1), .max = glm::vec3(1), .valid = true},
        .geometryRevision = 1,
        .materialRevision = 1,
        .instanceRevision = ++instanceRevision_,
        .displayName = "Synthetic BIM",
    };
    synchronizer_.sync(registry_, sync);
    return extractionChecksum();
  }

private:
  std::uint64_t extractionChecksum() const {
    const auto extraction =
        renderer::extractProviderSceneFrameInputs(registry_);
    const auto &snapshot = extraction.snapshots.front();
    const auto &ray = extraction.rayTracingBuildInputs.front();
    return snapshot.primitiveCount + ray.triangleGeometryCount +
           ray.triangleBatches[ray.triangleBatches.size() - 1].firstIndex +
           snapshot.revision.instances;
  }

  std::vector<container::geometry::PrimitiveRange> primitives_;
  decltype(scene::BimSceneAsset{}.triangleBatches) batches_;
#ifndef CONTAINER_BIM_CACHE_BASELINE
  scene::MeshSceneProviderAssetCache meshCache_;
#endif
  renderer::SceneProviderSynchronizer synchronizer_;
  scene::SceneProviderRegistry registry_;
  std::uint64_t instanceRevision_ = 0;
};

#ifndef CONTAINER_BIM_CACHE_BASELINE
class StatisticsFixture {
public:
  explicit StatisticsFixture(std::size_t count)
      : metadata_(count), objects_(count), commands_(count), lod_(count) {
    for (std::size_t i = 0; i < count; ++i) {
      const auto index = static_cast<std::uint32_t>(i);
      metadata_[i].objectIndex = index;
      metadata_[i].geometryKind = renderer::BimGeometryKind::Mesh;
      metadata_[i].materialIndex = index % 16;
      commands_[i].objectIndex = index;
      commands_[i].firstIndex = index * 3;
      commands_[i].indexCount = 3;
      lod_[i].objectIndex = index;
      lod_[i].clusterCount = 2;
      lod_[i].maxLodLevel = 3;
    }
    cache_.rebuild({.opaqueDrawCount = count}, metadata_, objects_, commands_,
                   {});
    inputs_.revision = 1;
    inputs_.objectCount = count;
    inputs_.metadata = metadata_;
    inputs_.opaqueDrawCommands = &commands_;
    filter_.drawBudgetEnabled = true;
    filter_.drawBudgetMaxObjects = 1024;
    // Cache-hit queries still overlay changing live residency every frame.
    residency_.computeReady = true;
    residency_.residentObjectCount = count;
  }

  std::uint64_t sceneStatsAndTopology() {
    residency_.dispatchPending = !residency_.dispatchPending;
    const auto stats = cache_.sceneStats(residency_, compaction_);
    const auto &batches = cache_.triangleBatches();
    return stats.objectCount + stats.meshletGpuResidentObjectCount +
           batches.size() + batches[batches.size() - 1].firstIndex;
  }

  std::uint64_t budgetStats() {
    const auto stats = cache_.drawBudgetLodStats(filter_, inputs_, lod_, 1, 1);
    return stats.visibleObjectCount + stats.visibleMeshletClusterReferences +
           stats.visibleMaxLodLevel;
  }

private:
  std::vector<renderer::BimElementMetadata> metadata_;
  std::vector<container::gpu::ObjectData> objects_;
  std::vector<renderer::DrawCommand> commands_;
  std::vector<renderer::BimObjectLodStreamingMetadata> lod_;
  renderer::BimStatisticsCache cache_;
  renderer::BimDrawFilterStateInputs inputs_;
  renderer::BimDrawFilter filter_;
  renderer::BimMeshletResidencyStats residency_;
  std::array<renderer::BimDrawCompactionStats, 2> compaction_{};
};
#endif

void printMeasurement(const Measurement &result, bool first) {
  auto sorted = result.microsecondsPerCall;
  std::sort(sorted.begin(), sorted.end());
  std::cout << (first ? "\n" : ",\n") << "    {\"path\":\"" << result.path
            << "\",\"elements\":" << result.elements
            << ",\"iterations_per_repeat\":" << result.iterations
            << ",\"allocation_iterations\":" << result.allocationIterations
            << ",\"median_us_per_call\":" << sorted[sorted.size() / 2]
            << ",\"allocations_per_call\":"
            << static_cast<double>(result.allocationCalls) /
                   result.allocationIterations
            << ",\"allocated_bytes_per_call\":"
            << static_cast<double>(result.allocatedBytes) /
                   result.allocationIterations
            << ",\"repeat_us_per_call\":[";
  for (std::size_t i = 0; i < result.microsecondsPerCall.size(); ++i) {
    if (i)
      std::cout << ',';
    std::cout << result.microsecondsPerCall[i];
  }
  std::cout << "]}";
}
} // namespace

int main() {
#ifdef CONTAINER_BIM_CACHE_BASELINE
  constexpr std::string_view implementation = "before";
#else
  constexpr std::string_view implementation = "after";
#endif
#ifdef CONTAINER_BIM_BENCHMARK_CONFIG
  constexpr std::string_view configuration = CONTAINER_BIM_BENCHMARK_CONFIG;
#elif defined(_DEBUG)
  constexpr std::string_view configuration = "Debug";
#else
  constexpr std::string_view configuration = "Release";
#endif
  std::cout
      << std::fixed << std::setprecision(6)
      << "{\n  \"schema_version\":1,\n  \"implementation\":\"" << implementation
      << "\",\n  \"configuration\":\"" << configuration
      << "\",\n  \"clock\":\"steady_clock\",\n  \"warmup_calls\":8,\n"
      << "  \"repeats\":7,\n  \"timing_allocation_counters_enabled\":false,\n"
      << "  \"notes\":[\"Synthetic immutable inputs; cache installation "
         "excluded.\","
      << "\"Provider paths run production synchronization, snapshots and ray "
         "extraction.\","
      << "\"BIM provider path excludes manager topology construction and count "
         "scans.\","
      << "\"Historical manager statistics baseline requires native application "
         "timing.\"],\n"
      << "  \"measurements\":[";
  bool first = true;
  for (const std::size_t count : {10u, 8790u, 100000u}) {
    ProviderFixture providers(count);
    printMeasurement(measure("mesh_provider_frame", count,
                             [&] { return providers.meshFrame(); }),
                     first);
    first = false;
    printMeasurement(measure("bim_provider_frame", count,
                             [&] { return providers.bimProviderFrame(); }),
                     false);
#ifndef CONTAINER_BIM_CACHE_BASELINE
    StatisticsFixture statistics(count);
    printMeasurement(
        measure("bim_statistics_and_topology", count,
                [&] { return statistics.sceneStatsAndTopology(); }),
        false);
    printMeasurement(measure("bim_budget_statistics_cache_hit", count,
                             [&] { return statistics.budgetStats(); }),
                     false);
#endif
  }
  std::cout << "\n  ],\n  \"checksum\":" << checksumSink << "\n}\n";
  return 0;
}
