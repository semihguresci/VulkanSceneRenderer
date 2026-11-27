#include "RoboticsOpenUsdData.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <glm/common.hpp>
#include <limits>
#include <locale>
#include <map>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string_view>

namespace container::robotics::detail {
namespace {

constexpr std::string_view worldPath = "/World";

std::ostream& indent(std::ostream& out, size_t depth) {
  return out << std::string(depth * 4, ' ');
}

void quoted(std::ostream& out, std::string_view value) {
  constexpr std::string_view hex = "0123456789abcdef";
  out.put('"');
  for (unsigned char ch : value) {
    if (ch == '"' || ch == '\\') {
      out.put('\\').put(static_cast<char>(ch));
    } else if (ch < 0x20 || ch == 0x7f) {
      out << "\\x" << hex[ch >> 4] << hex[ch & 15];
    } else {
      out.put(static_cast<char>(ch));
    }
  }
  out.put('"');
}

template <typename T>
void number(std::ostream& out, T value) {
  if (!std::isfinite(value))
    throw std::invalid_argument("OpenUSD export needs finite geometry and material values");
  std::array<char, 32> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{})
    throw std::runtime_error("OpenUSD export could not format a numeric value");
  out.write(buffer.data(), result.ptr - buffer.data());
}

template <glm::length_t Length, typename T>
void tuple(std::ostream& out, const glm::vec<Length, T>& value) {
  out.put('(');
  for (glm::length_t i = 0; i < Length; ++i) {
    if (i)
      out << ", ";
    number(out, value[i]);
  }
  out.put(')');
}

template <typename WriteValue>
void array(std::ostream& out, size_t depth, size_t count, size_t perLine,
           const WriteValue& writeValue) {
  out.put('[');
  for (size_t index = 0; index < count; ++index) {
    if (index)
      out.put(',');
    if (index % perLine == 0) {
      out.put('\n');
      indent(out, depth + 1);
    } else {
      out.put(' ');
    }
    writeValue(index);
  }
  if (count) {
    out.put('\n');
    indent(out, depth);
  }
  out.put(']');
}

void stringProperty(std::ostream& out, size_t depth, std::string_view property,
                    std::string_view value) {
  indent(out, depth) << "custom string " << property << " = ";
  quoted(out, value);
  out.put('\n');
}

void matrixTransform(std::ostream& out, size_t depth, const glm::dmat4& matrix) {
  indent(out, depth) << "matrix4d xformOp:transform = (";
  // GLM transforms column vectors. USD multiplies row vectors, so the GLM
  // columns are written as USD rows. This places translation in the last row.
  for (glm::length_t row = 0; row < 4; ++row) {
    if (row)
      out << ", ";
    tuple(out, matrix[row]);
  }
  out << ")\n";
  indent(out, depth) << "uniform token[] xformOpOrder = [\"xformOp:transform\"]\n";
}

void meshTopology(std::ostream& out, size_t depth, std::span<const glm::vec3> points,
                  std::span<const uint32_t> indices) {
  if (points.empty() || indices.empty() || indices.size() % 3 != 0 ||
      points.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
      std::ranges::any_of(indices, [&](uint32_t index) { return index >= points.size(); }))
    throw std::invalid_argument("OpenUSD export received invalid triangle topology");

  glm::vec3 low = points.front();
  glm::vec3 high = points.front();
  for (const auto& point : points) {
    low = glm::min(low, point);
    high = glm::max(high, point);
  }
  indent(out, depth) << "float3[] extent = [";
  tuple(out, low);
  out << ", ";
  tuple(out, high);
  out << "]\n";

  indent(out, depth) << "point3f[] points = ";
  array(out, depth, points.size(), 4, [&](size_t i) { tuple(out, points[i]); });
  out.put('\n');
  indent(out, depth) << "int[] faceVertexCounts = ";
  array(out, depth, indices.size() / 3, 24, [&](size_t) { out.put('3'); });
  out.put('\n');
  indent(out, depth) << "int[] faceVertexIndices = ";
  array(out, depth, indices.size(), 12, [&](size_t i) { out << indices[i]; });
  out.put('\n');
  indent(out, depth) << "uniform token subdivisionScheme = \"none\"\n";
  indent(out, depth) << "uniform token orientation = \"rightHanded\"\n";
}

std::string materialName(size_t index) {
  return "Material_" + std::to_string(index);
}

void previewMaterial(std::ostream& out, size_t depth, const OpenUsdMesh& mesh,
                     size_t index) {
  const auto name = materialName(index);
  const std::string path = std::string(worldPath) + "/Materials/" + name;
  const auto& material = mesh.material;
  const auto color = glm::vec3(material.color);
  indent(out, depth) << "def Material \"" << name << "\"\n";
  indent(out, depth) << "{\n";
  ++depth;
  indent(out, depth) << "token outputs:surface.connect = <" << path
                     << "/Preview.outputs:surface>\n";
  indent(out, depth) << "def Shader \"Preview\"\n";
  indent(out, depth) << "{\n";
  indent(out, depth + 1) << "uniform token info:id = \"UsdPreviewSurface\"\n";
  indent(out, depth + 1) << "color3f inputs:diffuseColor";
  if (mesh.displayColors.empty()) {
    out << " = ";
    tuple(out, color);
  } else {
    out << ".connect = <" << path << "/ReadDisplayColor.outputs:result>";
  }
  out.put('\n');
  indent(out, depth + 1) << "float inputs:opacity";
  if (mesh.displayOpacities.empty()) {
    out << " = ";
    number(out, material.color.a);
  } else {
    out << ".connect = <" << path << "/ReadDisplayOpacity.outputs:result>";
  }
  out.put('\n');
  indent(out, depth + 1) << "color3f inputs:emissiveColor = ";
  tuple(out, material.emissive);
  out.put('\n');
  const auto input = [&](std::string_view attribute, float value) {
    indent(out, depth + 1) << "float inputs:" << attribute << " = ";
    number(out, value);
    out.put('\n');
  };
  input("metallic", material.metallic);
  input("roughness", material.roughness);
  input("ior", material.ior);
  if (material.opacityThreshold)
    input("opacityThreshold", *material.opacityThreshold);
  indent(out, depth + 1) << "token outputs:surface\n";
  indent(out, depth) << "}\n";

  const auto reader = [&](std::string_view shader, std::string_view type,
                          std::string_view primvar, const auto& writeFallback) {
    indent(out, depth) << "def Shader \"" << shader << "\"\n";
    indent(out, depth) << "{\n";
    indent(out, depth + 1) << "uniform token info:id = \"UsdPrimvarReader_" << type << "\"\n";
    indent(out, depth + 1) << "string inputs:varname = \"" << primvar << "\"\n";
    indent(out, depth + 1) << type << " inputs:fallback = ";
    writeFallback();
    out.put('\n');
    indent(out, depth + 1) << type << " outputs:result\n";
    indent(out, depth) << "}\n";
  };
  if (!mesh.displayColors.empty())
    reader("ReadDisplayColor", "float3", "displayColor", [&] { tuple(out, color); });
  if (!mesh.displayOpacities.empty())
    reader("ReadDisplayOpacity", "float", "displayOpacity", [&] { number(out, material.color.a); });
  indent(out, depth - 1) << "}\n";
}

void renderMesh(std::ostream& out, size_t depth, const OpenUsdMesh& mesh,
                size_t meshIndex, size_t materialIndex) {
  const auto matchingPointCount = [&](size_t count) {
    return count == 0 || count == mesh.positions.size();
  };
  if (!matchingPointCount(mesh.normals.size()) || !matchingPointCount(mesh.displayColors.size()) ||
      !matchingPointCount(mesh.displayOpacities.size()))
    throw std::invalid_argument("OpenUSD mesh attributes do not match its point count");

  indent(out, depth) << "def Mesh \"Mesh_" << meshIndex << "\" (\n";
  indent(out, depth + 1) << "prepend apiSchemas = [\"MaterialBindingAPI\"]\n";
  indent(out, depth) << ")\n";
  indent(out, depth) << "{\n";
  ++depth;
  matrixTransform(out, depth, mesh.parentFromMesh);
  meshTopology(out, depth, mesh.positions, mesh.indices);
  indent(out, depth) << "uniform bool doubleSided = " << (mesh.doubleSided ? "true" : "false") << '\n';
  stringProperty(out, depth, "robotics:representationId", mesh.representationId);
  indent(out, depth) << "custom uint robotics:sourceMeshId = " << mesh.sourceMeshId << '\n';
  indent(out, depth) << "rel material:binding = <" << worldPath << "/Materials/"
                     << materialName(materialIndex) << ">\n";
  const auto attribute = [&](std::string_view declaration, size_t count, const auto& value) {
    if (count == 0)
      return;
    indent(out, depth) << declaration << " = ";
    array(out, depth, count, 4, value);
    out << " (\n";
    indent(out, depth + 1) << "interpolation = \"vertex\"\n";
    indent(out, depth) << ")\n";
  };
  attribute("normal3f[] normals", mesh.normals.size(), [&](size_t i) { tuple(out, mesh.normals[i]); });
  attribute("color3f[] primvars:displayColor", mesh.displayColors.size(),
            [&](size_t i) { tuple(out, mesh.displayColors[i]); });
  attribute("float[] primvars:displayOpacity", mesh.displayOpacities.size(),
            [&](size_t i) { number(out, mesh.displayOpacities[i]); });
  indent(out, depth - 1) << "}\n";
}

using RelationshipTargets = std::map<std::string_view, std::vector<std::string_view>>;

void entityPrimitives(std::ostream& out, size_t depth, size_t index, const OpenUsdData& data,
                      std::span<const size_t> materialOffsets,
                      std::span<const RelationshipTargets> targets) {
  const auto& node = data.entities[index];
  const auto& entity = data.scene.entities[index];
  const auto kind = semanticKindName(entity.kind);
  indent(out, depth) << "def Xform ";
  quoted(out, node.primName);
  out << " (\n";
  indent(out, depth + 1) << "prepend apiSchemas = [\"SemanticsLabelsAPI:class\"]\n";
  if (!entity.name.empty()) {
    indent(out, depth + 1) << "displayName = ";
    quoted(out, entity.name);
    out.put('\n');
  }
  indent(out, depth) << ")\n";
  indent(out, depth) << "{\n";
  ++depth;
  matrixTransform(out, depth, node.parentFromLocal);
  stringProperty(out, depth, "ifc:guid", entity.guid);
  stringProperty(out, depth, "ifc:class", entity.ifcClass);
  stringProperty(out, depth, "ifc:sourceId", entity.sourceId);
  stringProperty(out, depth, "robotics:entityId", entity.id);
  indent(out, depth) << "custom token robotics:kind = ";
  quoted(out, kind);
  out.put('\n');
  indent(out, depth) << "token[] semantics:labels:class = [";
  quoted(out, kind);
  out << "]\n";
  for (const auto& [relationship, paths] : targets[index]) {
    indent(out, depth) << "custom rel ifc:" << relationship << " = ";
    array(out, depth, paths.size(), 1, [&](size_t i) { out << '<' << paths[i] << '>'; });
    out.put('\n');
  }
  for (size_t i = 0; i < node.meshes.size(); ++i)
    renderMesh(out, depth, node.meshes[i], i, materialOffsets[index] + i);
  for (size_t child : node.children)
    entityPrimitives(out, depth, child, data, materialOffsets, targets);
  indent(out, depth - 1) << "}\n";
}

std::string_view colliderName(BimColliderKind kind) {
  switch (kind) {
  case BimColliderKind::StaticMesh: return "staticMesh";
  case BimColliderKind::DoorMesh: return "doorMesh";
  case BimColliderKind::DynamicBox: return "dynamicBox";
  }
  throw std::invalid_argument("OpenUSD export received an unknown collider recipe");
}

void colliderPrimitive(std::ostream& out, size_t depth, const BimCollider& collider,
                       size_t index, std::string_view ownerPath) {
  const bool box = collider.kind == BimColliderKind::DynamicBox;
  indent(out, depth) << "def " << (box ? "Cube" : "Mesh") << " \"Collider_" << index << "\" (\n";
  indent(out, depth + 1) << "prepend apiSchemas = [\"PhysicsCollisionAPI\"";
  if (!box)
    out << ", \"PhysicsMeshCollisionAPI\"";
  out << "]\n";
  indent(out, depth) << ")\n";
  indent(out, depth) << "{\n";
  ++depth;
  indent(out, depth) << "uniform token purpose = \"guide\"\n";
  indent(out, depth) << "bool physics:collisionEnabled = true\n";
  indent(out, depth) << "rel physics:simulationOwner = <" << worldPath << "/PhysicsScene>\n";
  indent(out, depth) << "custom rel robotics:entity = <" << ownerPath << ">\n";
  indent(out, depth) << "custom token robotics:colliderKind = ";
  quoted(out, colliderName(collider.kind));
  out.put('\n');
  stringProperty(out, depth, "robotics:entityId", collider.entityId);
  stringProperty(out, depth, "robotics:representationId", collider.representationId);
  indent(out, depth) << "custom uint robotics:sourceMeshId = " << collider.sourceMeshId << '\n';
  indent(out, depth) << "custom bool robotics:supportCandidate = "
                     << (collider.supportCandidate ? "true" : "false") << '\n';
  indent(out, depth) << "double3 xformOp:translate = ";
  tuple(out, collider.originMeters);
  out.put('\n');
  if (box) {
    indent(out, depth) << "double size = 2\n";
    indent(out, depth) << "float3[] extent = [(-1, -1, -1), (1, 1, 1)]\n";
    indent(out, depth) << "float3 xformOp:scale = ";
    tuple(out, collider.halfExtents);
    out.put('\n');
    indent(out, depth) << "uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:scale\"]\n";
  } else {
    if (!collider.mesh)
      throw std::invalid_argument("OpenUSD mesh collider is missing its cooked geometry");
    meshTopology(out, depth, collider.mesh->vertices, collider.mesh->indices);
    indent(out, depth) << "uniform token physics:approximation = \"none\"\n";
    indent(out, depth) << "uniform token[] xformOpOrder = [\"xformOp:translate\"]\n";
  }
  indent(out, depth - 1) << "}\n";
}

} // namespace

