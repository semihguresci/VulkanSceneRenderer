#include "Container/geometry/GeometryLodCache.h"

#include "Container/utility/ContentFingerprint.h"
#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace container::geometry {
namespace {

constexpr uint32_t formatVersion = 1;
// Bump when changing collapse decisions, derived range order, or error values.
constexpr uint32_t algorithmVersion = 1;
constexpr size_t fingerprintChunkElements = 1024;
constexpr size_t copyChunkIndices = 65'536;
constexpr uint32_t maxReducedLevels = 2;
using Clock = std::chrono::steady_clock;

double millisecondsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void appendU32(std::string& bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes.push_back(static_cast<char>(value >> shift));
}

void appendU64(std::string& bytes, uint64_t value) {
  appendU32(bytes, static_cast<uint32_t>(value));
  appendU32(bytes, static_cast<uint32_t>(value >> 32));
}

std::string fingerprint(std::string_view bytes, std::stop_token stop) {
  return util::FingerprintContent(
      std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()), stop).digest;
}

template<class T, class Encode>
void appendStreamDigest(std::string& key, std::span<const T> stream,
                        Encode encode, std::stop_token stop) {
  appendU64(key, stream.size());
  std::string chunk;
  while (!stream.empty()) {
    const auto part = stream.first(std::min(stream.size(), fingerprintChunkElements));
    for (const auto& value : part)
      encode(chunk, value);
    key += fingerprint(chunk, stop);
    chunk.clear();
    stream = stream.subspan(part.size());
  }
}

std::string cacheKey(std::span<const Vertex> vertices, std::span<const uint32_t> indices,
                     std::span<const PrimitiveRange> primitives,
                     GeometryLodPreparationOptions options, std::stop_token stop) {
  std::string fields{"container-geometry-lod"};
  appendU32(fields, formatVersion);
  appendU32(fields, algorithmVersion);
  appendU32(fields, options.maxBufferIndexCount);
  appendU32(fields, options.sourceIndexBudget);
  appendU32(fields, static_cast<uint32_t>(options.weldPolicy));
  appendStreamDigest(fields, vertices, [](std::string& bytes, const Vertex& vertex) {
    const auto vector = [&bytes](const auto& values) {
      for (glm::length_t i = 0; i < values.length(); ++i)
        appendU32(bytes, std::bit_cast<uint32_t>(values[i]));
    };
    vector(vertex.position);
    vector(vertex.color);
    vector(vertex.texCoord);
    vector(vertex.texCoord1);
    vector(vertex.normal);
    vector(vertex.tangent);
    appendU32(bytes, std::bit_cast<uint32_t>(vertex.colorAlpha));
  }, stop);
  appendStreamDigest(fields, indices, appendU32, stop);
  appendStreamDigest(fields, primitives, [](std::string& bytes, const PrimitiveRange& range) {
    appendU32(bytes, range.firstIndex);
    appendU32(bytes, range.indexCount);
    appendU32(bytes, static_cast<uint32_t>(range.materialIndex));
    appendU32(bytes, range.disableBackfaceCulling ? 1u : 0u);
  }, stop);
  return fingerprint(fields, stop);
}

void appendIndices(std::vector<uint32_t>& target, std::span<const uint32_t> input,
                   std::stop_token stop) {
  while (!input.empty()) {
    util::checkModelImportCancellation(stop);
    const auto part = input.first(std::min(input.size(), copyChunkIndices));
    target.insert(target.end(), part.begin(), part.end());
    input = input.subspan(part.size());
  }
}

bool fitsSource(const PrimitiveRange& range, size_t count) {
  return range.firstIndex <= count && range.indexCount <= count - range.firstIndex;
}

PreparedGeometryLods generate(std::span<const Vertex> vertices, std::span<const uint32_t> source,
                             std::span<const PrimitiveRange> primitives,
                             GeometryLodPreparationOptions options, std::stop_token stop) {
  PreparedGeometryLods result;
  result.ranges.resize(primitives.size());
  auto budget = options.sourceIndexBudget;
  for (size_t i = 0; i < primitives.size(); ++i) {
    util::checkModelImportCancellation(stop);
    const auto& primitive = primitives[i];
    if (!fitsSource(primitive, source.size()) || primitive.indexCount > budget)
      continue;
    budget -= primitive.indexCount;
    const auto levels = buildTriangleLods(vertices,
        source.subspan(primitive.firstIndex, primitive.indexCount), stop, options.weldPolicy);
    for (const auto& level : levels) {
      const auto offset = result.indices.empty() ? source.size() : result.indices.size();
      if (level.indices.size() > options.maxBufferIndexCount - offset)
        break;
      if (result.indices.empty()) {
        result.indices.reserve(source.size() + std::min({source.size(), size_t(options.sourceIndexBudget),
                                                         options.maxBufferIndexCount - source.size()}));
        appendIndices(result.indices, source, stop);
      }
      auto& range = result.ranges[i];
      range.firstIndex = primitive.firstIndex;
      range.indexCount = primitive.indexCount;
      range.levels.push_back({static_cast<uint32_t>(offset),
                              static_cast<uint32_t>(level.indices.size()), level.maxError});
      appendIndices(result.indices, level.indices, stop);
    }
  }
  util::checkModelImportCancellation(stop);
  return result;
}

