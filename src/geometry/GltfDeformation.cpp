#include <Container/geometry/GltfDeformation.h>
#include <Container/geometry/GltfAccessor.h>
#include <Container/utility/ModelImportCancellation.h>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace container::geometry::gltf {
namespace {

using container::util::checkModelImportCancellation;

[[noreturn]] void fail(std::string_view label, std::string_view message) {
  throw std::runtime_error("glTF deformation " + std::string(label) + ": " +
                           std::string(message));
}

const tinygltf::Accessor& accessorAt(const tinygltf::Model& source, int index,
                                    std::string_view label) {
  if (index < 0 || static_cast<size_t>(index) >= source.accessors.size()) fail(label,"accessor index is out of range");
  return source.accessors[static_cast<size_t>(index)];
}
void requireType(const tinygltf::Accessor& accessor, int type, int component,
                 std::string_view label, bool allowDouble = false) {
  if (accessor.type != type ||
      (accessor.componentType != component && !(allowDouble && component == TINYGLTF_COMPONENT_TYPE_FLOAT && accessor.componentType == TINYGLTF_COMPONENT_TYPE_DOUBLE)) ||
      accessor.normalized) {
    fail(label, "accessor has an invalid type, component type or normalization");
  }
}

void requireCount(const tinygltf::Accessor& accessor, size_t count,
                  std::string_view label) {
  if (accessor.count != count) fail(label, "accessor count differs from vertex count");
}

std::vector<glm::vec3> readDeltas(const tinygltf::Model& source,
                                 const tinygltf::Primitive& primitive,
                                 const std::map<std::string, int>& target,
                                 const char* semantic, size_t vertexCount,
                                 const std::stop_token& stop) {
  const auto found = target.find(semantic);
  if (found == target.end()) return {};
  if (!primitive.attributes.contains(semantic)) {
    fail(semantic, "morph target has no corresponding base attribute");
  }
  const auto& accessor = accessorAt(source, found->second, semantic);
  const bool floating = !accessor.normalized && (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT ||
    (accessor.componentType == TINYGLTF_COMPONENT_TYPE_DOUBLE && HasRequiredExtension(source,"KHR_accessor_float64")));
  const bool signedInteger = accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE || accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT;
  const bool quantized = HasRequiredExtension(source,"KHR_mesh_quantization") && signedInteger &&
    (std::string_view(semantic) == "POSITION" || accessor.normalized);
  if (accessor.type != TINYGLTF_TYPE_VEC3 || !(floating || quantized))
    fail(semantic,"morph deltas require FLOAT, declared DOUBLE, or valid declared signed quantization");
  requireCount(accessor, vertexCount, semantic);
  CheckedAccessor values(source,found->second,semantic,{AccessorUsage::VertexAttribute,
    HasRequiredExtension(source,"KHR_accessor_float64")},stop);
  std::vector<glm::vec3> result(vertexCount);
  for (size_t i = 0; i < vertexCount; ++i) {
    if (i % 1024 == 0) checkModelImportCancellation(stop);
    std::array<float,3> value{}; values.readFloats(i,value);
    result[i] = glm::vec3(value[0],value[1],value[2]);
  }
  return result;
}

std::vector<float> weights(const std::vector<double>& values, size_t count,
                           std::string_view label, const std::stop_token& stop) {
  if (!values.empty() && values.size() != count) {
    fail(label, "weight count differs from morph target count");
  }
  std::vector<float> result(count, 0.0f);
  for (size_t i = 0; i < values.size(); ++i) {
    if (i % 1024 == 0) checkModelImportCancellation(stop);
    if (!std::isfinite(values[i]) ||
        std::abs(values[i]) > static_cast<double>(std::numeric_limits<float>::max())) {
      fail(label, "morph weight is non-finite or outside float range");
    }
    // Morph weights may be negative, greater than one or not sum to one.
    result[i] = static_cast<float>(values[i]);
  }
  return result;
}

bool finite(const glm::vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(const glm::mat4& matrix) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      if (!std::isfinite(matrix[c][r])) return false;
    }
  }
  return true;
}

