#include <Container/geometry/GltfPrimitiveFrameVariants.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace container::geometry::gltf {
namespace {
[[noreturn]] void fail(const char* reason) {
  throw std::runtime_error(std::string("glTF primitive frame variants: ") + reason);
}
uint64_t add(uint64_t a, uint64_t b) {
  if (b > std::numeric_limits<uint64_t>::max() - a) fail("byte/count overflow");
  return a + b;
}
uint64_t mul(uint64_t a, uint64_t b) {
  if (a && b > std::numeric_limits<uint64_t>::max() / a) fail("byte/count overflow");
  return a * b;
}
void cap(uint64_t value, uint64_t limit, const char* reason) {
  if (value > limit) fail(reason);
}
template<class V> bool finiteVector(const V& value) {
  for (int i = 0; i < value.length(); ++i)
    if (!std::isfinite(value[i])) return false;
  return true;
}
PrimitiveFrameExportPolicy exportPolicy(const CanonicalPrimitiveFrameMetadata& metadata, bool generated) {
  const bool normal = metadata.authoredNormal();
  const bool tangent = normal && metadata.authoredTangent() && !generated;
  return {normal, tangent, normal, tangent};
}
PrimitiveFrameVariantKey key(const CanonicalPrimitiveFrameMetadata& metadata,
                            PrimitiveFrameVariantKind kind) {
  return {metadata.runtimePrimitive(), metadata.bindRevision(), kind};
}
} // namespace

CanonicalPrimitiveFrameMetadata::CanonicalPrimitiveFrameMetadata(uint32_t runtimePrimitive,
    std::optional<uint32_t> sourceFlatPrimitive, bool authoredNormal, bool authoredTangent,
    uint32_t bindNormalTexCoord, uint8_t availableTexCoordMask, uint64_t bindRevision)
    : runtimePrimitive_(runtimePrimitive), sourceFlatPrimitive_(sourceFlatPrimitive),
      authoredNormal_(authoredNormal), authoredTangent_(authoredTangent),
      bindNormalTexCoord_(bindNormalTexCoord), availableTexCoordMask_(availableTexCoordMask),
      bindRevision_(bindRevision) {
  if (runtimePrimitive == std::numeric_limits<uint32_t>::max() ||
      sourceFlatPrimitive == std::numeric_limits<uint32_t>::max()) fail("reserved primitive identity");
  if (bindNormalTexCoord > 1 || (availableTexCoordMask & ~uint8_t(3))) fail("unsupported canonical UV metadata");
}

