#pragma once

#include "Container/geometry/Model.h"
#include "Container/scene/MeshSceneProviderBuilder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace container::scene {

struct MeshSceneProviderMaterialFacts {
  bool transparent{false};
  bool doubleSided{false};
};

using MeshSceneProviderMaterialResolver =
    std::function<MeshSceneProviderMaterialFacts(uint32_t)>;

struct MeshSceneProviderBindingLimits {
  uint64_t maxObjects{1'000'000}, maxDraws{1'000'000}, maxInstances{1'000'000};
  // Logical detached/cache payload, including the old cache during replacement.
  // Allocator bookkeeping and external ObjectData/geometry storage are excluded.
  uint64_t maxOwnedBytes{256ull * 1024 * 1024}, maxWork{64'000'000};
};

struct MeshSceneProviderObjectBinding {
  uint32_t primitive{}, material{}, source{};
  bool doubleSided{};
};
struct MeshSceneProviderDrawBinding {
  uint32_t objectIndex{}, firstIndex{}, indexCount{}, instanceCount{1};
};
// Allocation-free, read-only projection over an owner's borrowed arrays. The
// renderer bridge supplies plain scalar projections; no resource/owner is kept.
template<class Row> struct MeshSceneProviderReadView {
  const void* data{};
  size_t count{};
  Row (*at)(const void*,size_t) noexcept{};
  [[nodiscard]] size_t size() const noexcept { return count; }
  [[nodiscard]] bool empty() const noexcept { return !count; }
  [[nodiscard]] Row operator[](size_t index) const noexcept { return at(data,index); }
  [[nodiscard]] Row back() const noexcept { return (*this)[count-1]; }
  [[nodiscard]] bool valid() const noexcept { return !count || (data && at); }
};

// A complete current/prospective primary-mesh object/draw view. Every real
// object must occur exactly once in these draws. A diagnostic object is only
// accepted as the final UINT32_MAX primitive/source row, without a mesh draw.
// indexCount describes the SAME geometry generation as these draw ranges.
struct MeshSceneProviderBindingView {
  MeshSceneProviderReadView<MeshSceneProviderObjectBinding> objects;
  MeshSceneProviderReadView<MeshSceneProviderDrawBinding> opaqueDraws, transparentDraws;
  uint64_t indexCount{};
  MeshSceneProviderBindingLimits limits{};
};

struct MeshSceneProviderAssetAdapterInput {
  std::span<const container::geometry::PrimitiveRange> primitiveRanges{};
  std::size_t materialCount{0};
  std::size_t instanceCount{0};
  SceneProviderBounds bounds{};
  MeshSceneProviderMaterialResolver materialFactsAt{};
  std::optional<MeshSceneProviderBindingView> bindings;
};