void requireAffine(const glm::mat4& matrix, std::string_view label) {
  if (!finite(matrix) || std::abs(matrix[0][3]) > 1.0e-5f ||
      std::abs(matrix[1][3]) > 1.0e-5f || std::abs(matrix[2][3]) > 1.0e-5f ||
      std::abs(matrix[3][3] - 1.0f) > 1.0e-5f) {
    fail(label, "matrix must be finite and affine");
  }
}

glm::vec3 unitOr(const glm::vec3& value, const glm::vec3& fallback) {
  if (!finite(value)) return fallback;
  const float magnitude = std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
  if (!(magnitude > 0.0f)) return fallback;
  const glm::vec3 scaled = value / magnitude;
  return scaled / std::sqrt(glm::dot(scaled, scaled));
}

glm::vec3 frameTangent(const glm::vec3& tangent, const glm::vec3& normal) {
  const glm::vec3 axis = std::abs(normal.x) < 0.9f ? glm::vec3(1, 0, 0)
                                                   : glm::vec3(0, 1, 0);
  const glm::vec3 fallback = glm::normalize(axis - normal * glm::dot(normal, axis));
  return unitOr(tangent - normal * glm::dot(normal, tangent), fallback);
}

size_t setIndex(std::string_view semantic, std::string_view prefix) {
  const auto suffix = semantic.substr(prefix.size());
  size_t index = 0;
  const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
  if (suffix.empty() || (suffix.size() > 1 && suffix.front() == '0') ||
      parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size()) {
    fail(semantic, "invalid influence set suffix");
  }
  return index;
}

