#include "RoboticsOpenUsdData.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/robotics/BimSensorLabels.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace container::robotics::detail {
namespace {

namespace bim = geometry::dotbim;
constexpr auto noMaterial = std::numeric_limits<uint32_t>::max();

template <glm::length_t Length, typename T, glm::qualifier Qualifier>
bool finite(const glm::vec<Length, T, Qualifier>& value) {
  for (glm::length_t axis = 0; axis < Length; ++axis)
    if (!std::isfinite(value[axis])) return false;
  return true;
}

bool finite(const glm::dmat4& value) {
  for (glm::length_t column = 0; column < 4; ++column)
    if (!finite(value[column])) return false;
  return true;
}

std::optional<glm::dmat4> authoredMetricPlacement(const bim::AuthoredDrawablePlacement& placement,
                                                 const glm::dmat4& roboticsFromRenderer) {
  if (!placement.rendererWorldFromLocal)
    return std::nullopt;
  const auto metric = roboticsFromRenderer * *placement.rendererWorldFromLocal;
  if (!finite(metric) || metric[0][3] != 0.0 || metric[1][3] != 0.0 ||
      metric[2][3] != 0.0 || metric[3][3] != 1.0)
    return std::nullopt;
  const auto determinant = glm::determinant(glm::dmat3(metric));
  if (!std::isfinite(determinant) || determinant == 0.0)
    return std::nullopt;
  return metric;
}

void diagnose(OpenUsdData& data, const bim::Element& element, std::string_view code,
              std::string_view message) {
  data.diagnostics.push_back({std::string(code), element.sourceId, std::string(message)});
}

bool hasTextures(const bim::Material& source) {
  using Paths = bim::MaterialTexturePaths;
  constexpr std::array paths{
      &Paths::baseColor, &Paths::normal, &Paths::occlusion, &Paths::emissive,
      &Paths::metallicRoughness, &Paths::roughness, &Paths::metalness, &Paths::specular,
      &Paths::specularColor, &Paths::opacity, &Paths::transmission, &Paths::clearcoat,
      &Paths::clearcoatRoughness, &Paths::clearcoatNormal, &Paths::sheenColor,
      &Paths::sheenRoughness, &Paths::iridescence, &Paths::iridescenceThickness};
  if (std::ranges::any_of(paths, [&](auto path) { return !(source.texturePaths.*path).empty(); }))
    return true;

  using Pbr = material::Material;
  constexpr std::array indices{
      &Pbr::baseColorTextureIndex, &Pbr::normalTextureIndex, &Pbr::occlusionTextureIndex,
      &Pbr::emissiveTextureIndex, &Pbr::metallicRoughnessTextureIndex,
      &Pbr::roughnessTextureIndex, &Pbr::metalnessTextureIndex, &Pbr::specularTextureIndex,
      &Pbr::heightTextureIndex, &Pbr::opacityTextureIndex, &Pbr::specularColorTextureIndex,
      &Pbr::transmissionTextureIndex, &Pbr::clearcoatTextureIndex,
      &Pbr::clearcoatRoughnessTextureIndex, &Pbr::clearcoatNormalTextureIndex,
      &Pbr::thicknessTextureIndex, &Pbr::sheenColorTextureIndex, &Pbr::sheenRoughnessTextureIndex,
      &Pbr::iridescenceTextureIndex, &Pbr::iridescenceThicknessTextureIndex,
      &Pbr::anisotropyTextureIndex, &Pbr::diffuseTransmissionTextureIndex,
      &Pbr::diffuseTransmissionColorTextureIndex};
  return std::ranges::any_of(indices, [&](auto index) { return source.pbr.*index != noMaterial; }) ||
         std::ranges::any_of(source.pbr.experimental.textureResources(),
                             [](auto index) { return index != noMaterial; });
}

bool hasUnsupportedSurface(const material::Material& pbr) {
  const material::Material defaults;
  return pbr.unlit || pbr.specularGlossinessWorkflow ||
         pbr.specularFactor != defaults.specularFactor ||
         pbr.specularColorFactor != defaults.specularColorFactor ||
         pbr.transmissionFactor != 0.0f || pbr.diffuseTransmissionFactor != 0.0f ||
         pbr.dispersion != 0.0f || pbr.clearcoatFactor != 0.0f || pbr.thicknessFactor != 0.0f ||
         pbr.sheenColorFactor != defaults.sheenColorFactor || pbr.iridescenceFactor != 0.0f ||
         pbr.anisotropyStrength != 0.0f || pbr.experimental.retroreflection != 0.0f ||
         pbr.experimental.scatterStrength != 0.0f;
}

// The same fallback as BIM rendering uses the occurrence color when there is
// no indexed material. An authored opaque material explicitly ignores alpha.
bool setMaterial(const bim::Model& model, const bim::Element& element,
                 OpenUsdMesh& mesh, OpenUsdData& data) {
  mesh.doubleSided = element.doubleSided;
  const auto fallback = [&] {
    mesh.material.color = finite(element.color)
        ? glm::clamp(element.color, 0.0f, 1.0f) : glm::vec4(1.0f);
    return true;
  };
  if (element.materialIndex == noMaterial) {
    if (!finite(element.color))
      diagnose(data, element, "invalid-visual-material", "The occurrence color is nonfinite. A neutral color was exported.");
    return fallback();
  }
  if (element.materialIndex >= model.materials.size()) {
    diagnose(data, element, "invalid-visual-material", "The indexed material is absent. The occurrence color was exported.");
    return fallback();
  }

  const auto& source = model.materials[element.materialIndex];
  const auto& pbr = source.pbr;
  if (hasTextures(source))
    diagnose(data, element, "unsupported-visual-textures", "Texture maps are omitted from the USD preview material.");
  if (hasUnsupportedSurface(pbr))
    diagnose(data, element, "unsupported-visual-material-features",
             "The USD preview material reduces the authored surface to the supported basic PBR factors.");

  mesh.doubleSided = mesh.doubleSided || pbr.doubleSided;
  const auto emission = pbr.emissiveColor * pbr.emissiveStrength;
  if (!finite(pbr.baseColor) || !finite(emission) || !std::isfinite(pbr.opacityFactor) ||
      !std::isfinite(pbr.metallicFactor) || !std::isfinite(pbr.roughnessFactor) ||
      !std::isfinite(pbr.ior) || !std::isfinite(pbr.alphaCutoff)) {
    diagnose(data, element, "invalid-visual-material", "Nonfinite indexed material factors were replaced by the occurrence color.");
    return fallback();
  }

  auto& preview = mesh.material;
  preview.color = glm::clamp(pbr.baseColor, 0.0f, 1.0f);
  const bool usesAlpha = pbr.alphaMode != material::AlphaMode::Opaque;
  preview.color.a = usesAlpha ? preview.color.a * std::clamp(pbr.opacityFactor, 0.0f, 1.0f) : 1.0f;
  preview.emissive = glm::max(emission, glm::vec3(0.0f));
  preview.metallic = std::clamp(pbr.metallicFactor, 0.0f, 1.0f);
  preview.roughness = std::clamp(pbr.roughnessFactor, 0.0f, 1.0f);
  preview.ior = std::max(pbr.ior, 1.0f);
  if (pbr.alphaMode == material::AlphaMode::Mask)
    preview.opacityThreshold = std::clamp(pbr.alphaCutoff, 0.0f, 1.0f);
  return usesAlpha;
}

bool populateMesh(const bim::Model& model, const bim::Element& element,
                  const bim::MeshRange* range, const glm::dmat4& worldFromMesh,
                  OpenUsdMesh& mesh, OpenUsdData& data) {
  if (!range || range->firstIndex > model.indices.size() || range->indexCount == 0 ||
      range->indexCount % 3 != 0 || range->indexCount > model.indices.size() - range->firstIndex) {
    diagnose(data, element, "invalid-visual-mesh-range", "The visual mesh range is absent, ambiguous or not a valid triangle range.");
    return false;
  }
  if (!finite(worldFromMesh) || !finite(mesh.parentFromMesh)) {
    diagnose(data, element, "invalid-visual-placement", "The visual mesh placement cannot be represented by a finite USD matrix.");
    return false;
  }

  const bool usesAlpha = setMaterial(model, element, mesh, data);
  const auto constantColor = glm::vec3(mesh.material.color);
  const float constantOpacity = mesh.material.color.a;
  std::unordered_map<uint32_t, uint32_t> compactIds;
  mesh.indices.reserve(range->indexCount);
  bool invalidNormal = false;
  bool invalidColor = false;
  bool varyingColor = false;
  bool varyingOpacity = false;
  for (size_t offset = 0; offset < range->indexCount; ++offset) {
    const uint32_t sourceIndex = model.indices[static_cast<size_t>(range->firstIndex) + offset];
    if (sourceIndex >= model.vertices.size()) {
      diagnose(data, element, "invalid-visual-mesh-index", "The visual mesh refers to a vertex outside the imported vertex array.");
      return false;
    }
    const auto [entry, inserted] = compactIds.try_emplace(sourceIndex, static_cast<uint32_t>(mesh.positions.size()));
    if (inserted) {
      if (mesh.positions.size() >= static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        diagnose(data, element, "visual-mesh-too-large", "The visual mesh exceeds the USD signed vertex index range.");
        return false;
      }
      const auto& vertex = model.vertices[sourceIndex];
      if (!finite(vertex.position) || !finite(worldFromMesh * glm::dvec4(vertex.position, 1.0))) {
        diagnose(data, element, "invalid-visual-mesh-position", "The visual mesh contains a nonfinite local or metric position.");
        return false;
      }
      mesh.positions.push_back(vertex.position);
      mesh.normals.push_back(vertex.normal);
      invalidNormal |= !finite(vertex.normal) || vertex.normal == glm::vec3(0.0f);

      const bool validColor = finite(vertex.color);
      const bool validAlpha = std::isfinite(vertex.colorAlpha);
      invalidColor |= !validColor || (usesAlpha && !validAlpha);
      const auto color = validColor ? constantColor * glm::clamp(vertex.color, 0.0f, 1.0f) : constantColor;
      const float opacity = usesAlpha && validAlpha
          ? constantOpacity * std::clamp(vertex.colorAlpha, 0.0f, 1.0f) : constantOpacity;
      mesh.displayColors.push_back(color);
      mesh.displayOpacities.push_back(opacity);
      varyingColor |= color != constantColor;
      varyingOpacity |= opacity != constantOpacity;
    }
    // Source order is the local right-handed winding. A retained affine
    // reflection changes the effective orientation in the USD consumer.
    mesh.indices.push_back(entry->second);
  }
  if (!varyingColor) mesh.displayColors.clear();
  if (!varyingOpacity) mesh.displayOpacities.clear();
  if (invalidNormal) {
    mesh.normals.clear();
    diagnose(data, element, "invalid-visual-mesh-normal", "Invalid authored normals were omitted to allow the USD consumer to derive them.");
  }
  if (invalidColor)
    diagnose(data, element, "invalid-visual-vertex-color", "Nonfinite vertex color or opacity factors were replaced by the material constants.");
  return true;
}

void sortMeshes(std::vector<OpenUsdMesh>& meshes) {
  std::ranges::stable_sort(meshes, [](const auto& first, const auto& second) {
    return std::tie(first.representationId, first.sourceMeshId) <
           std::tie(second.representationId, second.sourceMeshId);
  });
}

} // namespace