namespace mesh_provider_detail {
[[noreturn]] inline void invalid(const char* message) {
  throw std::invalid_argument(message);
}
struct BindingRow {
  uint32_t primitive{}, material{}, firstIndex{}, indexCount{}, drawOrdinal{};
  bool doubleSided{}, transparent{};
  bool operator==(const BindingRow&) const = default;
};
struct BindingShape { size_t realObjects{}; uint64_t ownedBytes{}, workUsed{}; };
inline void work(uint64_t& remaining, uint64_t count = 1) {
  if(count > remaining) invalid("Mesh provider binding traversal exceeds its work limit");
  remaining -= count;
}
inline BindingShape preflight(const MeshSceneProviderAssetAdapterInput& input,
                              uint64_t oldOwnedBytes = 0) {
  const auto& view = *input.bindings;
  const auto& limits = view.limits;
  if(!limits.maxObjects || !limits.maxDraws || !limits.maxInstances || !limits.maxOwnedBytes ||
      !limits.maxWork || limits.maxObjects == UINT64_MAX || limits.maxDraws == UINT64_MAX ||
      limits.maxInstances == UINT64_MAX || limits.maxOwnedBytes == UINT64_MAX || limits.maxWork == UINT64_MAX ||
      !view.objects.valid() || !view.opaqueDraws.valid() || !view.transparentDraws.valid() ||
      view.indexCount == UINT64_MAX || input.primitiveRanges.size() >= UINT32_MAX ||
      input.materialCount > uint64_t(INT32_MAX) || view.objects.size() > UINT32_MAX ||
      view.objects.size() > limits.maxObjects || view.opaqueDraws.size() > limits.maxDraws ||
      view.transparentDraws.size() > limits.maxDraws - view.opaqueDraws.size())
    invalid("Mesh provider binding inputs exceed their finite limits");
  const auto draws = view.opaqueDraws.size() + view.transparentDraws.size();
  if(draws > UINT32_MAX || input.primitiveRanges.size() > limits.maxObjects ||
      input.materialCount > limits.maxObjects || !input.materialFactsAt)
    invalid("Mesh provider binding identity tables are unavailable or excessive");
  const bool diagnostic = !view.objects.empty() && view.objects.back().primitive == UINT32_MAX;
  const size_t count = view.objects.size() - size_t(diagnostic);
  if(diagnostic && view.objects.back().source != UINT32_MAX)
    invalid("Mesh provider diagnostic row has an invalid source identity");
  if(count > limits.maxInstances)
    invalid("Mesh provider binding instance count exceeds its limit");
  constexpr uint64_t perRow = 2 * sizeof(BindingRow) + sizeof(SceneProviderTriangleBatch);
  constexpr uint64_t fixed = sizeof(MeshSceneAsset) + 3 * sizeof(std::vector<BindingRow>) + 128;
  if(oldOwnedBytes > limits.maxOwnedBytes || fixed > limits.maxOwnedBytes - oldOwnedBytes ||
      count > (limits.maxOwnedBytes - oldOwnedBytes - fixed) / perRow)
    invalid("Mesh provider binding storage exceeds its limit");
  uint64_t instances{};
  for(const auto drawList : {view.opaqueDraws, view.transparentDraws}) for(size_t d=0;d<drawList.size();++d) {
    const auto draw=drawList[d];
    if(!draw.instanceCount || draw.indexCount % 3 ||
        draw.objectIndex >= count || draw.instanceCount > count - draw.objectIndex ||
        uint64_t(draw.firstIndex) + draw.indexCount > view.indexCount ||
        draw.instanceCount > limits.maxInstances - instances)
      invalid("Mesh provider draw has an invalid object, triangle or instance range");
    instances += draw.instanceCount;
  }
  if(instances != count) invalid("Mesh provider draws do not cover every real object exactly once");
  // Preflight all scalar identities before allocating scratch. Actual sorting
  // comparisons and every later traversal also consume the finite work counter.
  uint64_t remaining = limits.maxWork;
  work(remaining, draws);
  for(size_t i = 0; i < count; ++i) {
    work(remaining);
    const auto& object = view.objects[i];
    if(object.primitive >= input.primitiveRanges.size() || object.material >= input.materialCount)
      invalid("Mesh provider object has an invalid primitive or material identity");
  }
  return {count, fixed + count * perRow, limits.maxWork - remaining};
}

inline bool matches(const MeshSceneProviderAssetAdapterInput& input,
                    std::span<const BindingRow> rows, const BindingShape& shape, uint64_t& remaining) {
  const auto& view = *input.bindings;
  if(rows.size() != shape.realObjects) return false;
  for(size_t i = 0; i < rows.size(); ++i) {
    work(remaining);
    const auto& object = view.objects[i];
    const auto& primitive = input.primitiveRanges[object.primitive];
    const auto facts = input.materialFactsAt(object.material);
    if(facts.transparent != rows[i].transparent ||
        ((facts.doubleSided || primitive.disableBackfaceCulling) && !rows[i].doubleSided) ||
        primitive.indexCount != rows[i].indexCount) return false;
    if(rows[i].primitive != object.primitive || rows[i].material != object.material ||
        rows[i].doubleSided != object.doubleSided) return false;
  }
  uint32_t ordinal{};
  for(unsigned transparent = 0; transparent < 2; ++transparent) {
    const auto draws = transparent ? view.transparentDraws : view.opaqueDraws;
    for(size_t d=0;d<draws.size();++d) {
      const auto draw=draws[d];
      work(remaining);
      for(uint32_t instance = 0; instance < draw.instanceCount; ++instance) {
        work(remaining);
        const auto& row = rows[size_t(draw.objectIndex) + instance];
        if(row.drawOrdinal != ordinal || row.firstIndex != draw.firstIndex || row.indexCount != draw.indexCount ||
            row.transparent != bool(transparent)) return false;
      }
      ++ordinal;
    }
  }
  return true;
}

struct BindingAsset { MeshSceneAsset asset; std::vector<BindingRow> rows; };
inline BindingAsset build(const MeshSceneProviderAssetAdapterInput& input, const BindingShape& shape,
                          uint64_t remaining) {
  const auto& view = *input.bindings;
  BindingAsset result;
  work(remaining, shape.realObjects);
  result.rows.resize(shape.realObjects);
  for(auto& row : result.rows) { work(remaining); row.drawOrdinal = UINT32_MAX; }
  uint32_t ordinal{};
  for(unsigned transparent = 0; transparent < 2; ++transparent) {
    const auto draws = transparent ? view.transparentDraws : view.opaqueDraws;
    for(size_t d=0;d<draws.size();++d) {
      const auto draw=draws[d];
      work(remaining);
      for(uint32_t instance = 0; instance < draw.instanceCount; ++instance) {
        work(remaining);
        const size_t id = size_t(draw.objectIndex) + instance;
        auto& row = result.rows[id];
        if(row.drawOrdinal != UINT32_MAX) invalid("Mesh provider draws contain a duplicate object binding");
        const auto& object = view.objects[id];
        const auto& primitive = input.primitiveRanges[object.primitive];
        const auto facts = input.materialFactsAt(object.material);
        const bool doubleSided = object.doubleSided;
        if(facts.transparent != bool(transparent) || ((facts.doubleSided || primitive.disableBackfaceCulling) && !doubleSided) ||
            draw.indexCount != primitive.indexCount)
          invalid("Mesh provider draw classification or primitive topology disagrees with its binding");
        row = {object.primitive, object.material, draw.firstIndex, draw.indexCount, ordinal,
            doubleSided, bool(transparent)};
      }
      ++ordinal;
    }
  }
  for(const auto& row : result.rows) {
    work(remaining);
    if(row.drawOrdinal == UINT32_MAX) invalid("Mesh provider object has no draw binding");
  }
  work(remaining, shape.realObjects);
  auto sorted = result.rows;
  const auto key = [](const BindingRow& row) {
    return std::tuple{row.primitive, row.firstIndex, row.indexCount, row.material, row.doubleSided, row.transparent};
  };
  std::sort(sorted.begin(), sorted.end(), [&](const auto& a, const auto& b) {
    work(remaining); return key(a) < key(b);
  });
  std::vector<SceneProviderTriangleBatch> batches;
  batches.reserve(sorted.size());
  for(size_t i = 0; i < sorted.size();) {
    const auto& row = sorted[i];
    size_t end = i + 1;
    while(end < sorted.size()) { work(remaining); if(key(sorted[end]) != key(row)) break; ++end; }
    work(remaining);
    if(row.indexCount) batches.push_back({row.firstIndex, row.indexCount, int32_t(row.material), row.doubleSided, row.transparent,
        uint32_t(end - i)});
    i = end;
  }
  result.asset = {.primitiveCount = input.primitiveRanges.size(), .materialCount = input.materialCount,
      .instanceCount = shape.realObjects, .triangleBatches = std::move(batches), .bounds = input.bounds,
      .triangleBatchesAuthoritative = true};
  return result;
}
} // namespace mesh_provider_detail