uint64_t maxTailCount(size_t sourceSize, GeometryLodPreparationOptions options) {
  return std::min<uint64_t>(options.sourceIndexBudget, options.maxBufferIndexCount - sourceSize);
}

uint64_t maxPayloadSize(size_t sourceCount, size_t primitiveCount, GeometryLodPreparationOptions options) {
  constexpr uint64_t countBytes = 2 * sizeof(uint32_t);
  constexpr uint64_t recordBytes = (2 + maxReducedLevels * 2) * sizeof(uint32_t);
  return countBytes + recordBytes * std::min<uint64_t>(primitiveCount, options.sourceIndexBudget / 3) +
         sizeof(uint32_t) * maxTailCount(sourceCount, options);
}

std::string encode(const PreparedGeometryLods& result, size_t sourceCount, std::stop_token stop) {
  const auto tailCount = result.indices.empty() ? 0u : result.indices.size() - sourceCount;
  const auto rangeCount = std::ranges::count_if(result.ranges,
      [](const auto& range) { return !range.levels.empty(); });
  std::string payload;
  payload.reserve(8 + 24 * size_t(rangeCount) + sizeof(uint32_t) * tailCount);
  appendU32(payload, static_cast<uint32_t>(rangeCount));
  appendU32(payload, static_cast<uint32_t>(tailCount));
  for (size_t i = 0; i < result.ranges.size(); ++i) {
    const auto& range = result.ranges[i];
    if (range.levels.empty())
      continue;
    util::checkModelImportCancellation(stop);
    appendU32(payload, static_cast<uint32_t>(i));
    appendU32(payload, static_cast<uint32_t>(range.levels.size()));
    for (const auto& level : range.levels) {
      appendU32(payload, level.indexCount);
      appendU32(payload, std::bit_cast<uint32_t>(level.maxError));
      for (const auto index : std::span(result.indices).subspan(level.firstIndex, level.indexCount))
        appendU32(payload, index);
    }
  }
  return payload;
}

class PayloadReader {
 public:
  explicit PayloadReader(std::string_view bytes) : bytes_(bytes) {}

  bool read(uint32_t& value) {
    if (bytes_.size() < sizeof(value))
      return false;
    value = 0;
    for (size_t i = 0; i < sizeof(value); ++i)
      value |= uint32_t(static_cast<unsigned char>(bytes_[i])) << (i * 8);
    bytes_.remove_prefix(sizeof(value));
    return true;
  }
  [[nodiscard]] size_t remaining() const { return bytes_.size(); }

 private:
  std::string_view bytes_;
};