PrimitiveFrameVariantPlan PreparePrimitiveFrameVariants(
    const CanonicalPrimitiveFrameMetadata& metadata, const DeformationPrimitive& primitive,
    std::span<const uint32_t> indices, std::span<const PrimitiveFrameBindingRequest> bindings,
    const PrimitiveFrameVariantLimits& limits) {
  cap(primitive.bindVertices.size(), limits.maxInputVertices, "input vertex limit exceeded");
  cap(indices.size(), limits.maxTriangleCorners, "triangle corner limit exceeded");
  cap(bindings.size(), limits.maxBindings, "binding limit exceeded");
  cap(primitive.morphTargets.size(), limits.maxMorphTargets, "morph target limit exceeded");
  if (primitive.bindVertices.empty() || indices.empty() || indices.size() % 3 ||
      indices.size() > std::numeric_limits<uint32_t>::max() ||
      indices.size() / 3 > size_t(std::numeric_limits<int>::max())) fail("invalid local triangle list");
  if (primitive.authoredBaseNormal != metadata.authoredNormal() ||
      primitive.authoredBaseTangent != metadata.authoredTangent() ||
      primitive.normalTextureTexCoord != metadata.bindNormalTexCoord())
    fail("primitive differs from captured canonical frame policy");
  if (!primitive.influences.empty() && primitive.influences.size() != primitive.bindVertices.size())
    fail("influence count differs from bind vertex count");
  if (primitive.meshDefaultWeights.size() != primitive.morphTargets.size())
    fail("default weight count differs from morph target count");

  std::array<bool, 2> generated{};
  for (const auto& binding : bindings) {
    if (binding.graphNode == std::numeric_limits<uint32_t>::max()) fail("reserved graph binding identity");
    if (!binding.normalTexCoord) continue;
    const auto uv = *binding.normalTexCoord;
    if (uv > 1 || !(metadata.availableTexCoordMask() & (1u << uv))) fail("requested normal texture UV is absent or unsupported");
    if (uv != metadata.bindNormalTexCoord()) generated[uv] = true;
  }
  const uint64_t generatedCount = uint64_t(generated[0]) + uint64_t(generated[1]);
  uint64_t inputInfluences = 0;
  for (const auto& vertex : primitive.influences) {
    inputInfluences = add(inputInfluences, vertex.values.size());
    cap(inputInfluences, limits.maxInfluences, "input influence limit exceeded");
  }
  uint64_t expandedInfluences = 0;
  for (const auto index : indices) {
    if (index >= primitive.bindVertices.size()) fail("triangle index exceeds bind vertex count");
    if (!primitive.influences.empty()) expandedInfluences = add(expandedInfluences, primitive.influences[index].values.size());
  }
  cap(mul(expandedInfluences, generatedCount), limits.maxInfluences, "expanded influence limit exceeded");
  uint64_t retainedMorphAttributes = 0;
  uint64_t inputBytes = add(sizeof(DeformationPrimitive), mul(primitive.bindVertices.size(), sizeof(Vertex)));
  inputBytes = add(inputBytes, mul(indices.size(), sizeof(uint32_t)));
  inputBytes = add(inputBytes, mul(bindings.size(), sizeof(PrimitiveFrameBindingRequest)));
  inputBytes = add(inputBytes, mul(primitive.influences.size(), sizeof(VertexSkinInfluences)));
  inputBytes = add(inputBytes, mul(inputInfluences, sizeof(JointInfluence)));
  inputBytes = add(inputBytes, mul(primitive.morphTargets.size(), sizeof(MorphTarget)));
  inputBytes = add(inputBytes, mul(primitive.meshDefaultWeights.size(), sizeof(float)));
  for (const auto& target : primitive.morphTargets) {
    for (const auto* values : {&target.positions, &target.normals, &target.tangents}) {
      if (!values->empty() && values->size() != primitive.bindVertices.size()) fail("morph delta count differs from bind vertex count");
      inputBytes = add(inputBytes, mul(values->size(), sizeof(glm::vec3)));
    }
    retainedMorphAttributes = add(retainedMorphAttributes, !target.positions.empty());
    if (metadata.authoredNormal()) retainedMorphAttributes = add(retainedMorphAttributes, !target.normals.empty());
  }
  cap(inputBytes, limits.maxInputBytes, "input byte limit exceeded");

  uint64_t perTemplate = mul(indices.size(), sizeof(Vertex) + 2 * sizeof(uint32_t));
  if (!primitive.influences.empty()) {
    perTemplate = add(perTemplate, mul(indices.size(), sizeof(VertexSkinInfluences)));
    perTemplate = add(perTemplate, mul(expandedInfluences, sizeof(JointInfluence)));
  }
  perTemplate = add(perTemplate, mul(primitive.morphTargets.size(), sizeof(MorphTarget)));
  // PrepareDeformationFrames appends retained target deltas without reserve;
  // account for geometric vector growth as well as its final logical length.
  perTemplate = add(perTemplate, mul(mul(indices.size(), retainedMorphAttributes), 2 * sizeof(glm::vec3)));
  perTemplate = add(perTemplate, mul(primitive.meshDefaultWeights.size(), sizeof(float)));
  uint64_t outputBytes = add(sizeof(PrimitiveFrameVariantPlan), mul(bindings.size(), sizeof(PrimitiveFrameVariantSelection)));
  outputBytes = add(outputBytes, mul(generatedCount, add(sizeof(PrimitiveFrameVariantTemplate), perTemplate)));
  const auto idScratch = mul(bindings.size(), sizeof(uint32_t));
  // Templates are built sequentially. Each preparer performs one temporary
  // EvaluatePrimitive copy; its scratch is reused rather than summed.
  const auto workspaceBytes = add(idScratch, generatedCount ? mul(indices.size(), sizeof(Vertex)) : 0);
  cap(outputBytes, limits.maxOutputBytes, "output byte limit exceeded");
  cap(workspaceBytes, limits.maxWorkspaceBytes, "workspace byte limit exceeded");

  // Payload validation is allocation-free, including attributes retained for
  // canonical aliases. Missing NORMAL deliberately ignores supplied N/T data.
  for (const auto& vertex : primitive.influences) {
    double sum = 0;
    for (const auto& influence : vertex.values) {
      if (!std::isfinite(influence.weight) || influence.weight < 0) fail("invalid influence weight");
      sum += influence.weight;
    }
    if (!(sum > 0) || !std::isfinite(sum)) fail("vertex has no positive influence weight");
  }
  for (const auto& vertex : primitive.bindVertices) {
    if (!finiteVector(vertex.position) || !finiteVector(vertex.color) || !std::isfinite(vertex.colorAlpha)) fail("non-finite vertex attribute");
    if (metadata.authoredNormal() && !finiteVector(vertex.normal)) fail("non-finite authored normal");
    if (metadata.authoredNormal() && metadata.authoredTangent() && !finiteVector(vertex.tangent)) fail("non-finite authored tangent");
    if ((metadata.availableTexCoordMask() & 1) && !finiteVector(vertex.texCoord)) fail("non-finite UV0");
    if ((metadata.availableTexCoordMask() & 2) && !finiteVector(vertex.texCoord1)) fail("non-finite UV1");
  }
  for (const auto weight : primitive.meshDefaultWeights)
    if (!std::isfinite(weight)) fail("non-finite default morph weight");
  for (const auto& target : primitive.morphTargets) {
    for (const auto& value : target.positions) if (!finiteVector(value)) fail("non-finite POSITION morph delta");
    if (metadata.authoredNormal()) {
      for (const auto& value : target.normals) if (!finiteVector(value)) fail("non-finite NORMAL morph delta");
      if (metadata.authoredTangent())
        for (const auto& value : target.tangents) if (!finiteVector(value)) fail("non-finite TANGENT morph delta");
    }
  }
  std::vector<uint32_t> bindingIds;
  bindingIds.reserve(bindings.size());
  for (const auto& binding : bindings) bindingIds.push_back(binding.graphNode);
  std::sort(bindingIds.begin(), bindingIds.end());
  if (std::adjacent_find(bindingIds.begin(), bindingIds.end()) != bindingIds.end()) fail("duplicate graph binding request");

  PrimitiveFrameVariantPlan result(metadata);
  result.outputBytes_ = outputBytes;
  result.workspaceBytes_ = workspaceBytes;
  result.templates_.reserve(size_t(generatedCount));
  std::array<std::optional<uint32_t>, 2> templateIndices;
  for (uint32_t uv = 0; uv < 2; ++uv) {
    if (!generated[uv]) continue;
    templateIndices[uv] = static_cast<uint32_t>(result.templates_.size());
    PrimitiveFrameVariantTemplate prepared;
    prepared.key_ = key(metadata, uv ? PrimitiveFrameVariantKind::GeneratedUV1 : PrimitiveFrameVariantKind::GeneratedUV0);
    // A different UV basis invalidates even an authored source tangent. Only
    // the derived copy loses T/target-T; immutable canonical data stays intact.
    prepared.frames_ = PrepareDeformationFrames(primitive, indices, {metadata.authoredNormal(), false, uv});
    result.templates_.push_back(std::move(prepared));
  }
  result.selections_.reserve(bindings.size());
  for (const auto& binding : bindings) {
    const auto uv = binding.normalTexCoord.value_or(metadata.bindNormalTexCoord());
    const bool replacement = uv != metadata.bindNormalTexCoord();
    const auto kind = !replacement ? PrimitiveFrameVariantKind::Canonical :
        (uv ? PrimitiveFrameVariantKind::GeneratedUV1 : PrimitiveFrameVariantKind::GeneratedUV0);
    result.selections_.push_back({binding.graphNode, key(metadata, kind),
        replacement ? templateIndices[uv] : std::nullopt, exportPolicy(metadata, replacement)});
  }
  return result;
}

} // namespace container::geometry::gltf