void appendOpenUsdMeshes(const bim::Model& model, OpenUsdData& data) {
  const auto labels = buildBimSensorLabels(model, data.scene);
  data.diagnostics.insert(data.diagnostics.end(), labels.diagnostics.begin(), labels.diagnostics.end());
  const auto& roboticsFromRenderer = *data.scene.frame.roboticsFromRenderer;

  std::map<std::string_view, size_t> entities;
  std::vector<glm::dmat4> entityFromWorld;
  entityFromWorld.reserve(data.entities.size());
  for (size_t index = 0; index < data.scene.entities.size(); ++index) {
    entities.emplace(data.scene.entities[index].id, index);
    entityFromWorld.push_back(glm::affineInverse(data.entities[index].worldFromLocal));
  }

  std::map<uint32_t, const bim::MeshRange*> ranges;
  for (const auto& range : model.meshRanges) {
    const auto [entry, inserted] = ranges.try_emplace(range.meshId, &range);
    if (!inserted) entry->second = nullptr;
  }

  std::unordered_set<const SemanticRepresentation*> matchedRepresentations;
  for (size_t index = 0; index < model.elements.size(); ++index) {
    const auto& element = model.elements[index];
    if (element.geometryKind != bim::GeometryKind::Mesh) {
      diagnose(data, element, "unsupported-visual-representation",
               "Native point and curve representations are outside the USD triangle mesh export scope.");
      continue;
    }

    const uint32_t instanceId = labels.elements[index].instanceId;
    if (instanceId == 0 || instanceId > labels.instances.size()) {
      diagnose(data, element, "visual-mesh-owner-unavailable",
               "The visual mesh has no unambiguous stable semantic or standalone representation identity.");
      continue;
    }
    const auto authoredWorld = element.authoredPlacement
        ? authoredMetricPlacement(*element.authoredPlacement, roboticsFromRenderer) : std::nullopt;
    const bool invalidAuthoredPlacement = element.authoredPlacement && !authoredWorld;
    if (invalidAuthoredPlacement)
      diagnose(data, element, "visual-metric-placement-unavailable",
               "The IFCX drawable has an invalid authored placement or inherits one from an ancestor.");

    const auto foundRange = ranges.find(element.meshId);
    const auto* range = foundRange == ranges.end() ? nullptr : foundRange->second;
    OpenUsdMesh mesh{.representationId = element.sourceId, .sourceMeshId = element.meshId};
    if (element.isRepresentationOnly) {
      if (invalidAuthoredPlacement) continue;
      const auto worldFromMesh = authoredWorld.value_or(roboticsFromRenderer * glm::dmat4(element.transform));
      mesh.parentFromMesh = worldFromMesh;
      if (populateMesh(model, element, range, worldFromMesh, mesh, data)) {
        diagnose(data, element, "standalone-visual-mesh-no-collider",
                 "The standalone mesh has no authored semantic product or cooked collision recipe.");
        data.standaloneMeshes.push_back(std::move(mesh));
      }
      continue;
    }

    const auto entityIndex = entities.at(labels.instances[instanceId - 1].entityId);
    auto& entity = data.scene.entities[entityIndex];
    const auto rendererPlacement = glm::dmat4(element.transform);
    const auto representation = std::ranges::find_if(entity.representations, [&](const auto& row) {
      return row.sourceId == element.sourceId && row.meshId == element.meshId &&
             row.geometryKind == SemanticGeometryKind::Mesh &&
             row.rendererWorldFromLocal == rendererPlacement && !matchedRepresentations.contains(&row);
    });
    if (representation == entity.representations.end()) {
      diagnose(data, element, "visual-metric-placement-unavailable",
               "The authoritative mesh representation has no verified metric placement.");
      continue;
    }
    matchedRepresentations.insert(&*representation);
    // Keep the float transform used for row matching and refine only the
    // USD-owned metric recipe, which is also consumed by collision cooking.
    if (element.authoredPlacement)
      representation->roboticsWorldFromLocal = authoredWorld;
    if (!representation->roboticsWorldFromLocal) {
      if (!invalidAuthoredPlacement)
        diagnose(data, element, "visual-metric-placement-unavailable",
                 "The authoritative mesh representation has no verified metric placement.");
      continue;
    }
    const auto& worldFromMesh = *representation->roboticsWorldFromLocal;
    mesh.parentFromMesh = entityFromWorld[entityIndex] * worldFromMesh;
    if (populateMesh(model, element, range, worldFromMesh, mesh, data))
      data.entities[entityIndex].meshes.push_back(std::move(mesh));
  }

  for (auto& entity : data.entities) sortMeshes(entity.meshes);
  sortMeshes(data.standaloneMeshes);
}

} // namespace container::robotics::detail
