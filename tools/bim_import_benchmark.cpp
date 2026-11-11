#include "Container/renderer/bim/BimManager.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <bit>
#include <charconv>
#include <chrono>
#include <concepts>
#include <iostream>
#include <stdexcept>
#include <type_traits>

#ifdef _WIN32
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#ifdef CONTAINER_BIM_IMPORT_COUNT_ALLOCATIONS
void readBimImportAllocationCounters(size_t &count, size_t &bytes) noexcept;
#endif

namespace {

using container::renderer::BimManager;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// FNV-1a over length-prefixed text/sequences and fixed-width little-endian
// values. Hash semantic fields explicitly so paths, pointers, and padding
// never affect equality. The digest runs after all measurements.
class Digest {
public:
  template <std::integral T> void add(T value) {
    auto bits = static_cast<uint64_t>(value);
    for (size_t i = 0; i < sizeof(bits); ++i) {
      byte(static_cast<uint8_t>(bits));
      bits >>= 8;
    }
  }

  template <typename T> requires std::is_enum_v<T>
  void add(T value) { add(std::to_underlying(value)); }

  void add(float value) { add(std::bit_cast<uint32_t>(value)); }
  void add(double value) { add(std::bit_cast<uint64_t>(value)); }

  void add(std::string_view value) {
    add(value.size());
    for (const auto ch : value)
      byte(static_cast<uint8_t>(ch));
  }

  template <glm::length_t N, typename T, glm::qualifier Q>
  void add(const glm::vec<N, T, Q> &value) {
    for (glm::length_t i = 0; i < N; ++i)
      add(value[i]);
  }

  template <glm::length_t C, glm::length_t R, typename T, glm::qualifier Q>
  void add(const glm::mat<C, R, T, Q> &value) {
    for (glm::length_t i = 0; i < C; ++i)
      add(value[i]);
  }

  template <typename... T> void fields(const T &...values) {
    (add(values), ...);
  }

  template <typename Range, typename Append>
  void sequence(const Range &range, Append append) {
    add(range.size());
    for (const auto &entry : range)
      append(entry);
  }

  [[nodiscard]] std::string hex() const {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(16, '0');
    auto bits = value_;
    for (auto it = result.rbegin(); it != result.rend(); ++it) {
      *it = digits[bits & 0xf];
      bits >>= 4;
    }
    return result;
  }

private:
  void byte(uint8_t value) { value_ = (value_ ^ value) * 1099511628211ull; }

