#include <Container/geometry/GltfInstancing.h>
#include <Container/utility/ModelImportCancellation.h>
#include <Container/geometry/GltfAccessor.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace container::geometry::gltf {
namespace {
using container::util::checkModelImportCancellation;
using Json = nlohmann::json;
constexpr std::string_view kExtension = "EXT_mesh_gpu_instancing";
[[noreturn]] void fail(std::string_view context, std::string_view message) {
  throw std::runtime_error(std::string(kExtension) + " " + std::string(context) + ": " + std::string(message));
}
std::string label(uint32_t node) { return "node[" + std::to_string(node) + "]"; }
bool listed(const std::vector<std::string>& list, std::string_view name) {
  return std::find(list.begin(), list.end(), name) != list.end();
}
bool listed(const Json& source, const char* table, std::string_view name) {
  const auto found = source.find(table);
  if (found == source.end()) return false;
  if (!found->is_array()) fail(table, "must be an array");
  return std::ranges::any_of(*found, [&](const auto& value) {
    return value.is_string() && value.template get_ref<const std::string&>() == name;
  });
}
template<class Document>
Document* table(Document& source, const char* name) {
  const auto found = source.find(name);
  if (found == source.end()) return nullptr;
  if (!found->is_array()) fail(name, "must be an array");
  return &*found;
}
uint64_t integer(const Json& value, uint64_t maximum, std::string_view context, bool&) {
  uint64_t result;
  if (value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0)) {
    result = value.get<uint64_t>();
  } else if (value.is_number_float()) {
    const auto number = value.get<double>();
    // UINT64_MAX rounds up when converted to double.
    if (!(number >= 0 && number < 0x1p64 && std::trunc(number) == number))
      fail(context, "must be a nonnegative integer within range");
    result = static_cast<uint64_t>(number);
  } else {
    fail(context, "must be a nonnegative integer");
  }
  if (result > maximum) fail(context, "integer exceeds its range");
  return result;
}
uint64_t integer(Json& value, uint64_t maximum, std::string_view context, bool& rewritten) {
  const auto result = integer(std::as_const(value), maximum, context, rewritten);
  if (!value.is_number_unsigned()) {
    value = result;
    rewritten = true;
  }
  return result;
}
template<class Value>
int reference(Value& value, uint64_t count, std::string_view context, bool& rewritten) {
  const auto index = integer(value, INT_MAX, context, rewritten);
  if (index >= count) fail(context, "index is outside the source table");
  return static_cast<int>(index);
}
int shape(const std::string& value) {
  if (value == "SCALAR") return TINYGLTF_TYPE_SCALAR;
  if (value == "VEC2") return TINYGLTF_TYPE_VEC2;
  if (value == "VEC3") return TINYGLTF_TYPE_VEC3;
  if (value == "VEC4") return TINYGLTF_TYPE_VEC4;
  if (value == "MAT2") return TINYGLTF_TYPE_MAT2;
  if (value == "MAT3") return TINYGLTF_TYPE_MAT3;
  if (value == "MAT4") return TINYGLTF_TYPE_MAT4;
  return -1;
}
struct AccessorMetadata { int type{}, component{}; uint64_t count{}; bool normalized{}; };
struct AttributePlan { std::string semantic; int accessor{}; AccessorMetadata metadata; };
struct NodePlan { uint32_t node{}, mesh{}, primitives{}; uint64_t count{}; std::vector<AttributePlan> attributes; };
struct Budget {
  InstancingLimits limits;
  InstancingSummary summary;
  explicit Budget(const InstancingLimits& value) : limits(value) {
    if (!limits.maxInstances || limits.maxInstances > UINT32_MAX || !limits.maxExpandedPrimitives ||
        limits.maxExpandedPrimitives == UINT64_MAX || !limits.maxDecodedBytes || limits.maxDecodedBytes == UINT64_MAX)
      fail("limits", "instance, expansion and decoded-byte limits must be finite and positive");
  }
  void bytes(uint64_t count, uint64_t size, std::string_view context) {
    const auto available = std::min<uint64_t>(limits.maxDecodedBytes, SIZE_MAX) - summary.decodedBytes;
    if (count > available / size) fail(context, "decoded instance data exceeds the byte budget");
    summary.decodedBytes += count * size;
  }
  void finish(NodePlan& node) {
    const auto context = label(node.node);
    if (node.attributes.empty() || !node.count) fail(context, "attributes must establish a positive instance count");
    if (node.count > limits.maxInstances - summary.instances) fail(context, "aggregate instance count exceeds its limit");
    if (!node.primitives || node.count > (limits.maxExpandedPrimitives - summary.expandedPrimitives) / node.primitives)
      fail(context, "expanded primitive count exceeds its limit or mesh has no primitives");
    bytes(1, sizeof(NodeInstancing), context);
    bytes(node.count, sizeof(glm::mat4), context);
    summary.instances += node.count;
    summary.expandedPrimitives += node.count * node.primitives;
    ++summary.instancedNodes;
  }
};
void attribute(NodePlan& node, std::string_view semantic, int index,
               const AccessorMetadata& accessor, bool allowDouble, Budget& budget) {
  const auto context = label(node.node) + "." + std::string(semantic);
  if (index < 0) fail(context, "accessor index is negative");
  if (!accessor.count || accessor.count > budget.limits.maxInstances) fail(context, "accessor count is zero or exceeds the instance limit");
  if (node.count && node.count != accessor.count) fail(context, "all attribute counts must match");
  node.count = accessor.count;
  const bool float32 = accessor.component == TINYGLTF_COMPONENT_TYPE_FLOAT && !accessor.normalized;
  if (semantic == "TRANSLATION" || semantic == "SCALE") {
    if (accessor.type != TINYGLTF_TYPE_VEC3 || !float32) fail(context, "requires an unnormalized FLOAT VEC3 accessor");
  } else if (semantic == "ROTATION") {
    const bool quantized = accessor.normalized && (accessor.component == TINYGLTF_COMPONENT_TYPE_BYTE || accessor.component == TINYGLTF_COMPONENT_TYPE_SHORT);
    if (accessor.type != TINYGLTF_TYPE_VEC4 || (!float32 && !quantized))
      fail(context, "requires FLOAT VEC4 or normalized signed BYTE/SHORT VEC4");
  } else {
    if (semantic.empty() || semantic.front() != '_') fail(context, "application attribute names must begin with an underscore");
    const bool narrowInteger = accessor.component == TINYGLTF_COMPONENT_TYPE_BYTE || accessor.component == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
        accessor.component == TINYGLTF_COMPONENT_TYPE_SHORT || accessor.component == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
    const bool other = accessor.component == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT || accessor.component == TINYGLTF_COMPONENT_TYPE_FLOAT ||
        (allowDouble && accessor.component == TINYGLTF_COMPONENT_TYPE_DOUBLE);
    if (tinygltf::GetNumComponentsInType(accessor.type) <= 0 || (!narrowInteger && !other) || (accessor.normalized && !narrowInteger))
      fail(context, "custom accessor type/component/normalization is invalid");
  }
  budget.bytes(1, sizeof(InstanceAttributeReference), context);
  budget.bytes(semantic.size(), 1, context);
  node.attributes.push_back({std::string(semantic), index, accessor});
}
std::vector<NodePlan> modelLayout(const tinygltf::Model& source, Budget& budget,
                                  const std::stop_token& stop) {
  if (source.nodes.size() > UINT32_MAX) fail("nodes", "source node identity exceeds uint32 range");
  std::vector<NodePlan> result;
  for (size_t i = 0; i < source.nodes.size(); ++i) {
    if (i % 1024 == 0) checkModelImportCancellation(stop);
    const auto& node = source.nodes[i];
    const auto extension = node.extensions.find(std::string(kExtension));
    if (extension == node.extensions.end()) continue;
    const auto context = label(uint32_t(i));
    if (!listed(source.extensionsUsed, kExtension)) fail(context, "extension must be declared in extensionsUsed");
    if (node.mesh < 0 || size_t(node.mesh) >= source.meshes.size() || uint64_t(node.mesh) > UINT32_MAX)
      fail(context, "instancing requires a valid mesh node");
    const auto& primitives = source.meshes[size_t(node.mesh)].primitives;
    if (primitives.size() > UINT32_MAX) fail(context, "primitive identity exceeds uint32 range");
    const auto& value = extension->second;
    if (!value.IsObject() || !value.Has("attributes") || !value.Get("attributes").IsObject())
      fail(context, "extension must contain an attributes object");
    NodePlan plan{uint32_t(i), uint32_t(node.mesh), uint32_t(primitives.size())};
    for (const auto& [semantic, accessorIndex] : value.Get("attributes").Get<tinygltf::Value::Object>()) {
      const auto attributeLabel = context + "." + semantic;
      if (!accessorIndex.IsInt()) fail(attributeLabel, "accessor index must be an int");
      const auto index = accessorIndex.Get<int>();
      if (index < 0 || size_t(index) >= source.accessors.size()) fail(attributeLabel, "accessor index is outside its table");
      const auto& accessor = source.accessors[size_t(index)];
      attribute(plan, semantic, index, {accessor.type, accessor.componentType, accessor.count, accessor.normalized},
                HasRequiredExtension(source, "KHR_accessor_float64"), budget);
    }
    budget.finish(plan);
    result.push_back(std::move(plan));
  }
  return result;
}
bool finiteAffine(const glm::mat4& matrix) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      if (!std::isfinite(matrix[column][row])) return false;
  return std::abs(matrix[0][3]) <= 1e-5f && std::abs(matrix[1][3]) <= 1e-5f &&
      std::abs(matrix[2][3]) <= 1e-5f && std::abs(matrix[3][3] - 1.f) <= 1e-5f;
}
template<class Document>
InstancingSummary validateJson(Document& source, const InstancingLimits& limits,
                               const std::stop_token& stop, bool& rewritten) {
  try {
    Budget budget(limits);
    auto* nodes = table(source, "nodes");
    const auto* meshes = table(source, "meshes");
    auto* accessors = table(source, "accessors");
    if (!nodes) return budget.summary;
    if (nodes->size() > UINT32_MAX) fail("nodes", "source node identity exceeds uint32 range");

    const auto readField = [&](auto& object, const char* field, uint64_t maximum,
                               const std::string& context, bool required = false) {
      const auto value = object.find(field);
      if (value != object.end()) return integer(*value, maximum, context + "." + field, rewritten);
      if (required) fail(context + "." + field, "field is required");
      return uint64_t{0};
    };
    // The core parser can drop non-integer offsets/strides or narrow an index
    // into another view. Check the original storage references used by instance
    // attributes. CheckedAccessor still validates alignment, ranges and bytes.
    const auto validateStorage = [&](auto& storage, const std::string& context, bool requiredView) {
      readField(storage, "byteOffset", SIZE_MAX, context);
      const auto viewIndex = storage.find("bufferView");
      if (viewIndex == storage.end()) {
        if (requiredView) fail(context + ".bufferView", "field is required");
        return;
      }
      auto* views = table(source, "bufferViews");
      const auto viewContext = context + ".bufferView";
      const auto index = reference(*viewIndex, views ? views->size() : 0, viewContext, rewritten);
      auto& view = views->at(size_t(index));
      const auto bufferIndex = view.find("buffer");
      if (bufferIndex == view.end()) fail(viewContext + ".buffer", "field is required");
      auto* buffers = table(source, "buffers");
      const auto buffer = reference(*bufferIndex, buffers ? buffers->size() : 0, viewContext + ".buffer", rewritten);
      readField(buffers->at(size_t(buffer)), "byteLength", SIZE_MAX, viewContext + ".buffer", true);
      readField(view, "byteLength", SIZE_MAX, viewContext, true);
      readField(view, "byteOffset", SIZE_MAX, viewContext);
      readField(view, "byteStride", SIZE_MAX, viewContext);
      readField(view, "target", INT_MAX, viewContext);
    };

    for (size_t i = 0; i < nodes->size(); ++i) {
      if (i % 1024 == 0) checkModelImportCancellation(stop);
      auto& node = nodes->at(i);
      if (!node.is_object()) fail("nodes", "node must be an object");
      const auto extensions = node.find("extensions");
      if (extensions == node.end()) continue;
      if (!extensions->is_object()) fail(label(uint32_t(i)), "extensions must be an object");
      const auto extension = extensions->find(kExtension);
      if (extension == extensions->end()) continue;
      const auto context = label(uint32_t(i));
      if (!listed(source, "extensionsUsed", kExtension)) fail(context, "extension must be declared in extensionsUsed");
      if (!node.contains("mesh")) fail(context, "instancing requires a valid mesh node");
      const auto mesh = reference(node.at("mesh"), meshes ? meshes->size() : 0, context + ".mesh", rewritten);
      const auto& sourceMesh = meshes->at(size_t(mesh));
      const auto primitives = sourceMesh.find("primitives");
      if (primitives == sourceMesh.end() || !primitives->is_array() || primitives->size() > UINT32_MAX)
        fail(context, "mesh primitive table is invalid");
      if (!extension->is_object() || !extension->contains("attributes") || !extension->at("attributes").is_object())
        fail(context, "extension must contain an attributes object");
      NodePlan plan{uint32_t(i), uint32_t(mesh), uint32_t(primitives->size())};
      for (auto&& [semantic, value] : extension->at("attributes").items()) {
        const auto attributeLabel = context + "." + semantic;
        const auto index = reference(value, accessors ? accessors->size() : 0, attributeLabel, rewritten);
        auto& accessor = accessors->at(size_t(index));
        if (!accessor.contains("type") || !accessor.at("type").is_string())
          fail(attributeLabel, "accessor must specify its shape");
        const auto component = readField(accessor, "componentType", INT_MAX, attributeLabel, true);
        const auto count = readField(accessor, "count", SIZE_MAX, attributeLabel, true);
        if (accessor.contains("normalized") && !accessor.at("normalized").is_boolean()) fail(attributeLabel, "normalized must be boolean");
        attribute(plan, semantic, index, {shape(accessor.at("type").template get<std::string>()), int(component),
            count, accessor.value("normalized", false)},
            listed(source, "extensionsRequired", "KHR_accessor_float64"), budget);
        validateStorage(accessor, attributeLabel, false);
        if (auto sparse = accessor.find("sparse"); sparse != accessor.end()) {
          const auto sparseContext = attributeLabel + ".sparse";
          readField(*sparse, "count", INT_MAX, sparseContext, true);
          for (const char* field : {"indices", "values"}) {
            auto storage = sparse->find(field);
            const auto storageContext = sparseContext + "." + field;
            if (storage == sparse->end() || !storage->is_object()) fail(storageContext, "must be an object");
            if (storage.key() == "indices") readField(*storage, "componentType", INT_MAX, storageContext, true);
            validateStorage(*storage, storageContext, true);
          }
        }
      }
      budget.finish(plan);
    }
    return budget.summary;
  } catch (const Json::exception& error) { fail("metadata", error.what()); }
}
} // namespace