[[nodiscard]] inline std::vector<MeshSceneProviderPrimitive>
meshSceneProviderPrimitivesFromImport(
    std::span<const container::geometry::PrimitiveRange> primitiveRanges) {
  std::vector<MeshSceneProviderPrimitive> primitives;
  primitives.reserve(primitiveRanges.size());

  for (const container::geometry::PrimitiveRange &primitive : primitiveRanges) {
    primitives.push_back(MeshSceneProviderPrimitive{
        .firstIndex = primitive.firstIndex,
        .indexCount = primitive.indexCount,
        .materialIndex = primitive.materialIndex,
        .doubleSided = primitive.disableBackfaceCulling,
    });
  }

  return primitives;
}

[[nodiscard]] inline std::vector<MeshSceneMaterialProperties>
meshSceneMaterialPropertiesFromImport(
    std::size_t materialCount,
    const MeshSceneProviderMaterialResolver &materialFactsAt) {
  std::vector<MeshSceneMaterialProperties> materials;
  materials.reserve(materialCount);

  for (std::size_t materialIndex = 0; materialIndex < materialCount;
       ++materialIndex) {
    const MeshSceneProviderMaterialFacts facts =
        materialFactsAt ? materialFactsAt(static_cast<uint32_t>(materialIndex))
                        : MeshSceneProviderMaterialFacts{};
    materials.push_back(MeshSceneMaterialProperties{
        .transparent = facts.transparent,
        .doubleSided = facts.doubleSided,
    });
  }

  return materials;
}