  uint64_t value_{14695981039346656037ull};
};

template <typename T> void appendProperties(Digest &hash, const T &properties) {
  hash.sequence(properties, [&](const auto &p) {
    hash.fields(p.set, p.name, p.value, p.category);
  });
}

template <typename T> void appendElementIdentity(Digest &hash, const T &row) {
  hash.fields(row.meshId, row.geometryKind, row.guid, row.type, row.displayName,
              row.objectType, row.storeyName, row.storeyId, row.materialName,
              row.materialCategory, row.discipline, row.phase, row.fireRating,
              row.loadBearing, row.status, row.sourceId, row.doubleSided);
  appendProperties(hash, row.properties);
}

void appendMesh(Digest &hash, std::span<const container::geometry::Vertex> vertices,
                std::span<const uint32_t> indices) {
  hash.sequence(vertices, [&](const auto &v) {
    hash.fields(v.position, v.color, v.texCoord, v.texCoord1, v.normal,
                v.tangent, v.colorAlpha);
  });
  hash.sequence(indices, [&](const auto i) { hash.add(i); });
}

Json semanticSnapshot(const BimManager::PreparedScene &scene) {
  const auto &model = scene.model;
  const auto &report = model.importReport;
  const auto &graph = scene.relationships();
  const auto rows = scene.metadata();

  Digest source;
  source.add(scene.format);
  source.sequence(model.elements, [&](const auto &row) {
    appendElementIdentity(source, row);
    source.fields(row.transform, row.color, row.materialIndex);
  });
  source.sequence(model.materials, [&](const auto &material) {
    const auto &m = material.pbr;
    source.fields(m.baseColor, m.emissiveColor, m.metallicFactor,
                  m.roughnessFactor, m.alphaCutoff, m.opacityFactor,
                  m.alphaMode, m.doubleSided);
  });
  const auto &unit = model.unitMetadata;
  source.fields(unit.hasSourceUnits, unit.sourceUnits, unit.hasMetersPerUnit,
                unit.metersPerUnit, unit.hasImportScale, unit.importScale,
                unit.hasEffectiveImportScale, unit.effectiveImportScale);
  const auto &geo = model.georeferenceMetadata;
  source.fields(geo.hasSourceUpAxis, geo.sourceUpAxis, geo.hasCoordinateOffset,
                geo.coordinateOffset, geo.coordinateOffsetSource, geo.crsName,
                geo.crsAuthority, geo.crsCode, geo.mapConversionName);

  Digest geometry;
  appendMesh(geometry, model.vertices, model.indices);
  const auto appendRanges = [&](const auto &ranges) {
    geometry.sequence(ranges, [&](const auto &r) {
      geometry.fields(r.meshId, r.firstIndex, r.indexCount, r.boundsCenter,
                      r.boundsRadius);
    });
  };
  appendRanges(model.meshRanges);
  appendRanges(model.nativePointRanges);
  appendRanges(model.nativeCurveRanges);
  geometry.sequence(scene.meshletClusters, [&](const auto &c) {
    geometry.fields(c.meshId, c.firstIndex, c.indexCount, c.triangleCount,
                    c.lodLevel, c.boundsCenter, c.boundsRadius, c.estimated);
  });
  const auto &optimized = scene.optimizedMetadata;
  geometry.fields(optimized.meshObjectCount, optimized.meshletClusterCount,
                  optimized.sourceMeshletClusterCount,
                  optimized.estimatedMeshletClusterCount, optimized.maxLodLevel);

  Digest upload;
  appendMesh(upload, scene.uploadVertices, scene.uploadIndices);
  for (const auto *overlay : {&scene.floorPlanGround, &scene.floorPlanSourceElevation}) {
    upload.fields(overlay->firstIndex, overlay->indexCount, overlay->objectIndex,
                  overlay->boundsCenter, overlay->boundsRadius);
    upload.sequence(overlay->drawCommands, [&](const auto &draw) {
      upload.fields(draw.objectIndex, draw.firstIndex, draw.indexCount,
                    draw.instanceCount);
    });
  }

  Digest metadata;
  size_t propertyCount = 0;
  metadata.sequence(rows, [&](const auto &row) {
    appendElementIdentity(metadata, row);
    const auto &b = row.bounds;
    metadata.fields(row.objectIndex, row.sourceElementIndex, row.sourceMaterialIndex,
                    row.materialIndex, row.semanticTypeId, row.productIdentityId,
                    row.sourceColor, row.transparent, b.valid, b.min, b.max,
                    b.center, b.size, b.radius, b.floorElevation);
    propertyCount += row.properties.size();
  });

  Digest relationships;
  relationships.sequence(model.relationships, [&](const auto &r) {
    relationships.fields(r.fromGuid, r.fromSourceId, r.toGuid, r.toSourceId,
                         r.kind, r.label);
  });
  relationships.sequence(graph.nodes(), [&](const auto &n) {
    relationships.fields(n.objectIndex, n.guid, n.sourceId, n.label, n.ifcClass);
  });
  relationships.sequence(graph.edges(), [&](const auto &e) {
    relationships.fields(e.from, e.to, e.kind, e.label);
  });
  size_t propertyGroupCount = 0;
  for (const auto &row : rows) {
    const auto groups = graph.propertySetsForObject(row.objectIndex);
    propertyGroupCount += groups.size();
    relationships.sequence(groups, [&](const auto &g) {
      relationships.fields(g.set, g.category);
      relationships.sequence(g.properties, [&](const auto &p) {
        relationships.fields(p.name, p.value, p.category);
      });
    });
  }

  Digest diagnostics;
  diagnostics.fields(report.completeness, report.sourceFormat, report.sourceItemName,
                     report.sourceProductCount, report.importedProductCount,
                     report.skippedProductCount, report.partialProductCount,
                     !report.fallbackSource.empty());
  diagnostics.sequence(report.representationWarnings, [&](const auto &entry) {
    diagnostics.fields(entry.first, entry.second);
  });
  diagnostics.sequence(report.diagnostics, [&](const auto &d) {
    auto reason = d.reason;
    // File-level fallback errors may embed the absolute requested filename.
    if (const auto pos = reason.find(scene.requestedPath); pos != std::string::npos)
      reason.replace(pos, scene.requestedPath.size(), "<input>");
    diagnostics.fields(d.representationType, d.entityId, d.productId,
                       d.productGuid, reason);
  });

  return {
      {"digest", {{"source", source.hex()}, {"geometry", geometry.hex()},
                  {"uploadAndOverlays", upload.hex()}, {"metadata", metadata.hex()},
                  {"relationships", relationships.hex()}, {"diagnostics", diagnostics.hex()}}},
      {"counts", {{"elements", model.elements.size()}, {"vertices", model.vertices.size()},
                  {"indices", model.indices.size()}, {"materials", model.materials.size()},
                  {"uploadVertices", scene.uploadVertices.size()},
                  {"uploadIndices", scene.uploadIndices.size()},
                  {"groundIndices", scene.floorPlanGround.indexCount},
                  {"sourceElevationIndices", scene.floorPlanSourceElevation.indexCount},
                  {"meshletClusters", scene.meshletClusters.size()}, {"metadataRows", rows.size()},
                  {"metadataProperties", propertyCount}, {"relationshipNodes", graph.nodes().size()},
                  {"relationshipEdges", graph.edges().size()}, {"propertyGroups", propertyGroupCount},
                  {"diagnostics", report.diagnostics.size()}, {"sourceProducts", report.sourceProductCount},
                  {"importedProducts", report.importedProductCount},
                  {"partialProducts", report.partialProductCount}, {"skippedProducts", report.skippedProductCount}}},
      {"format", scene.format}, {"sourceFormat", report.sourceFormat},
      {"completeness", container::geometry::importCompletenessName(report.completeness)},
      {"fallback", !report.fallbackSource.empty()},
  };
}

struct PeakMemory {
  uint64_t workingSetBytes{};
  uint64_t committedBytes{};
};

PeakMemory peakMemory() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    throw std::runtime_error("could not read process memory counters");
  return {counters.PeakWorkingSetSize, counters.PeakPagefileUsage};
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    throw std::runtime_error("could not read process resource usage");
#ifdef __APPLE__
  constexpr uint64_t rssUnitBytes = 1;
#else
  constexpr uint64_t rssUnitBytes = 1024;
#endif
  return {static_cast<uint64_t>(usage.ru_maxrss) * rssUnitBytes, 0};
#endif
}