void writeOpenUsdLayer(std::ostream& out, const OpenUsdData& data) {
  out.imbue(std::locale::classic());

  std::map<std::string_view, size_t, std::less<>> entityIndices;
  for (size_t i = 0; i < data.scene.entities.size(); ++i)
    entityIndices.emplace(data.scene.entities[i].id, i);
  std::vector<RelationshipTargets> targets(data.entities.size());
  for (const auto& relationship : data.scene.relationships) {
    auto name = semanticRelationshipKindName(relationship.kind);
    if (relationship.kind == SemanticRelationshipKind::SpaceBoundary)
      name = "spaceBoundary";
    const auto from = entityIndices.at(relationship.from);
    const auto to = entityIndices.at(relationship.to);
    targets[from][name].push_back(data.entities[to].primPath);
  }

  out << "#usda 1.0\n"
         "(\n"
         "    defaultPrim = \"World\"\n"
         "    upAxis = \"Z\"\n"
         "    metersPerUnit = 1\n"
         "    kilogramsPerUnit = 1\n"
         ")\n\n"
         "def Xform \"World\"\n"
         "{\n";
  stringProperty(out, 1, "robotics:sourceNamespace", data.scene.sourceNamespace);
  indent(out, 1) << "custom string[] robotics:diagnostics = ";
  array(out, 1, data.diagnostics.size(), 1, [&](size_t i) {
    const auto& diagnostic = data.diagnostics[i];
    quoted(out, diagnostic.code + " [" + diagnostic.sourceId + "]: " + diagnostic.message);
  });
  out << "\n\n";

  out << "    def Scope \"Materials\"\n"
         "    {\n";
  size_t materialIndex = 0;
  std::vector<size_t> materialOffsets;
  materialOffsets.reserve(data.entities.size());
  for (const auto& entity : data.entities) {
    materialOffsets.push_back(materialIndex);
    for (const auto& mesh : entity.meshes)
      previewMaterial(out, 2, mesh, materialIndex++);
  }
  const size_t standaloneMaterials = materialIndex;
  for (const auto& mesh : data.standaloneMeshes)
    previewMaterial(out, 2, mesh, materialIndex++);
  out << "    }\n";

  for (size_t root : data.roots)
    entityPrimitives(out, 1, root, data, materialOffsets, targets);

  if (!data.standaloneMeshes.empty()) {
    out << "    def Scope \"Unclassified\"\n"
           "    {\n";
    for (size_t i = 0; i < data.standaloneMeshes.size(); ++i)
      renderMesh(out, 2, data.standaloneMeshes[i], i, standaloneMaterials + i);
    out << "    }\n";
  }

  if (!data.colliders.empty()) {
    out << "    def PhysicsScene \"PhysicsScene\"\n"
           "    {\n"
           "        vector3f physics:gravityDirection = (0, 0, -1)\n"
           "        float physics:gravityMagnitude = 9.81\n"
           "    }\n"
           "    def Scope \"Colliders\"\n"
           "    {\n";
    for (size_t i = 0; i < data.colliders.size(); ++i) {
      const auto& collider = data.colliders[i];
      const auto& owner = data.entities[entityIndices.at(collider.entityId)];
      colliderPrimitive(out, 2, collider, i, owner.primPath);
    }
    out << "    }\n";
  }
  out << "}\n";
  if (!out)
    throw std::runtime_error("Failed to write the OpenUSD layer");
}

} // namespace container::robotics::detail