std::optional<PreparedGeometryLods> decode(std::string_view payload, size_t vertexCount,
    std::span<const uint32_t> source, std::span<const PrimitiveRange> primitives,
    GeometryLodPreparationOptions options, std::stop_token stop) {
  PayloadReader reader(payload);
  uint32_t rangeCount{}, tailCount{};
  if (!reader.read(rangeCount) || !reader.read(tailCount) ||
      rangeCount > primitives.size() || tailCount > maxTailCount(source.size(), options) ||
      tailCount > reader.remaining() / sizeof(uint32_t) ||
      (rangeCount == 0) != (tailCount == 0))
    return {};

  PreparedGeometryLods result;
  result.ranges.resize(primitives.size());
  if (tailCount) {
    result.indices.reserve(source.size() + tailCount);
    appendIndices(result.indices, source, stop);
  }
  size_t accounted = 0;
  uint32_t sourceBudget = options.sourceIndexBudget;
  for (uint32_t i = 0; i < rangeCount; ++i) {
    util::checkModelImportCancellation(stop);
    uint32_t primitiveId{}, levelCount{};
    if (!reader.read(primitiveId) || !reader.read(levelCount) ||
        primitiveId < accounted || primitiveId >= primitives.size() ||
        levelCount == 0 || levelCount > maxReducedLevels)
      return {};

    // Apply the original generation budget in primitive order, including
    // ranges that were considered but yielded no reduced level.
    for (; accounted <= primitiveId; ++accounted) {
      const auto& candidate = primitives[accounted];
      if (!fitsSource(candidate, source.size()) || candidate.indexCount > sourceBudget) {
        if (accounted == primitiveId)
          return {};
        continue;
      }
      sourceBudget -= candidate.indexCount;
    }
    const auto& primitive = primitives[primitiveId];
    const auto indices = source.subspan(primitive.firstIndex, primitive.indexCount);
    std::vector<uint32_t> domain(indices.begin(), indices.end());
    std::ranges::sort(domain);
    auto& range = result.ranges[primitiveId];
    range.firstIndex = primitive.firstIndex;
    range.indexCount = primitive.indexCount;
    auto previousCount = primitive.indexCount;
    float previousError = 0.0f;
    for (uint32_t level = 0; level < levelCount; ++level) {
      uint32_t count{}, bits{};
      if (!reader.read(count) || !reader.read(bits) || count == 0 || count % 3 ||
          count >= previousCount || count > tailCount || count > reader.remaining() / sizeof(uint32_t))
        return {};
      const auto error = std::bit_cast<float>(bits);
      if (!std::isfinite(error) || error <= 0.0f || error < previousError)
        return {};
      range.levels.push_back({static_cast<uint32_t>(result.indices.size()), count, error});
      for (uint32_t triangle = 0; triangle < count / 3; ++triangle) {
        if (triangle % 1024 == 0)
          util::checkModelImportCancellation(stop);
        uint32_t a{}, b{}, c{};
        if (!reader.read(a) || !reader.read(b) || !reader.read(c) ||
            a == b || b == c || c == a)
          return {};
        for (auto index : {a, b, c}) {
          if (index >= vertexCount || !std::ranges::binary_search(domain, index))
            return {};
          result.indices.push_back(index);
        }
      }
      previousCount = count;
      previousError = error;
      tailCount -= count;
    }
  }
  if (tailCount || reader.remaining())
    return {};
  return result;
}

} // namespace

PreparedGeometryLods prepareGeometryLods(
    std::span<const Vertex> vertices, std::span<const uint32_t> source,
    std::span<const PrimitiveRange> primitives, const std::filesystem::path& cacheDirectory,
    GeometryLodPreparationOptions options, std::stop_token stop) {
  util::checkModelImportCancellation(stop);
  if (vertices.empty() || source.empty() || primitives.empty() || primitives.size() > UINT32_MAX ||
      source.size() >= options.maxBufferIndexCount || options.sourceIndexBudget == 0) {
    PreparedGeometryLods result;
    if (!cacheDirectory.empty())
      result.cache.status = util::DerivedAssetCacheStatus::NotCacheable;
    return result;
  }

  GeometryLodCacheStats cache;
  std::filesystem::path file;
  if (!cacheDirectory.empty()) {
    const auto keyStart = Clock::now();
    cache.key = cacheKey(vertices, source, primitives, options, stop);
    cache.keyMilliseconds = millisecondsSince(keyStart);
    file = cacheDirectory / "geometry-lod" / (cache.key + ".bin");
    const auto readStart = Clock::now();
    const auto record = util::readDerivedAssetCache(
        file, formatVersion, cache.key, maxPayloadSize(source.size(), primitives.size(), options), stop);
    cache.status = record.status;
    cache.bytesRead = record.bytesRead;
    if (record.status == util::DerivedAssetCacheStatus::Hit) {
      if (auto result = decode(record.payload, vertices.size(), source, primitives, options, stop)) {
        cache.readMilliseconds = millisecondsSince(readStart);
        result->cache = std::move(cache);
        return std::move(*result);
      }
      cache.status = util::DerivedAssetCacheStatus::Corrupt;
    }
    cache.readMilliseconds = millisecondsSince(readStart);
  }

  const auto generationStart = Clock::now();
  auto result = generate(vertices, source, primitives, options, stop);
  cache.generationMilliseconds = millisecondsSince(generationStart);
  if (!file.empty()) {
    const auto writeStart = Clock::now();
    cache.bytesWritten = util::writeDerivedAssetCache(
        file, formatVersion, cache.key, encode(result, source.size(), stop), stop);
    cache.writeMilliseconds = millisecondsSince(writeStart);
  }
  result.cache = std::move(cache);
  return result;
}

} // namespace container::geometry