[[nodiscard]] inline MeshSceneAsset
buildMeshSceneProviderAsset(const MeshSceneProviderAssetAdapterInput &input) {
  if(input.bindings) {
    const auto shape = mesh_provider_detail::preflight(input);
    return mesh_provider_detail::build(input, shape, input.bindings->limits.maxWork - shape.workUsed).asset;
  }
  const std::vector<MeshSceneProviderPrimitive> primitives =
      meshSceneProviderPrimitivesFromImport(input.primitiveRanges);
  const std::vector<MeshSceneMaterialProperties> materials =
      meshSceneMaterialPropertiesFromImport(input.materialCount,
                                            input.materialFactsAt);

  return buildMeshSceneAsset({
      .primitives = primitives,
      .materials = materials,
      .materialCount = input.materialCount,
      .instanceCount = input.instanceCount,
      .bounds = input.bounds,
  });
}

// Instances and bounds are live metadata. Rebuild classification/topology only
// after an explicit source, geometry, material or binding change. Complete
// binding views are compared exactly; transforms never rebuild their batches.
class MeshSceneProviderAssetCache {
public:
  [[nodiscard]] const MeshSceneAsset &
  get(const MeshSceneProviderAssetAdapterInput &input,
      SceneProviderRevision revision, const void *sourceIdentity) {
    const Key next{sourceIdentity,
                   input.primitiveRanges.data(),
                   input.primitiveRanges.size(),
                   input.materialCount,
                   revision.geometry,
                   revision.materials,
                   input.bindings.has_value()};
    if(input.bindings) {
      const auto limit = input.bindings->limits.maxOwnedBytes;
      if(bindingRows_.size() > limit / sizeof(mesh_provider_detail::BindingRow))
        mesh_provider_detail::invalid("Mesh provider retained bindings exceed the storage limit");
      auto oldBytes = uint64_t(bindingRows_.size()) * sizeof(mesh_provider_detail::BindingRow);
      if(asset_.triangleBatches.size() > (limit - oldBytes) / sizeof(SceneProviderTriangleBatch))
        mesh_provider_detail::invalid("Mesh provider retained batches exceed the storage limit");
      oldBytes += uint64_t(asset_.triangleBatches.size()) * sizeof(SceneProviderTriangleBatch);
      const auto shape = mesh_provider_detail::preflight(input, oldBytes);
      auto remaining = input.bindings->limits.maxWork - shape.workUsed;
      if(!key_ || *key_ != next || !mesh_provider_detail::matches(input, bindingRows_, shape, remaining)) {
        auto candidate = mesh_provider_detail::build(input, shape, remaining);
        asset_ = std::move(candidate.asset); bindingRows_ = std::move(candidate.rows); key_ = next;
      }
      asset_.bounds = input.bounds;
      return asset_;
    }
    if (!key_ || *key_ != next) {
      asset_ = buildMeshSceneProviderAsset(input);
      std::vector<mesh_provider_detail::BindingRow>{}.swap(bindingRows_);
      key_ = next;
    }
    asset_.instanceCount = input.instanceCount;
    asset_.bounds = input.bounds;
    return asset_;
  }

  void clear() {
    asset_ = {};
    key_.reset();
    std::vector<mesh_provider_detail::BindingRow>{}.swap(bindingRows_);
  }

private:
  struct Key {
    const void *sourceIdentity{};
    const container::geometry::PrimitiveRange *ranges{};
    std::size_t primitiveCount{};
    std::size_t materialCount{};
    uint64_t geometryRevision{};
    uint64_t materialRevision{};
    bool bindingAware{};
    bool operator==(const Key &) const = default;
  };
  std::optional<Key> key_{};
  MeshSceneAsset asset_{};
  std::vector<mesh_provider_detail::BindingRow> bindingRows_;
};

} // namespace container::scene
