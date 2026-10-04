#pragma once

#include "Container/common/CommonMath.h"
#include "Container/geometry/ImportReport.h"
#include "Container/geometry/Vertex.h"
#include "Container/utility/Material.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace container::geometry::dotbim {

struct MeshRange {
  uint32_t meshId{0};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  glm::vec3 boundsCenter{0.0f};
  float boundsRadius{0.0f};
};

struct NativePrimitiveRange {
  uint32_t meshId{0};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  glm::vec3 boundsCenter{0.0f};
  float boundsRadius{0.0f};
};

struct MeshletClusterRange {
  uint32_t meshId{0};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  uint32_t triangleCount{0};
  uint32_t lodLevel{0};
  glm::vec3 boundsCenter{0.0f};
  float boundsRadius{0.0f};
};

enum class GeometryKind : uint8_t {
  Mesh = 0,
  Points = 1,
  Curves = 2,
};

struct ElementProperty {
  std::string set{};
  std::string name{};
  std::string value{};
  std::string category{};
};

struct Element {
  uint32_t meshId{0};
  GeometryKind geometryKind{GeometryKind::Mesh};
  glm::mat4 transform{1.0f};
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
  std::string guid{};
  std::string type{};
  std::string displayName{};
  std::string objectType{};
  std::string storeyName{};
  std::string storeyId{};
  std::string materialName{};
  std::string materialCategory{};
  std::string discipline{};
  std::string phase{};
  std::string fireRating{};
  std::string loadBearing{};
  std::string status{};
  std::string sourceId{};
  std::vector<ElementProperty> properties{};
  bool doubleSided{true};
  uint32_t materialIndex{std::numeric_limits<uint32_t>::max()};
};

struct MaterialTextureAsset {
  std::filesystem::path path{};
  std::string name{};
  std::vector<std::byte> encodedBytes{};
  uint32_t samplerIndex{0};

  [[nodiscard]] bool empty() const noexcept {
    return path.empty() && encodedBytes.empty();
  }
};

struct MaterialTexturePaths {
  MaterialTextureAsset baseColor{};
  MaterialTextureAsset normal{};
  MaterialTextureAsset occlusion{};
  MaterialTextureAsset emissive{};
  MaterialTextureAsset metallicRoughness{};
  MaterialTextureAsset roughness{};
  MaterialTextureAsset metalness{};
  MaterialTextureAsset specular{};
  MaterialTextureAsset specularColor{};
  MaterialTextureAsset opacity{};
  MaterialTextureAsset transmission{};
  MaterialTextureAsset clearcoat{};
  MaterialTextureAsset clearcoatRoughness{};
  MaterialTextureAsset clearcoatNormal{};
  MaterialTextureAsset sheenColor{};
  MaterialTextureAsset sheenRoughness{};
  MaterialTextureAsset iridescence{};
  MaterialTextureAsset iridescenceThickness{};
};

struct Material {
  container::material::Material pbr{};
  MaterialTexturePaths texturePaths{};
};

struct ModelUnitMetadata {
  bool hasSourceUnits{false};
  std::string sourceUnits{};
  bool hasMetersPerUnit{false};
  float metersPerUnit{1.0f};
  bool hasImportScale{false};
  float importScale{1.0f};
  bool hasEffectiveImportScale{false};
  float effectiveImportScale{1.0f};
};

struct ModelGeoreferenceMetadata {
  bool hasSourceUpAxis{false};
  std::string sourceUpAxis{};
  bool hasCoordinateOffset{false};
  glm::dvec3 coordinateOffset{0.0};
  std::string coordinateOffsetSource{};
  std::string crsName{};
  std::string crsAuthority{};
  std::string crsCode{};
  std::string mapConversionName{};
};

struct ElementRelationship {
  std::string fromGuid{};
  std::string fromSourceId{};
  std::string toGuid{};
  std::string toSourceId{};
  std::string kind{};
  std::string label{};
};

struct Model {
  container::geometry::ImportReport importReport{};
  std::vector<Vertex> vertices{};
  std::vector<uint32_t> indices{};
  std::vector<MeshRange> meshRanges{};
  std::vector<NativePrimitiveRange> nativePointRanges{};
  std::vector<NativePrimitiveRange> nativeCurveRanges{};
  std::vector<MeshletClusterRange> meshletClusters{};
  std::vector<Element> elements{};
  std::vector<Material> materials{};
  std::vector<ElementRelationship> relationships{};
  ModelUnitMetadata unitMetadata{};
  ModelGeoreferenceMetadata georeferenceMetadata{};
};

[[nodiscard]] Model LoadFromFile(const std::filesystem::path& path,
                                 float importScale = 1.0f);
[[nodiscard]] Model LoadFromJson(std::string_view jsonText,
                                 float importScale = 1.0f);

}  // namespace container::geometry::dotbim