DeformationPrimitive loadPrimitive(const tinygltf::Model& source,
                                    size_t meshIndex, size_t primitiveIndex,
                                    const Mesh& runtime, const std::stop_token& stop) {
  checkModelImportCancellation(stop);
  const auto& mesh = source.meshes[meshIndex];
  const auto& primitive = mesh.primitives[primitiveIndex];
  DeformationPrimitive result;
  result.sourceMesh = meshIndex;
  result.sourcePrimitive = primitiveIndex;
  result.bindVertices = runtime.vertices();
  result.authoredBaseNormal = primitive.attributes.contains("NORMAL");
  result.authoredBaseTangent = primitive.attributes.contains("TANGENT");
  if (primitive.material >= 0 && static_cast<size_t>(primitive.material) < source.materials.size()) {
    const auto& normal = source.materials[static_cast<size_t>(primitive.material)].normalTexture;
    if (normal.index >= 0) {
      int texCoord = normal.texCoord;
      if (const auto transform = normal.extensions.find("KHR_texture_transform");
          transform != normal.extensions.end() && transform->second.IsObject() && transform->second.Has("texCoord")) {
        const auto& selected = transform->second.Get("texCoord");
        if (!selected.IsInt()) fail("normal texture", "texture-transform texCoord must be an integer");
        texCoord = selected.Get<int>();
      }
      if (texCoord < 0) fail("normal texture", "texCoord must be nonnegative");
      if (!primitive.targets.empty() && (!result.authoredBaseNormal || !result.authoredBaseTangent) && texCoord > 1)
        fail("normal texture", "morph frame regeneration supports UV0 and UV1");
      result.normalTextureTexCoord = static_cast<uint32_t>(texCoord);
    }
  }
  const size_t count = result.bindVertices.size();
  const auto position = primitive.attributes.find("POSITION");
  if (position == primitive.attributes.end() || count == 0) {
    fail("primitive", "missing positions or empty runtime vertices");
  }
  requireCount(accessorAt(source, position->second, "POSITION"), count, "POSITION");
  for (size_t i = 0; i < count; ++i) {
    if (i % 1024 == 0) checkModelImportCancellation(stop);
    const auto& vertex = result.bindVertices[i];
    if (!finite(vertex.position) || !finite(vertex.normal) ||
        !finite(glm::vec3(vertex.tangent)) || !std::isfinite(vertex.tangent.w) || !std::isfinite(vertex.colorAlpha)) {
      fail("primitive", "bind vertex contains non-finite deformation attributes");
    }
  }

  std::map<size_t, int> jointSets;
  std::map<size_t, int> weightSets;
  for (const auto& [name, accessor] : primitive.attributes) {
    if (name.starts_with("JOINTS_")) {
      if (!jointSets.emplace(setIndex(name, "JOINTS_"), accessor).second) {
        fail(name, "duplicate influence set");
      }
    } else if (name.starts_with("WEIGHTS_")) {
      if (!weightSets.emplace(setIndex(name, "WEIGHTS_"), accessor).second) {
        fail(name, "duplicate influence set");
      }
    }
  }
  if (jointSets.size() != weightSets.size()) {
    fail("skinning", "JOINTS and WEIGHTS set counts differ");
  }
  if (!jointSets.empty()) result.influences.resize(count);
  if (jointSets.size() > std::numeric_limits<size_t>::max()/4) fail("JOINTS","influence count overflow");
  for (size_t i = 0; i < result.influences.size(); ++i) {
    if (i % 1024 == 0) checkModelImportCancellation(stop);
    result.influences[i].values.reserve(jointSets.size()*4);
  }
  size_t expectedSet = 0;
  for (const auto& [set, jointIndex] : jointSets) {
    const auto weight = weightSets.find(set);
    if (set != expectedSet++ || weight == weightSets.end()) {
      fail("skinning", "JOINTS and WEIGHTS sets must be paired and consecutive from zero");
    }
    const auto& joints = accessorAt(source, jointIndex, "JOINTS");
    const auto& weightAccessor = accessorAt(source, weight->second, "WEIGHTS");
    if (joints.type != TINYGLTF_TYPE_VEC4 || joints.normalized ||
        (joints.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
         joints.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)) {
      fail("JOINTS", "expected unnormalized unsigned byte or unsigned short VEC4");
    }
    const bool floatWeights = weightAccessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT ||
      (weightAccessor.componentType == TINYGLTF_COMPONENT_TYPE_DOUBLE && HasRequiredExtension(source,"KHR_accessor_float64"));
    const bool integerWeights = weightAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
                                weightAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
    if (weightAccessor.type != TINYGLTF_TYPE_VEC4 ||
        !(floatWeights || integerWeights) ||
        (floatWeights && weightAccessor.normalized) ||
        (integerWeights && !weightAccessor.normalized)) {
      fail("WEIGHTS", "expected float or normalized unsigned byte/short VEC4");
    }
    requireCount(joints, count, "JOINTS");
    requireCount(weightAccessor, count, "WEIGHTS");
    CheckedAccessor jointValues(source,jointIndex,"JOINTS",{AccessorUsage::VertexAttribute},stop);
    CheckedAccessor weightValues(source,weight->second,"WEIGHTS",{AccessorUsage::VertexAttribute,
      HasRequiredExtension(source,"KHR_accessor_float64")},stop);
    for (size_t i = 0; i < count; ++i) {
      if (i % 1024 == 0) checkModelImportCancellation(stop);
      std::array<uint32_t,4> jointIndices{}; jointValues.readUnsigned(i,jointIndices);
      std::array<float,4> decodedWeights{}; weightValues.readFloats(i,decodedWeights);
      for (size_t c = 0; c < 4; ++c) {
        const float influenceWeight = decodedWeights[c];
        if (influenceWeight < 0.0f || influenceWeight > 1.0f) {
          fail("WEIGHTS", "joint weight is outside [0,1]");
        }
        result.influences[i].values.push_back(
            {jointIndices[c], influenceWeight});
      }
    }
  }
  for (size_t i = 0; i < result.influences.size(); ++i) {
    if (i % 1024 == 0) checkModelImportCancellation(stop);
    auto& vertex = result.influences[i];
    double sum = 0.0;
    for (const auto& influence : vertex.values) sum += influence.weight;
    if (!(sum > 0.0) || !std::isfinite(sum)) {
      fail("WEIGHTS", "vertex has no positive finite joint weight");
    }
    for (auto& influence : vertex.values) {
      influence.weight = static_cast<float>(static_cast<double>(influence.weight) / sum);
    }
  }

  result.morphTargets.reserve(primitive.targets.size());
  for (const auto& target : primitive.targets) {
    checkModelImportCancellation(stop);
    for (const auto& [semantic, unused] : target) {
      (void)unused;
      if (semantic != "POSITION" && semantic != "NORMAL" && semantic != "TANGENT")
        fail(semantic,"unsupported morph target semantic; UV/color morph playback is not implemented");
    }
    result.morphTargets.push_back({
        readDeltas(source, primitive, target, "POSITION", count, stop),
        readDeltas(source, primitive, target, "NORMAL", count, stop),
        readDeltas(source, primitive, target, "TANGENT", count, stop)});
  }
  result.meshDefaultWeights = weights(mesh.weights, primitive.targets.size(), "mesh weights", stop);
  return result;
}

}  // namespace