InstancingSummary ValidateMeshGpuInstancingJson(const Json& source, const InstancingLimits& limits,
                                                const std::stop_token& stop) {
  bool rewritten = false;
  return validateJson(source, limits, stop, rewritten);
}

InstancingSummary NormalizeMeshGpuInstancingJson(Json& source, bool& rewritten,
                                                 const InstancingLimits& limits, const std::stop_token& stop) {
  return validateJson(source, limits, stop, rewritten);
}

GltfInstancingData ParseMeshGpuInstancing(const tinygltf::Model& source, const InstancingLimits& limits,
                                         const std::stop_token& stop) {
  Budget budget(limits);
  auto layout = modelLayout(source, budget, stop);
  GltfInstancingData result;
  result.summary = budget.summary;
  result.nodes.reserve(layout.size());
  for (auto& plan : layout) {
    checkModelImportCancellation(stop);
    const auto context = label(plan.node);
    NodeInstancing output{plan.node, plan.mesh, plan.primitives};
    std::vector<CheckedAccessor> readers;
    readers.reserve(plan.attributes.size());
    for (const auto& attribute : plan.attributes) {
      readers.emplace_back(source, attribute.accessor, context + "." + attribute.semantic,
          AccessorReadOptions{AccessorUsage::Generic, HasRequiredExtension(source, "KHR_accessor_float64")},
          stop);
      output.attributes.push_back({attribute.semantic, attribute.accessor});
    }
    output.localMatrices.reserve(size_t(plan.count));
    for (size_t instance = 0; instance < plan.count; ++instance) {
      if (instance % 1024 == 0) checkModelImportCancellation(stop);
      glm::vec3 translation(0), scale(1);
      glm::quat rotation(1,0,0,0);
      for (size_t slot = 0; slot < plan.attributes.size(); ++slot) {
        const auto& attribute = plan.attributes[slot];
        std::array<double,16> values{};
        readers[slot].readDoubles(instance, std::span(values).first(readers[slot].components()));
        if (attribute.semantic == "TRANSLATION") translation = {float(values[0]),float(values[1]),float(values[2])};
        else if (attribute.semantic == "SCALE") scale = {float(values[0]),float(values[1]),float(values[2])};
        else if (attribute.semantic == "ROTATION") {
          const double norm = std::sqrt(values[0]*values[0] + values[1]*values[1] + values[2]*values[2] + values[3]*values[3]);
          const double tolerance = attribute.metadata.component == TINYGLTF_COMPONENT_TYPE_BYTE ? .01 :
              attribute.metadata.component == TINYGLTF_COMPONENT_TYPE_SHORT ? .0001 : .00001;
          if (!std::isfinite(norm) || norm <= 0 || std::abs(norm - 1) > tolerance)
            fail(context + ".ROTATION instance[" + std::to_string(instance) + "]", "quaternion must be finite and unit within storage precision");
          rotation = {float(values[3]/norm),float(values[0]/norm),float(values[1]/norm),float(values[2]/norm)};
        }
      }
      const auto matrix = glm::translate(glm::mat4(1), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1), scale);
      if (!finiteAffine(matrix)) fail(context + " instance[" + std::to_string(instance) + "]", "TRS composition exceeds finite affine float range");
      output.localMatrices.push_back(matrix);
    }
    result.nodes.push_back(std::move(output));
  }
  return result;
}
SourceInstanceIdentity NodeInstancing::identity(uint32_t instance, uint32_t primitive) const {
  if (instance >= localMatrices.size() || primitive >= primitiveCount) fail(label(sourceNode), "instance/primitive identity is out of range");
  return {sourceNode, sourceMesh, primitive, instance};
}
const NodeInstancing* FindNodeInstancing(const GltfInstancingData& data, uint32_t sourceNode) noexcept {
  const auto found = std::lower_bound(data.nodes.begin(), data.nodes.end(), sourceNode,
      [](const auto& node, uint32_t index) { return node.sourceNode < index; });
  return found == data.nodes.end() || found->sourceNode != sourceNode ? nullptr : &*found;
}
glm::mat4 ComposeInstanceWorld(const NodeInstancing& node, uint32_t instance, const glm::mat4& nodeWorld) {
  if (instance >= node.localMatrices.size() || !finiteAffine(nodeWorld) || !finiteAffine(node.localMatrices[instance]))
    fail(label(node.sourceNode), "instance index or world/local affine matrix is invalid");
  const auto result = nodeWorld * node.localMatrices[instance];
  if (!finiteAffine(result)) fail(label(node.sourceNode), "world composition exceeds finite affine float range");
  return result;
}
} // namespace container::geometry::gltf