Json memoryRecord(PeakMemory peak) {
  return {{"peakWorkingSetBytes", peak.workingSetBytes},
          {"peakCommitBytes", peak.committedBytes == 0 ? Json(nullptr) : Json(peak.committedBytes)}};
}

constexpr std::array<std::string_view, 7> stages{
    "parse", "meshlets", "optimizedMetadata", "geometryCopy", "floorPlans",
    "drawMetadata", "relationships"};

int run(const std::filesystem::path &path, unsigned iterations) {
  if (!std::filesystem::is_regular_file(path))
    throw std::runtime_error("model input must be a regular file");
  const auto input = container::util::pathToUtf8(std::filesystem::absolute(path));
  const auto initialPeak = peakMemory();
  Json samples = Json::array();
  Json semantics;
  std::string loadedPath;
  for (unsigned iteration = 0; iteration < iterations; ++iteration) {
    std::array<Clock::time_point, stages.size() + 1> boundaries{};
#ifdef CONTAINER_BIM_IMPORT_COUNT_ALLOCATIONS
    struct Allocations {
      size_t count{};
      size_t bytes{};
    };
    std::array<Allocations, stages.size() + 1> allocations{};
#endif
    size_t stage = 0;
    const auto onStage = [&](std::string_view name) {
      if (stage >= stages.size() || stages[stage] != name)
        throw std::runtime_error("unexpected BIM preparation stage: " + std::string(name));
#ifdef CONTAINER_BIM_IMPORT_COUNT_ALLOCATIONS
      readBimImportAllocationCounters(allocations[stage].count,
                                      allocations[stage].bytes);
#endif
      boundaries[stage++] = Clock::now();
    };
    const auto start = Clock::now();
    const auto scene = BimManager::prepareScene(input, 1.0f, {}, {}, {}, onStage);
    const auto end = Clock::now();
#ifdef CONTAINER_BIM_IMPORT_COUNT_ALLOCATIONS
    readBimImportAllocationCounters(allocations.back().count,
                                    allocations.back().bytes);
#endif
    const auto peak = peakMemory();
    if (stage != stages.size())
      throw std::runtime_error("missing BIM preparation stages");
    boundaries.back() = end;
    const auto milliseconds = [](auto duration) {
      return std::chrono::duration<double, std::milli>(duration).count();
    };
    Json timing;
    for (size_t i = 0; i < stages.size(); ++i)
      timing[stages[i]] = milliseconds(boundaries[i + 1] - boundaries[i]);
    samples.push_back({{"iteration", iteration + 1},
                       {"cacheState", iteration == 0 ? "fresh process" : "repeat in process"},
                       {"elapsedMs", milliseconds(end - start)},
                       {"stagesMs", std::move(timing)}, {"processMemory", memoryRecord(peak)}});
#ifdef CONTAINER_BIM_IMPORT_COUNT_ALLOCATIONS
    Json stagesAllocations;
    for (size_t i = 0; i < stages.size(); ++i) {
      const auto &begin = allocations[i];
      const auto &end = allocations[i + 1];
      stagesAllocations[stages[i]] = {{"count", end.count - begin.count},
                                    {"requestedBytes", end.bytes - begin.bytes}};
    }
    samples.back()["stagesAllocations"] = std::move(stagesAllocations);
#endif
    if (iteration + 1 == iterations) {
      semantics = semanticSnapshot(scene);
      loadedPath = container::util::pathToUtf8(scene.assetPath);
    }
  }

  const Json result{
      {"configuration", CONTAINER_BUILD_CONFIG}, {"compiler", CONTAINER_BUILD_COMPILER},
      {"revision", CONTAINER_GIT_REVISION}, {"input", input}, {"loadedInput", loadedPath},
      {"scale", 1.0f}, {"initialProcessMemory", memoryRecord(initialPeak)},
      {"samples", std::move(samples)}, {"semantics", std::move(semantics)},
  };
  std::cout << result.dump() << '\n';
  return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  try {
    if (argc < 2 || argc > 3)
      throw std::runtime_error("usage: BimImportBenchmark MODEL [ITERATIONS]");
    unsigned iterations = 1;
    if (argc == 3) {
      const auto text = container::util::pathToUtf8(argv[2]);
      const auto *end = text.data() + text.size();
      const auto parsed = std::from_chars(text.data(), end, iterations);
      if (parsed.ptr != end || parsed.ec != std::errc{} || iterations == 0)
        throw std::runtime_error("ITERATIONS must be a positive integer");
    }
    return run(argv[1], iterations);
  } catch (const std::exception &error) {
    std::cerr << "BimImportBenchmark: " << error.what() << '\n';
    return 1;
  }
}