DeformationData LoadDeformationData(const tinygltf::Model& source,
                                     std::span<const Mesh> runtimePrimitives,
                                     const std::stop_token& stop) {
  checkModelImportCancellation(stop);
  DeformationData result;
  size_t primitiveCount = 0;
  for (const auto& mesh : source.meshes) {
    if (mesh.primitives.size() > std::numeric_limits<size_t>::max() - primitiveCount) {
      fail("primitive", "primitive count overflows size_t");
    }
    primitiveCount += mesh.primitives.size();
    if (!mesh.primitives.empty()) {
      for (const auto& primitive : mesh.primitives) {
        checkModelImportCancellation(stop);
        if (primitive.targets.size() != mesh.primitives.front().targets.size()) {
          fail("morph", "mesh primitives have different morph target counts");
        }
      }
    }
  }
  if (primitiveCount != runtimePrimitives.size()) {
    fail("primitive", "runtime primitive count differs from source primitive count");
  }
  result.primitives.reserve(primitiveCount);
  for (size_t mesh = 0; mesh < source.meshes.size(); ++mesh) {
    for (size_t primitive = 0; primitive < source.meshes[mesh].primitives.size(); ++primitive) {
      const size_t runtimeIndex = result.primitives.size();
      result.primitives.push_back(loadPrimitive(source, mesh, primitive,
                                                 runtimePrimitives[runtimeIndex], stop));
    }
  }

  result.skins.reserve(source.skins.size());
  for (const auto& skin : source.skins) {
    checkModelImportCancellation(stop);
    if (skin.joints.empty()) fail("skin", "skin has no joints");
    DeformationSkin converted;
    converted.skeleton = skin.skeleton;
    if (skin.skeleton < -1 ||
        (skin.skeleton >= 0 && static_cast<size_t>(skin.skeleton) >= source.nodes.size())) {
      fail("skin", "skeleton node is out of range");
    }
    std::set<int> uniqueJoints;
    for (const int joint : skin.joints) {
      if (converted.joints.size() % 1024 == 0) checkModelImportCancellation(stop);
      if (joint < 0 || static_cast<size_t>(joint) >= source.nodes.size() ||
          !uniqueJoints.insert(joint).second) {
        fail("skin", "joint node is out of range or duplicated");
      }
      converted.joints.push_back(static_cast<uint32_t>(joint));
    }
    converted.inverseBindMatrices.assign(skin.joints.size(), glm::mat4(1.0f));
    if (skin.inverseBindMatrices < -1) fail("skin", "invalid inverse bind accessor");
    if (skin.inverseBindMatrices >= 0) {
      const auto& accessor = accessorAt(source, skin.inverseBindMatrices, "inverse bind");
      requireType(accessor, TINYGLTF_TYPE_MAT4, TINYGLTF_COMPONENT_TYPE_FLOAT, "inverse bind",HasRequiredExtension(source,"KHR_accessor_float64"));
      if (accessor.count < skin.joints.size()) {
        fail("inverse bind", "matrix count is smaller than joint count");
      }
      CheckedAccessor values(source,skin.inverseBindMatrices,"inverse bind",{AccessorUsage::Generic,
        HasRequiredExtension(source,"KHR_accessor_float64")},stop);
      for (size_t i = 0; i < accessor.count; ++i) {
        if (i % 1024 == 0) checkModelImportCancellation(stop);
        std::array<float,16> decoded{}; values.readFloats(i,decoded);
        glm::mat4 matrix(1.0f);
        for (size_t c = 0; c < 4; ++c) {
          for (size_t r = 0; r < 4; ++r) {
            matrix[static_cast<int>(c)][static_cast<int>(r)] = decoded[c*4+r];
          }
        }
        requireAffine(matrix, "inverse bind");
        if (i < converted.inverseBindMatrices.size()) converted.inverseBindMatrices[i] = matrix;
      }
    }
    result.skins.push_back(std::move(converted));
  }

  result.nodes.reserve(source.nodes.size());
  for (const auto& node : source.nodes) {
    checkModelImportCancellation(stop);
    if (node.mesh < -1 || (node.mesh >= 0 && static_cast<size_t>(node.mesh) >= source.meshes.size())) {
      fail("node", "mesh index is out of range");
    }
    if (node.skin < -1 || (node.skin >= 0 && static_cast<size_t>(node.skin) >= result.skins.size())) {
      fail("node", "skin index is out of range");
    }
    DeformationNode converted;
    converted.sourceMesh = node.mesh;
    converted.skin = node.skin;
    if (node.mesh >= 0) {
      const auto& mesh = source.meshes[static_cast<size_t>(node.mesh)];
      const size_t count = mesh.primitives.empty() ? 0 : mesh.primitives.front().targets.size();
      converted.defaultMorphWeights = weights(node.weights.empty() ? mesh.weights : node.weights,
                                               count, "node weights", stop);
      if (node.skin >= 0) {
        const size_t joints = result.skins[static_cast<size_t>(node.skin)].joints.size();
        for (const auto& primitive : result.primitives) {
          checkModelImportCancellation(stop);
          if (primitive.sourceMesh != static_cast<size_t>(node.mesh)) continue;
          if (!primitive.hasSkinning()) fail("node", "skinned mesh is missing joint attributes");
          for (size_t i = 0; i < primitive.influences.size(); ++i) {
            if (i % 1024 == 0) checkModelImportCancellation(stop);
            const auto& vertex = primitive.influences[i];
            for (const auto& influence : vertex.values) {
              if (influence.joint >= joints) fail("JOINTS", "joint index is outside the node's skin");
            }
          }
        }
      }
    } else if (node.skin >= 0 || !node.weights.empty()) {
      fail("node", "skin or morph weights require a mesh");
    }
    result.nodes.push_back(std::move(converted));
  }
  return result;
}

