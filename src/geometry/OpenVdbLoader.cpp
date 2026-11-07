#include "Container/geometry/OpenVdbLoader.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/tools/VolumeToMesh.h>
#endif

namespace container::geometry::vdb {

bool IsAvailable() noexcept {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  return true;
#else
  return false;
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace {

void initializeOpenVdb() {
  static std::once_flag initialized;
  std::call_once(initialized, [] { openvdb::initialize(); });
}

[[noreturn]] void gridError(const std::string &name,
                            const std::string &reason) {
  throw std::runtime_error("OpenVDB grid '" + name + "': " + reason);
}

bool finite(const glm::dvec3 &value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

glm::dvec3 asVector(const openvdb::Vec3d &value) {
  return {value.x(), value.y(), value.z()};
}

// VolumeToMesh returns world positions but establishes winding in index space.
// This only measures the map handedness; appendSurface also converts OpenVDB's
// clockwise polygon convention to the renderer's counterclockwise convention.
bool reflectedAt(const openvdb::math::Transform &transform,
                 const openvdb::Vec3d &indexPoint, const std::string &name) {
  const glm::dvec3 origin = asVector(transform.indexToWorld(indexPoint));
  const glm::dvec3 x =
      asVector(transform.indexToWorld(indexPoint + openvdb::Vec3d(1, 0, 0))) -
      origin;
  const glm::dvec3 y =
      asVector(transform.indexToWorld(indexPoint + openvdb::Vec3d(0, 1, 0))) -
      origin;
  const glm::dvec3 z =
      asVector(transform.indexToWorld(indexPoint + openvdb::Vec3d(0, 0, 1))) -
      origin;
  const double determinant = glm::dot(glm::cross(x, y), z);
  if (!finite(origin) || !finite(x) || !finite(y) || !finite(z) ||
      !std::isfinite(determinant) || determinant == 0.0) {
    gridError(name, "the grid transform is non-finite or singular");
  }
  return determinant < 0.0;
}

void warnSkipped(dotbim::Model &model, uint32_t ordinal,
                 const std::string &name, const std::string &category,
                 const std::string &reason) {
  auto &report = model.importReport;
  ++report.skippedProductCount;
  ++report.representationWarnings[category];
  ImportDiagnostic diagnostic{};
  diagnostic.representationType = category;
  diagnostic.entityId = ordinal;
  diagnostic.productId = ordinal;
  diagnostic.productGuid = "vdb:" + std::to_string(ordinal) + ":" + name;
  diagnostic.reason = "Grid '" + name + "': " + reason;
  report.diagnostics.push_back(std::move(diagnostic));
}

template <typename Grid>
bool appendSurface(dotbim::Model &model, const Grid &grid,
                   const std::string &name, uint32_t ordinal, float importScale,
                   const ImportOptions &options) {
  const uint64_t active = grid.activeVoxelCount();
  if (active > options.maxActiveVoxels) {
    gridError(name, "active voxel count " + std::to_string(active) +
                        " exceeds maxActiveVoxels (" +
                        std::to_string(options.maxActiveVoxels) +
                        "); select a smaller grid or raise the explicit limit");
  }
  constexpr uint64_t leafVoxels = Grid::TreeType::LeafNodeType::NUM_VALUES;
  if (uint64_t(grid.tree().leafCount()) >
          options.maxActiveVoxels / leafVoxels +
              (options.maxActiveVoxels % leafVoxels != 0) ||
      uint64_t(grid.memUsage()) > options.maxInputBytes) {
    gridError(name, "stored topology exceeds the voxel or input-memory budget");
  }
  if (!std::isfinite(static_cast<double>(grid.background()))) {
    gridError(name, "the background value is not finite");
  }
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    if (!std::isfinite(static_cast<double>(*value))) {
      gridError(name, "a scalar value is not finite");
    }
  }
  const bool reflected = reflectedAt(grid.transform(), openvdb::Vec3d(0), name);
  const double isoValue = options.isoValue.value_or(
      grid.getGridClass() == openvdb::GRID_LEVEL_SET ? 0.0 : 0.5);
  std::vector<openvdb::Vec3s> points;
  std::vector<openvdb::Vec3I> triangles;
  std::vector<openvdb::Vec4I> quads;
  openvdb::tools::volumeToMesh(grid, points, triangles, quads, isoValue,
                               options.adaptivity);

  const size_t remaining = options.maxTriangles - model.indices.size() / 3u;
  if (triangles.size() > remaining ||
      quads.size() > (remaining - triangles.size()) / 2u) {
    gridError(name, "extracted surface exceeds maxTriangles (" +
                        std::to_string(options.maxTriangles) +
                        "); increase adaptivity or raise the explicit limit");
  }
  const size_t triangleCount = triangles.size() + quads.size() * 2u;
  const size_t indexLimit = std::numeric_limits<uint32_t>::max();
  if (triangleCount > (indexLimit - model.vertices.size()) / 3u ||
      triangleCount > (indexLimit - model.indices.size()) / 3u) {
    gridError(name, "the surface exceeds the renderer's 32-bit index capacity");
  }
  if (triangleCount == 0) {
    warnSkipped(model, ordinal, name, "OpenVDB.EmptySurface",
                "no surface crosses isovalue " + std::to_string(isoValue));
    return false;
  }

  std::vector<glm::vec3> positions;
  positions.reserve(points.size());
  for (const auto &point : points) {
    const glm::dvec3 scaled =
        glm::dvec3(point.x(), point.y(), point.z()) * double(importScale);
    const double maxFloat = std::numeric_limits<float>::max();
    if (!finite(scaled) || std::abs(scaled.x) > maxFloat ||
        std::abs(scaled.y) > maxFloat || std::abs(scaled.z) > maxFloat) {
      gridError(name, "a surface position is outside the finite float range");
    }
    positions.emplace_back(scaled);
  }

  dotbim::MeshRange range{};
  range.meshId = ordinal;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());
  const size_t firstVertex = model.vertices.size();
  glm::dvec3 boundsMin(std::numeric_limits<double>::max());
  glm::dvec3 boundsMax(std::numeric_limits<double>::lowest());
  size_t degenerateTriangles = 0;
  auto appendTriangle = [&](uint32_t a, uint32_t b, uint32_t c) {
    if (a >= positions.size() || b >= positions.size() ||
        c >= positions.size()) {
      gridError(name, "the mesher returned an invalid point index");
    }
    // OpenVDB already adjusts inside classification for level sets versus fog
    // using gridClass. Its outward normal is cross(v2-v0, v1-v0), so reverse
    // every polygon for our cross(v1-v0, v2-v0), then account for reflections.
    bool reverse = !reflected;
    if (!grid.transform().isLinear()) {
      const openvdb::Vec3d centroid =
          (openvdb::Vec3d(points[a]) + openvdb::Vec3d(points[b]) +
           openvdb::Vec3d(points[c])) /
          3.0;
      reverse = !reflectedAt(grid.transform(),
                             grid.transform().worldToIndex(centroid), name);
    }
    if (reverse)
      std::swap(b, c);
    const glm::dvec3 pa(positions[a]), pb(positions[b]), pc(positions[c]);
    const glm::dvec3 cross = glm::cross(pb - pa, pc - pa);
    const double length = glm::length(cross);
    if (!std::isfinite(length)) {
      gridError(name, "a triangle normal is not finite");
    }
    if (length == 0.0) {
      ++degenerateTriangles;
      return;
    }
    const glm::vec3 normal(cross / length);
    const glm::vec3 axis =
        std::abs(normal.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const glm::vec3 tangent = glm::normalize(glm::cross(axis, normal));
    for (uint32_t pointIndex : {a, b, c}) {
      Vertex vertex{};
      vertex.position = positions[pointIndex];
      vertex.color = {0.72f, 0.8f, 0.87f};
      vertex.normal = normal;
      vertex.tangent = glm::vec4(tangent, 1.0f);
      model.indices.push_back(static_cast<uint32_t>(model.vertices.size()));
      model.vertices.push_back(vertex);
      boundsMin = glm::min(boundsMin, glm::dvec3(vertex.position));
      boundsMax = glm::max(boundsMax, glm::dvec3(vertex.position));
    }
  };
  for (const auto &triangle : triangles) {
    appendTriangle(triangle[0], triangle[1], triangle[2]);
  }
  for (const auto &quad : quads) {
    appendTriangle(quad[0], quad[1], quad[2]);
    appendTriangle(quad[0], quad[2], quad[3]);
  }
  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount == 0) {
    warnSkipped(model, ordinal, name, "OpenVDB.EmptySurface",
                "all extracted triangles are degenerate");
    return false;
  }
  const glm::dvec3 center = (boundsMin + boundsMax) * 0.5;
  double radius = 0.0;
  for (size_t i = firstVertex; i < model.vertices.size(); ++i) {
    radius = std::max(
        radius, glm::length(glm::dvec3(model.vertices[i].position) - center));
  }
  if (!finite(center) || !std::isfinite(radius) ||
      radius > std::numeric_limits<float>::max()) {
    gridError(name, "surface bounds are outside the finite float range");
  }
  range.boundsCenter = glm::vec3(center);
  range.boundsRadius = static_cast<float>(radius);
  model.meshRanges.push_back(range);

  dotbim::Element element{};
  element.meshId = range.meshId;
  element.guid = "vdb:" + std::to_string(ordinal) + ":" + name;
  element.sourceId = name;
  element.displayName = grid.getName().empty() ? name : grid.getName();
  element.type = grid.getGridClass() == openvdb::GRID_LEVEL_SET
                     ? "OpenVDB Level Set"
                     : "OpenVDB Scalar Volume";
  element.materialName = "OpenVDB Surface";
  element.materialIndex = 0;
  element.color = {0.72f, 0.8f, 0.87f, 1.0f};
  element.doubleSided = false;
  element.properties = {
      {"OpenVDB", "Grid name", name, ""},
      {"OpenVDB", "Value type", grid.valueType(), ""},
      {"OpenVDB", "Isovalue", std::to_string(isoValue), ""},
      {"OpenVDB", "Active voxels", std::to_string(active), ""},
      {"OpenVDB", "Adaptivity", std::to_string(options.adaptivity), ""}};
  model.elements.push_back(std::move(element));
  ++model.importReport.importedProductCount;
  if (degenerateTriangles != 0) {
    ++model.importReport.partialProductCount;
    ++model.importReport.representationWarnings["OpenVDB.DegenerateTriangles"];
    model.importReport.diagnostics.push_back(
        {"OpenVDB.DegenerateTriangles", ordinal, ordinal,
         "vdb:" + std::to_string(ordinal) + ":" + name,
         "Grid '" + name + "': omitted " + std::to_string(degenerateTriangles) +
             " degenerate triangles"});
  }
  return true;
}

long double metadataBytes(const openvdb::MetaMap& map) {
  long double bytes = sizeof(openvdb::MetaMap);
  for (auto field = map.beginMeta(); field != map.endMeta(); ++field)
    bytes += 4.0L * (field->first.size() + field->second->typeName().size() +
                     field->second->size()) + 256;
  return bytes;
}

void mergePointPreview(dotbim::Model& model, dotbim::Model preview,
                       const std::vector<std::pair<std::string, openvdb::GridBase::Ptr>>& grids) {
  const auto limit = std::numeric_limits<uint32_t>::max();
  if (preview.vertices.size() > limit - model.vertices.size() ||
      preview.indices.size() > limit - model.indices.size() ||
      preview.materials.size() > limit - model.materials.size())
    throw std::runtime_error("combined OpenVDB scene exceeds renderer index limits");
  std::unordered_map<std::string, uint32_t> ordinals;
  std::vector<std::pair<uint32_t,std::string>> pointOrdinals;
  for (size_t index = 0; index < grids.size(); ++index) {
    ordinals.emplace(grids[index].first, static_cast<uint32_t>(index + 1));
    if (grids[index].second->isType<openvdb::points::PointDataGrid>())
      pointOrdinals.emplace_back(static_cast<uint32_t>(index + 1), grids[index].first);
  }
  std::unordered_map<uint32_t, uint32_t> meshIds;
  const auto firstVertex = static_cast<uint32_t>(model.vertices.size());
  const auto firstIndex = static_cast<uint32_t>(model.indices.size());
  const auto firstMaterial = static_cast<uint32_t>(model.materials.size());
  for (auto& element : preview.elements) {
    const auto ordinal = ordinals.at(element.sourceId);
    meshIds.emplace(element.meshId, ordinal);
    element.meshId = ordinal;
    element.guid = "vdb-points:" + std::to_string(ordinal) + ":" + element.sourceId;
    element.materialIndex += firstMaterial;
  }
  for (auto& range : preview.meshRanges) {
    range.meshId = meshIds.at(range.meshId);
    range.firstIndex += firstIndex;
  }
  model.vertices.reserve(model.vertices.size() + preview.vertices.size());
  model.indices.reserve(model.indices.size() + preview.indices.size());
  model.vertices.insert(model.vertices.end(), preview.vertices.begin(), preview.vertices.end());
  for (const auto index : preview.indices) model.indices.push_back(firstVertex + index);
  model.meshRanges.insert(model.meshRanges.end(), preview.meshRanges.begin(), preview.meshRanges.end());
  for (auto& material : preview.materials) model.materials.push_back(std::move(material));
  for (auto& element : preview.elements) model.elements.push_back(std::move(element));
  auto& report = model.importReport;
  report.importedProductCount += preview.importReport.importedProductCount;
  report.skippedProductCount += preview.importReport.skippedProductCount;
  report.partialProductCount += preview.importReport.partialProductCount;
  for (const auto& [category,count] : preview.importReport.representationWarnings)
    report.representationWarnings[category] += count;
  for (auto& diagnostic : preview.importReport.diagnostics) {
    const auto& [ordinal,name] = pointOrdinals.at(diagnostic.entityId - 1);
    diagnostic.entityId = diagnostic.productId = ordinal;
    diagnostic.productGuid = "vdb-points:" + std::to_string(ordinal) + ":" + name;
    report.diagnostics.push_back(std::move(diagnostic));
  }
}

} // namespace
#endif

dotbim::Model LoadFromFile(const std::filesystem::path &path, float importScale,
                           const ImportOptions &options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  const unsigned previewModes = unsigned(options.vectorPreview.has_value()) +
      unsigned(options.slicePreview.has_value()) + unsigned(options.nativePointPreview.has_value());
  if (previewModes > 1) {
    throw std::invalid_argument(
        "OpenVDB vector, slice and native point preview modes are mutually exclusive");
  }
  if (!std::isfinite(importScale) || importScale <= 0.0f) {
    throw std::invalid_argument(
        "OpenVDB importScale must be finite and positive");
  }
  if ((options.isoValue && !std::isfinite(*options.isoValue)) ||
      !std::isfinite(options.adaptivity) || options.adaptivity < 0.0 ||
      options.adaptivity > 1.0) {
    throw std::invalid_argument(
        "OpenVDB requires a finite isovalue and adaptivity in [0, 1]");
  }
  if (options.maxActiveVoxels == 0 || options.maxTriangles == 0 ||
      options.maxInputBytes == 0) {
    throw std::invalid_argument(
        "OpenVDB voxel and triangle limits must be positive");
  }
  if (options.vectorPreview) {
    auto preview=*options.vectorPreview;
    preview.maxActiveVoxels=std::min(preview.maxActiveVoxels,options.maxActiveVoxels);
    preview.maxTriangles=std::min(preview.maxTriangles,options.maxTriangles);
    preview.maxInputBytes=std::min(preview.maxInputBytes,options.maxInputBytes);
    return LoadVectorPreviewFromFile(path,options.gridName,importScale,preview);
  }
  if (options.slicePreview) {
    auto preview = *options.slicePreview;
    preview.maxActiveVoxels = std::min(preview.maxActiveVoxels, options.maxActiveVoxels);
    preview.maxTriangles = std::min(preview.maxTriangles, options.maxTriangles);
    preview.maxInputBytes = std::min(preview.maxInputBytes, options.maxInputBytes);
    return LoadSlicePreviewFromFile(path, options.gridName, importScale, preview);
  }
  if (options.nativePointPreview) {
    ProcessingOptions processing;
    processing.maxVoxelCount = options.maxActiveVoxels;
    processing.maxTriangles = options.maxTriangles;
    processing.maxInputBytes = options.maxInputBytes;
    return LoadNativePointsPreviewFromFile(path, options.gridName, importScale,
        *options.nativePointPreview, processing, options.nativePointLimits);
  }
  initializeOpenVdb();
  try {
    if (!std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > options.maxInputBytes) {
      throw std::runtime_error("input is not a regular file within maxInputBytes");
    }
    detail::RequireSeekableFile(path);
    openvdb::io::File file(path.string());
    file.open(false);
    {
      std::set<std::string> descriptors;
      long double catalogBytes = 0;
      for (auto it = file.beginName(); it != file.endName(); ++it) {
        const auto name = it.gridName();
        catalogBytes += 2.0L * name.size() + 128;
        if (descriptors.size() >= 4096 || catalogBytes > options.maxInputBytes)
          throw std::runtime_error("OpenVDB descriptor catalog exceeds import budgets");
        if (!descriptors.insert(name).second)
          throw std::runtime_error("OpenVDB grid descriptors collide; rename bracket-suffixed grids");
      }
    }
    // Use descriptor names so files containing duplicate display names retain
    // distinct grids. Inspect every selected grid before loading any voxel
    // data.
    std::vector<std::pair<std::string, openvdb::GridBase::Ptr>> grids;
    long double retainedMetadataBytes = 0;
    for (auto it = file.beginName(); it != file.endName(); ++it) {
      const std::string name = it.gridName();
      auto metadata = file.readGridMetadata(name);
      if (GridSelectionMatches(options.gridName, name, metadata->getName())) {
        if (!options.includePoints && metadata->isType<openvdb::points::PointDataGrid>()) {
          if (!options.gridName.empty())
            throw std::runtime_error("PointDataGrid '" + name + "' is excluded from scalar-only surface import");
          continue;
        }
        retainedMetadataBytes += metadataBytes(*metadata);
        if (retainedMetadataBytes + std::filesystem::file_size(path) > options.maxInputBytes)
          throw std::runtime_error("OpenVDB selected metadata exceeds import memory budget");
        grids.emplace_back(name, std::move(metadata));
      }
    }
    if (grids.empty()) {
      throw std::runtime_error(options.gridName.empty()
                                   ? "OpenVDB file contains no grids eligible for the selected import mode"
                                   : "OpenVDB grid '" + options.gridName +
                                         "' was not found");
    }
    if (!options.gridName.empty() && grids.size() > 1) {
      throw std::runtime_error(
          "OpenVDB grid name '" + options.gridName +
          "' is ambiguous; select descriptor:<unique descriptor name>");
    }
    dotbim::Model model{};
    model.importReport.sourceFormat = "OpenVDB";
    model.importReport.sourceItemName = "grids";
    model.importReport.sourceProductCount = grids.size();
    model.unitMetadata.hasImportScale = true;
    model.unitMetadata.importScale = importScale;
    model.unitMetadata.hasEffectiveImportScale = true;
    model.unitMetadata.effectiveImportScale = importScale;
    dotbim::Material material{};
    material.pbr.baseColor = {0.72f, 0.8f, 0.87f, 1.0f};
    material.pbr.roughnessFactor = 0.75f;
    material.pbr.attenuationDistance = std::numeric_limits<float>::max();
    model.materials.push_back(std::move(material));

    uint32_t ordinal = 0;
    bool hasPoints = false;
    for (const auto &[name, metadata] : grids) {
      ++ordinal;
      if (metadata->isType<openvdb::points::PointDataGrid>() && options.includePoints) {
        hasPoints = true;
        continue;
      }
      if (!metadata->isType<openvdb::FloatGrid>() &&
          !metadata->isType<openvdb::DoubleGrid>()) {
        warnSkipped(
            model, ordinal, name, "OpenVDB.UnsupportedGridType",
            "unsupported value type '" + metadata->valueType() +
                "'; this extraction accepts Float/Double surfaces" +
                std::string(options.includePoints ? " and standard PointDataGrids" : ""));
        continue;
      }
      const auto count = metadata->getMetadata<openvdb::Int64Metadata>(
          openvdb::GridBase::META_FILE_VOXEL_COUNT);
      if (count && (count->value() < 0 ||
          static_cast<uint64_t>(count->value()) > options.maxActiveVoxels)) {
        gridError(name, "file metadata exceeds maxActiveVoxels (" +
                            std::to_string(options.maxActiveVoxels) + ")");
      }
      const auto bytes = metadata->getMetadata<openvdb::Int64Metadata>(
          openvdb::GridBase::META_FILE_MEM_BYTES);
      if (bytes && (bytes->value() < 0 ||
                    static_cast<uint64_t>(bytes->value()) > options.maxInputBytes)) {
        gridError(name, "file metadata exceeds maxInputBytes (" +
                            std::to_string(options.maxInputBytes) + ")");
      }
      const auto grid = file.readGrid(name);
      if (grid->isType<openvdb::FloatGrid>()) {
        appendSurface(model, *openvdb::gridPtrCast<openvdb::FloatGrid>(grid),
                      name, ordinal, importScale, options);
      } else {
        appendSurface(model, *openvdb::gridPtrCast<openvdb::DoubleGrid>(grid),
                      name, ordinal, importScale, options);
      }
    }
    file.close();
    if (hasPoints) {
      auto previewOptions = options.pointPreview;
      previewOptions.maxActiveVoxels = std::min(previewOptions.maxActiveVoxels, options.maxActiveVoxels);
      const size_t trianglesLeft = options.maxTriangles - model.indices.size() / 3;
      // Markers require eight triangles. A cap of one allows empty grids while
      // rejecting any nonempty point output when no triangle budget remains.
      previewOptions.maxTriangles = std::min(previewOptions.maxTriangles, std::max<size_t>(1, trianglesLeft));
      const long double retained = retainedMetadataBytes + 3.0L * (
          model.vertices.capacity() * static_cast<long double>(sizeof(Vertex)) +
          model.indices.capacity() * static_cast<long double>(sizeof(uint32_t)));
      if (retained >= options.maxInputBytes)
        throw std::runtime_error("scalar geometry and point preview exceed combined import memory budget");
      previewOptions.maxInputBytes = std::min(previewOptions.maxInputBytes,
          static_cast<uintmax_t>(static_cast<long double>(options.maxInputBytes) - retained));
      auto preview = LoadPointsPreviewFromFile(path, options.gridName, importScale, previewOptions);
      mergePointPreview(model, std::move(preview), grids);
    }
    if (model.elements.empty()) {
      std::string reason =
          options.includePoints ? "OpenVDB file contains no displayable scalar surfaces or point markers"
                                : "OpenVDB file contains no extractable scalar surface";
      if (!model.importReport.diagnostics.empty()) {
        reason += "; " + model.importReport.diagnostics.front().reason;
      }
      throw std::runtime_error(reason);
    }
    model.importReport.completeness = model.importReport.diagnostics.empty()
                                          ? ImportCompleteness::Complete
                                          : ImportCompleteness::Partial;
    return model;
  } catch (const std::exception &error) {
    throw std::runtime_error("Cannot import OpenVDB file '" + path.string() +
                             "': " + error.what());
  }
#else
  (void)path;
  (void)importScale;
  (void)options;
  throw std::runtime_error(
      "OpenVDB support is unavailable in this build. Install OpenVDB and "
      "configure with -DENABLE_OPENVDB=ON to import .vdb surface meshes.");
#endif
}

} // namespace container::geometry::vdb