std::vector<glm::mat4> ComputeSkinPalette(
    const DeformationSkin& skin, const glm::mat4& meshNodeWorld,
    std::span<const glm::mat4> nodeWorldMatrices) {
  if (skin.joints.empty() || skin.joints.size() != skin.inverseBindMatrices.size()) {
    fail("palette", "skin joints and inverse bind matrices are empty or unpaired");
  }
  requireAffine(meshNodeWorld, "mesh world");
  const double determinant = glm::determinant(glm::dmat3(meshNodeWorld));
  if (!std::isfinite(determinant) || determinant == 0.0) {
    fail("mesh world", "mesh world transform is not invertible");
  }
  // Invert/combine in double precision so a small finite node scale does not
  // underflow a float determinant, or overflow an intermediate product.
  const glm::dmat4 worldToMesh = glm::inverse(glm::dmat4(meshNodeWorld));
  requireAffine(glm::mat4(worldToMesh), "inverse mesh world");
  std::vector<glm::mat4> result;
  result.reserve(skin.joints.size());
  for (size_t i = 0; i < skin.joints.size(); ++i) {
    if (skin.joints[i] >= nodeWorldMatrices.size()) fail("palette", "joint world node is out of range");
    requireAffine(nodeWorldMatrices[skin.joints[i]], "joint world");
    requireAffine(skin.inverseBindMatrices[i], "inverse bind");
    const glm::mat4 matrix(worldToMesh * glm::dmat4(nodeWorldMatrices[skin.joints[i]]) *
                           glm::dmat4(skin.inverseBindMatrices[i]));
    requireAffine(matrix, "palette");
    result.push_back(matrix);
  }
  return result;
}

std::vector<Vertex> EvaluatePrimitive(const DeformationPrimitive& primitive,
                                      std::span<const float> morphWeights,
                                      std::span<const glm::mat4> skinPalette) {
  if (morphWeights.empty()) morphWeights = primitive.meshDefaultWeights;
  if (morphWeights.size() != primitive.morphTargets.size()) {
    fail("evaluate", "morph weight count differs from target count");
  }
  for (const float weight : morphWeights) {
    if (!std::isfinite(weight)) fail("evaluate", "non-finite morph weight");
  }
  const size_t count = primitive.bindVertices.size();
  if (!primitive.influences.empty() && primitive.influences.size() != count) {
    fail("evaluate", "influence count differs from vertex count");
  }
  for (const auto& target : primitive.morphTargets) {
    for (const auto* deltas : {&target.positions, &target.normals, &target.tangents}) {
      if (!deltas->empty() && deltas->size() != count) {
        fail("evaluate", "morph delta count differs from vertex count");
      }
      for (const auto& delta : *deltas) {
        if (!finite(delta)) fail("evaluate", "non-finite morph delta");
      }
    }
  }
  for (const auto& matrix : skinPalette) requireAffine(matrix, "palette");
  std::vector<Vertex> result = primitive.bindVertices;
  for (size_t i = 0; i < count; ++i) {
    auto& vertex = result[i];
    if (!finite(vertex.position) || !finite(vertex.normal) ||
        !finite(glm::vec3(vertex.tangent)) || !std::isfinite(vertex.tangent.w)) {
      fail("evaluate", "bind vertex contains a non-finite attribute");
    }
    glm::vec3 normal = vertex.normal;
    glm::vec3 tangent(vertex.tangent);
    for (size_t targetIndex = 0; targetIndex < primitive.morphTargets.size(); ++targetIndex) {
      const auto& target = primitive.morphTargets[targetIndex];
      const float weight = morphWeights[targetIndex];
      if (!target.positions.empty()) vertex.position += weight * target.positions[i];
      if (!target.normals.empty()) normal += weight * target.normals[i];
      if (!target.tangents.empty()) tangent += weight * target.tangents[i];
    }
    float handedness = vertex.tangent.w < 0.0f ? -1.0f : 1.0f;
    if (!skinPalette.empty() && primitive.hasSkinning()) {
      glm::mat4 blended(0.0f);
      double sum = 0.0;
      for (const auto& influence : primitive.influences[i].values) {
        if (influence.joint >= skinPalette.size() ||
            !std::isfinite(influence.weight) || influence.weight < 0.0f) {
          fail("evaluate", "invalid joint index or influence weight");
        }
        blended += skinPalette[influence.joint] * influence.weight;
        sum += influence.weight;
      }
      if (!(sum > 0.0) || !std::isfinite(sum)) fail("evaluate", "vertex has no positive joint weight");
      // Public structs may also be authored directly by tests or tools.
      blended /= static_cast<float>(sum);
      vertex.position = glm::vec3(blended * glm::vec4(vertex.position, 1.0f));
      const glm::mat3 linear(blended);
      float linearMagnitude = 0.0f;
      for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) linearMagnitude = std::max(linearMagnitude, std::abs(linear[c][r]));
      }
      const glm::mat3 scaled = linearMagnitude > 0.0f ? linear / linearMagnitude : glm::mat3(0.0f);
      const float determinant = glm::determinant(scaled);
      if (determinant != 0.0f) {
        // Cofactors are inverse-transpose times determinant. Normalization
        // removes its magnitude; keeping the sign preserves mirrored normals.
        // Scaling first avoids a determinant threshold tied to world units.
        const glm::mat3 cofactors(glm::cross(scaled[1], scaled[2]),
                                  glm::cross(scaled[2], scaled[0]),
                                  glm::cross(scaled[0], scaled[1]));
        normal = cofactors * normal * (determinant < 0.0f ? -1.0f : 1.0f);
      } else {
        // Linear blend skinning can legitimately collapse opposing rotations.
        // Retain a finite deterministic frame when an inverse does not exist.
        normal = scaled * normal;
      }
      tangent = scaled * tangent;
      handedness *= determinant < 0.0f ? -1.0f : 1.0f;
    }
    if (!finite(vertex.position) || !finite(normal) || !finite(tangent)) {
      fail("evaluate", "deformation produced a non-finite attribute");
    }
    vertex.normal = unitOr(normal, unitOr(primitive.bindVertices[i].normal, glm::vec3(0, 1, 0)));
    vertex.tangent = glm::vec4(frameTangent(tangent, vertex.normal), handedness);
  }
  return result;
}

}  // namespace container::geometry::gltf
