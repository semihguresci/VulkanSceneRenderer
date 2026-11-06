#include "Container/geometry/OpenVdbProcessing.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"
#include "Container/geometry/OpenVdbMeshConversion.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include "Container/geometry/DotBimLoader.h"
#include "Container/geometry/GltfModelLoader.h"
#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/geometry/UsdLoader.h"

#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/GridTransformer.h>
#include <openvdb/tools/LevelSetFilter.h>
#include <openvdb/tools/LevelSetRebuild.h>
#include <openvdb/tools/LevelSetSphere.h>
#include <openvdb/tools/LevelSetUtil.h>
#include <openvdb/tools/MeshToVolume.h>

#include <cctype>
#include <functional>
#include <mutex>
#include <set>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <unordered_map>
#include <vector>
#endif

namespace container::geometry::vdb {
namespace {

[[noreturn]] void unavailable() {
  throw std::runtime_error(
      "OpenVDB processing is unavailable in this build. Install the OpenVDB "
      "dependencies and configure with -DENABLE_OPENVDB=ON.");
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
void initialize() {
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
}

void validateOptions(const ProcessingOptions& options) {
  if (!std::isfinite(options.voxelSize) || options.voxelSize <= 0.0 ||
      options.voxelSize > std::numeric_limits<float>::max() ||
      static_cast<float>(options.voxelSize) == 0.0f)
    throw std::invalid_argument("voxel size must be finite and positive");
  if (!std::isfinite(options.halfWidth) || options.halfWidth <= 1.0 ||
      options.halfWidth > 64.0)
    throw std::invalid_argument("half width must be in (1, 64] voxels");
  if (options.voxelSize * options.halfWidth > std::numeric_limits<float>::max())
    throw std::invalid_argument("voxel size and half width exceed the FloatGrid distance range");
  if (options.maxVoxelCount == 0 || options.maxTriangles == 0 ||
      options.maxInputBytes == 0)
    throw std::invalid_argument("processing budgets must be positive");
  if (options.gridName.empty())
    throw std::invalid_argument("output grid name must not be empty");
}

void validateOutput(const std::filesystem::path& output) {
  if (output.empty())
    throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(output))
    throw std::invalid_argument("output already exists; choose a new .vdb path");
  if (!output.parent_path().empty() &&
      !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("output directory does not exist");
}

void validateInput(const std::filesystem::path& input,
                   const ProcessingOptions& options) {
  if (!std::filesystem::is_regular_file(input))
    throw std::invalid_argument("input is not a regular file");
  if (std::filesystem::file_size(input) > options.maxInputBytes)
    throw std::invalid_argument("input exceeds the file-size budget");
}

void validateBounds(const std::array<double, 3>& minimum,
                    const std::array<double, 3>& maximum, double voxelSize,
                    double padding, const ProcessingOptions& options) {
  long double count = 1.0;
  for (size_t axis = 0; axis < 3; ++axis) {
    const double low = minimum[axis] / voxelSize - padding;
    const double high = maximum[axis] / voxelSize + padding;
    if (!std::isfinite(low) || !std::isfinite(high) || high < low ||
        low < static_cast<double>(std::numeric_limits<int32_t>::min()) + 1.0 ||
        high > static_cast<double>(std::numeric_limits<int32_t>::max()) - 1.0)
      throw std::invalid_argument("geometry exceeds OpenVDB coordinate limits");
    count *= std::ceil(high) - std::floor(low) + 1.0;
    if (count > static_cast<long double>(options.maxVoxelCount))
      throw std::invalid_argument(
          "geometry exceeds the voxel budget; increase voxel size or "
          "--max-voxels");
  }
}

void validateGrid(const openvdb::FloatGrid& grid,
                   const ProcessingOptions& options) {
  if (grid.activeVoxelCount() > options.maxVoxelCount)
    throw std::invalid_argument("input grid exceeds the voxel budget");
  constexpr uint64_t leafVoxels = openvdb::FloatTree::LeafNodeType::NUM_VALUES;
  if (uint64_t(grid.tree().leafCount()) >
          options.maxVoxelCount / leafVoxels +
              (options.maxVoxelCount % leafVoxels != 0) ||
      uint64_t(grid.memUsage()) > options.maxInputBytes)
    throw std::invalid_argument("stored grid topology exceeds the voxel or input-memory budget");
  if (!grid.transform().isLinear())
    throw std::invalid_argument("processing requires an affine grid transform");
  const std::array<openvdb::Vec3d, 4> worldPoints{
      grid.indexToWorld(openvdb::Vec3d(0, 0, 0)),
      grid.indexToWorld(openvdb::Vec3d(1, 0, 0)),
      grid.indexToWorld(openvdb::Vec3d(0, 1, 0)),
      grid.indexToWorld(openvdb::Vec3d(0, 0, 1))};
  for (const auto& point : worldPoints)
    for (size_t axis = 0; axis < 3; ++axis)
      if (!std::isfinite(point[axis]))
        throw std::invalid_argument("input grid has a non-finite affine transform");
  const auto x = worldPoints[1] - worldPoints[0];
  const auto y = worldPoints[2] - worldPoints[0];
  const auto z = worldPoints[3] - worldPoints[0];
  const double determinant = x.dot(y.cross(z));
  if (!std::isfinite(determinant) || determinant == 0.0)
    throw std::invalid_argument("input grid has a singular affine transform");
  for (size_t axis = 0; axis < 3; ++axis)
    if (!std::isfinite(grid.voxelSize()[axis]) || grid.voxelSize()[axis] <= 0.0)
      throw std::invalid_argument("input grid has an invalid voxel transform");
  auto bounds = grid.evalActiveVoxelBoundingBox();
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    if (!std::isfinite(*value))
      throw std::invalid_argument("input grid has a non-finite scalar value");
    // Inactive negative tiles are filled by sdfToFogVolume and may become
    // active density. Include them before that operation, not only afterward.
    if (*value != grid.background()) bounds.expand(value.getBoundingBox());
  }
  if (bounds.empty()) return;
  long double count = 1.0;
  for (size_t axis = 0; axis < 3; ++axis) {
    count *= static_cast<long double>(bounds.max()[axis]) -
             static_cast<long double>(bounds.min()[axis]) + 1.0L;
    if (count > static_cast<long double>(options.maxVoxelCount))
      throw std::invalid_argument("grid bounds exceed the voxel budget");
  }
}

openvdb::FloatGrid::Ptr readGrid(const std::filesystem::path& input,
                                const std::string& name, bool levelSet,
                                const ProcessingOptions& options) {
  validateInput(input, options);
  initialize();
  openvdb::io::File file(input.string());
  file.open(false);
  std::string selected;
  bool found = false;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const std::string descriptorName = it.gridName();
    const auto grid = file.readGridMetadata(descriptorName);
    if (!GridSelectionMatches(name, descriptorName, grid->getName()) ||
        !grid->isType<openvdb::FloatGrid>() ||
        (levelSet && grid->getGridClass() != openvdb::GRID_LEVEL_SET))
      continue;
    if (found)
      throw std::invalid_argument("input grid name is ambiguous; use descriptor:<unique descriptor name>");
    if (const auto count = grid->getMetadata<openvdb::Int64Metadata>(
            openvdb::GridBase::META_FILE_VOXEL_COUNT);
        count && (count->value() < 0 ||
                  static_cast<uint64_t>(count->value()) > options.maxVoxelCount))
      throw std::invalid_argument("input grid metadata exceeds the voxel budget");
    if (const auto bytes = grid->getMetadata<openvdb::Int64Metadata>(
            openvdb::GridBase::META_FILE_MEM_BYTES);
        bytes && (bytes->value() < 0 ||
                  static_cast<uint64_t>(bytes->value()) > options.maxInputBytes))
      throw std::invalid_argument("input grid metadata exceeds the input-memory budget");
    selected = descriptorName;
    found = true;
    if (name.empty()) break;
  }
  if (!found)
    throw std::invalid_argument(levelSet
        ? "no matching FloatGrid level set found in input"
        : "no matching FloatGrid found in input");
  auto result = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(selected));
  file.close();
  validateGrid(*result, options);
  if (!std::isfinite(result->background()) ||
      (levelSet && result->background() <= 0.0f))
    throw std::invalid_argument("level set has an invalid background value");
  return result;
}

// Sampling accepts either scalar precision without broadening the FloatGrid
// contract of SDF editing/resampling. Inspect a bounded, collision-free catalog
// before loading the selected scalar tree.
openvdb::GridBase::Ptr readSampleGrid(const std::filesystem::path& input,
                                     const std::string& selection,
                                     const ProcessingOptions& options) {
  validateInput(input, options);
  initialize();
  detail::RequireSeekableFile(input);
  const auto encoded = std::filesystem::file_size(input);
  openvdb::io::File file(input.string());
  file.open(false);
  {
    std::set<std::string> descriptors;
    long double catalogBytes = encoded;
    for (auto name = file.beginName(); name != file.endName(); ++name) {
      const auto descriptor = name.gridName();
      catalogBytes += 2.0L * descriptor.size() + 128;
      if (catalogBytes > options.maxInputBytes)
        throw std::invalid_argument("scalar sample descriptor catalog exceeds the input-memory budget");
      if (!descriptors.insert(descriptor).second)
        throw std::invalid_argument("scalar sample grid descriptors collide; rename bracket-suffixed grids");
    }
  }
  std::string selected;
  bool found = false;
  for (auto name = file.beginName(); name != file.endName(); ++name) {
    const auto descriptor = name.gridName();
    const auto metadata = file.readGridMetadata(descriptor);
    if (!GridSelectionMatches(selection, descriptor, metadata->getName())) continue;
    if (!metadata->isType<openvdb::FloatGrid>() && !metadata->isType<openvdb::DoubleGrid>()) {
      if (!selection.empty()) throw std::invalid_argument("selected sample grid must be FloatGrid or DoubleGrid");
      continue;
    }
    if (found) throw std::invalid_argument("input grid name is ambiguous; use descriptor:<unique descriptor name>");
    for (const char* key : {openvdb::GridBase::META_FILE_VOXEL_COUNT, openvdb::GridBase::META_FILE_MEM_BYTES}) {
      const auto value = metadata->getMetadata<openvdb::Int64Metadata>(key);
      const long double limit = key == openvdb::GridBase::META_FILE_VOXEL_COUNT
                                    ? options.maxVoxelCount : options.maxInputBytes;
      if (value && (value->value() < 0 || static_cast<long double>(value->value()) > limit))
        throw std::invalid_argument("scalar sample recorded topology exceeds processing budgets");
    }
    selected = descriptor;
    found = true;
    if (selection.empty()) break;
  }
  if (!found) throw std::invalid_argument("no matching FloatGrid or DoubleGrid scalar grid found in input");
  auto grid = file.readGrid(selected);
  file.close();
  return grid;
}

template<class Grid>
double sampleScalarGrid(const Grid& grid, const std::array<double, 3>& position,
                        const ProcessingOptions& options, uintmax_t encoded) {
  constexpr uint64_t leafValues = Grid::TreeType::LeafNodeType::NUM_VALUES;
  const auto maxLeaves = options.maxVoxelCount / leafValues + (options.maxVoxelCount % leafValues != 0);
  long double metadataBytes = sizeof(openvdb::MetaMap);
  for (auto field = grid.beginMeta(); field != grid.endMeta(); ++field)
    metadataBytes += 2.0L * (field->first.size() + field->second->typeName().size() +
                            static_cast<long double>(field->second->size())) + 256;
  if (grid.activeVoxelCount() > options.maxVoxelCount || grid.tree().leafCount() > maxLeaves ||
      static_cast<long double>(encoded) + grid.memUsage() + metadataBytes > options.maxInputBytes)
    throw std::invalid_argument("scalar sample stored topology or memory exceeds processing budgets");
  if (!std::isfinite(double(grid.background())))
    throw std::invalid_argument("input grid has a non-finite scalar background");
  for (auto value = grid.cbeginValueAll(); value; ++value)
    if (!std::isfinite(double(*value))) throw std::invalid_argument("input grid has a non-finite scalar value");
  if (!grid.transform().isLinear()) throw std::invalid_argument("scalar sampling requires an affine grid transform");
  const auto map = grid.transform().baseMap();
  const auto origin = map->applyMap(openvdb::Vec3d(0));
  std::array<openvdb::Vec3d, 3> axes;
  for (size_t axis = 0; axis < 3; ++axis) {
    openvdb::Vec3d unit(0); unit[axis] = 1;
    axes[axis] = map->applyJacobian(unit);
    const double length = std::hypot(axes[axis].x(), axes[axis].y(), axes[axis].z());
    if (!std::isfinite(origin[axis]) || !std::isfinite(length) || length <= 0)
      throw std::invalid_argument("scalar sample transform must be finite and nonsingular");
    axes[axis] /= length;
  }
  const double determinant = axes[0].dot(axes[1].cross(axes[2]));
  if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
    throw std::invalid_argument("scalar sample transform is singular or ill-conditioned");
  const auto coordinate = map->applyInverseJacobian(openvdb::Vec3d(position[0],position[1],position[2]) - origin);
  for (size_t axis = 0; axis < 3; ++axis)
    if (!std::isfinite(coordinate[axis]) ||
        coordinate[axis] < double(std::numeric_limits<int32_t>::min()) + 1 ||
        coordinate[axis] > double(std::numeric_limits<int32_t>::max()) - 1)
      throw std::invalid_argument("sample exceeds OpenVDB coordinate limits");
  const double result = double(grid.tree().getValue(openvdb::Coord::round(coordinate)));
  if (!std::isfinite(result)) throw std::invalid_argument("sampled input grid value is not finite");
  return result;
}

ProcessingResult writeGrid(openvdb::FloatGrid::Ptr grid,
                           const std::filesystem::path& output,
                           const ProcessingOptions& options) {
  validateGrid(*grid, options);
  detail::RemoveBoundaryProjectionMetadata(*grid);
  grid->setName(options.gridName);
  ProcessingResult result{
      grid->getName(), grid->getGridClass() == openvdb::GRID_LEVEL_SET
                           ? "level set" : "fog volume",
      static_cast<uint64_t>(grid->activeVoxelCount()), grid->voxelSize().x()};
  try {
    openvdb::io::File file(output.string());
    file.write(openvdb::GridPtrVec{grid});
    file.close();
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    throw;
  }
  return result;
}

bool finitePoint(const std::array<double, 3>& point) {
  return std::all_of(point.begin(), point.end(),
                     [](double x) { return std::isfinite(x); });
}

void validateMeshImport(const dotbim::Model& model) {
  const auto& report = model.importReport;
  if (report.completeness == ImportCompleteness::Partial ||
      report.completeness == ImportCompleteness::Failed ||
      report.skippedProductCount != 0 || report.partialProductCount != 0) {
    std::string reason = "cannot voxelize an incomplete import: " + report.summary();
    if (!report.diagnostics.empty()) reason += "; " + report.diagnostics.front().reason;
    throw std::invalid_argument(reason);
  }
  for (const auto& element : model.elements)
    if (element.geometryKind != dotbim::GeometryKind::Mesh)
      throw std::invalid_argument(
          "mesh voxelization requires triangle surfaces only; points or curves would be omitted");
}

glm::dmat4 gltfLocalTransform(const tinygltf::Node& node) {
  if ((!node.matrix.empty() && node.matrix.size() != 16) ||
      (!node.translation.empty() && node.translation.size() != 3) ||
      (!node.rotation.empty() && node.rotation.size() != 4) ||
      (!node.scale.empty() && node.scale.size() != 3))
    throw std::invalid_argument("glTF node has an invalid matrix or TRS component count");
  if (!node.matrix.empty()) {
    if (!node.translation.empty() || !node.rotation.empty() || !node.scale.empty())
      throw std::invalid_argument("glTF node cannot specify both a matrix and TRS");
    glm::dmat4 matrix(1.0);
    for (size_t column = 0; column < 4; ++column)
      for (size_t row = 0; row < 4; ++row)
        matrix[column][row] = node.matrix[column * 4 + row];
    return matrix;
  }
  glm::dmat4 matrix(1.0);
  if (!node.translation.empty())
    matrix = glm::translate(matrix, glm::dvec3(node.translation[0],
                                             node.translation[1], node.translation[2]));
  if (!node.rotation.empty()) {
    const glm::dquat rotation(node.rotation[3], node.rotation[0],
                             node.rotation[1], node.rotation[2]);
    if (!std::isfinite(glm::length(rotation)) ||
        std::abs(glm::length(rotation) - 1.0) > 1.0e-5)
      throw std::invalid_argument("glTF node rotation must be a finite unit quaternion");
    matrix *= glm::mat4_cast(rotation);
  }
  if (!node.scale.empty())
    matrix = glm::scale(matrix, glm::dvec3(node.scale[0], node.scale[1], node.scale[2]));
  return matrix;
}

void validateAffine(const glm::dmat4& transform) {
  for (size_t column = 0; column < 4; ++column)
    for (size_t row = 0; row < 4; ++row)
      if (!std::isfinite(transform[column][row]) ||
          std::abs(transform[column][row]) > std::numeric_limits<float>::max())
        throw std::invalid_argument("mesh instance transform exceeds finite float coordinates");
  const double determinant = glm::determinant(glm::dmat3(transform));
  if (transform[0][3] != 0.0 || transform[1][3] != 0.0 ||
      transform[2][3] != 0.0 || transform[3][3] != 1.0 ||
      !std::isfinite(determinant) || determinant == 0.0)
    throw std::invalid_argument("mesh instance transform must be nonsingular and affine");
}

dotbim::Model gltfWorldInstances(const std::filesystem::path& input,
                                float importScale,
                                const ProcessingOptions& options,
                                bool requireTransformPrecision = false) {
  // The shared loader owns attribute/index validation and primitive ordering.
  // Its flattened primitive buffers still need the active scene's instances.
  const auto pathUtf8 = input.u8string();
  auto loaded = [&] {
    try {
      return gltf::LoadModelWithSource(
          std::string(pathUtf8.begin(), pathUtf8.end()),
          {.decodeImages = false, .maxInputBytes = options.maxInputBytes});
    } catch (const std::runtime_error& error) {
      // The importer now rejects hierarchy defects before our active-scene
      // walk. Preserve that walk's invalid-input category for this diagnostic
      // family, while leaving file/buffer/decoder runtime failures unchanged.
      const auto hierarchyPrefix=std::string("glTF asset '")+
          std::string(pathUtf8.begin(),pathUtf8.end())+"': glTF scene hierarchy:";
      if (std::string_view(error.what()).starts_with(hierarchyPrefix))
        throw std::invalid_argument(error.what());
      throw;
    }
  }();
  const auto& source = loaded.gltfModel;
  const auto& primitives = loaded.model.primitiveRanges();
  const auto& vertices = loaded.model.vertices();
  const auto& indices = loaded.model.indices();
  uintmax_t payloadBytes = std::filesystem::file_size(input);
  auto addPayload = [&](uintmax_t bytes) {
    if (bytes > options.maxInputBytes - payloadBytes)
      throw std::invalid_argument("glTF payloads exceed the input-memory budget");
    payloadBytes += bytes;
  };
  for (const auto& buffer : source.buffers) addPayload(buffer.data.size());
  for (const auto& image : source.images) addPayload(image.image.size());
  addPayload(uintmax_t(vertices.size()) * sizeof(Vertex));
  addPayload(uintmax_t(indices.size()) * sizeof(uint32_t));
  dotbim::Model result;
  result.vertices = vertices;
  result.indices = indices;
  result.importReport.sourceFormat = "glTF";
  result.importReport.sourceItemName = "triangle instances";
  std::vector<size_t> primitiveBase(source.meshes.size());
  size_t base = 0;
  for (size_t mesh = 0; mesh < source.meshes.size(); ++mesh) {
    primitiveBase[mesh] = base;
    base += source.meshes[mesh].primitives.size();
  }
  if (base != primitives.size() || base > std::numeric_limits<uint32_t>::max())
    throw std::invalid_argument("glTF primitive layout exceeds the mesh ID range");
  for (size_t primitive = 0; primitive < primitives.size(); ++primitive)
    result.meshRanges.push_back({.meshId = static_cast<uint32_t>(primitive),
                                 .firstIndex = primitives[primitive].firstIndex,
                                 .indexCount = primitives[primitive].indexCount});
  std::vector<int> roots;
  if (!source.scenes.empty()) {
    const int scene = source.defaultScene < 0 ? 0 : source.defaultScene;
    if (static_cast<size_t>(scene) >= source.scenes.size())
      throw std::invalid_argument("glTF default scene index is invalid");
    roots = source.scenes[scene].nodes;
  } else {
    std::vector<bool> hasParent(source.nodes.size());
    for (const auto& node : source.nodes)
      for (int child : node.children) {
        if (child < 0 || static_cast<size_t>(child) >= source.nodes.size())
          throw std::invalid_argument("glTF child node index is invalid");
        hasParent[child] = true;
      }
    for (size_t node = 0; node < source.nodes.size(); ++node)
      if (!hasParent[node]) roots.push_back(static_cast<int>(node));
  }
  struct Visit { int node; glm::dmat4 parent; };
  std::vector<Visit> pending;
  const auto scale = glm::scale(glm::dmat4(1.0), glm::dvec3(importScale));
  for (int root : roots) pending.push_back({root, scale});
  std::vector<bool> visited(source.nodes.size());
  size_t instanceTriangles = 0;
  while (!pending.empty()) {
    const auto visit = pending.back();
    pending.pop_back();
    if (visit.node < 0 || static_cast<size_t>(visit.node) >= source.nodes.size())
      throw std::invalid_argument("glTF scene node index is invalid");
    if (visited[visit.node])
      throw std::invalid_argument("glTF active scene contains a cycle or repeated node");
    visited[visit.node] = true;
    const auto& node = source.nodes[visit.node];
    if (node.skin >= 0 || !node.weights.empty() ||
        node.extensions.count("EXT_mesh_gpu_instancing") != 0)
      throw std::invalid_argument("glTF skin, morph and GPU-instance deformation must be baked before voxelization");
    const auto world = visit.parent * gltfLocalTransform(node);
    validateAffine(world);
    if (node.mesh >= 0) {
      if (static_cast<size_t>(node.mesh) >= source.meshes.size())
        throw std::invalid_argument("glTF node mesh index is invalid");
      const auto& mesh = source.meshes[node.mesh];
      if (!mesh.weights.empty())
        throw std::invalid_argument("glTF morph deformation must be baked before voxelization");
      for (size_t localPrimitive = 0; localPrimitive < mesh.primitives.size(); ++localPrimitive) {
        if (!mesh.primitives[localPrimitive].targets.empty())
          throw std::invalid_argument("glTF morph targets must be baked before voxelization");
        const size_t primitive = primitiveBase[node.mesh] + localPrimitive;
        const size_t triangleCount = primitives[primitive].indexCount / 3;
        if (triangleCount > options.maxTriangles - instanceTriangles)
          throw std::invalid_argument("glTF world instances exceed the triangle budget");
        instanceTriangles += triangleCount;
        dotbim::Element element;
        element.meshId = static_cast<uint32_t>(primitive);
        element.transform = glm::mat4(world);
        if (requireTransformPrecision) {
          const auto& range=primitives[primitive];
          glm::dmat4 represented(1.0);
          // Force the storage conversion: MSVC may otherwise retain excess
          // precision through an immediately promoted Float matrix temporary.
          for (int column=0;column<4;++column)
            for (int row=0;row<4;++row) {
              volatile float stored=element.transform[column][row];
              represented[column][row]=stored;
            }
          for (uint64_t index=range.firstIndex;index<uint64_t(range.firstIndex)+range.indexCount;++index) {
            if (index>=indices.size() || indices[index]>=vertices.size())
              throw std::invalid_argument("glTF instance references an invalid vertex");
            const glm::dvec4 local(vertices[indices[index]].position,1);
            const auto exact=world*local, narrowed=represented*local;
            for (int axis=0;axis<3;++axis)
              if (!std::isfinite(narrowed[axis]) || std::abs(exact[axis]-narrowed[axis])>options.voxelSize*1e-4)
                throw std::invalid_argument("glTF instance transform loses more than 1e-4 voxel in the scene Float transform; recenter/bake the source or increase voxel size");
          }
        }
        element.guid = "gltf:" + std::to_string(visit.node) + ":" + std::to_string(localPrimitive);
        result.elements.push_back(std::move(element));
      }
    }
    for (int child : node.children) pending.push_back({child, world});
  }
  if (result.elements.empty())
    throw std::invalid_argument("glTF active scene contains no triangle surfaces");
  result.importReport.completeness = ImportCompleteness::Complete;
  result.importReport.sourceProductCount = result.elements.size();
  result.importReport.importedProductCount = result.elements.size();
  return result;
}

void validateResampleBounds(const openvdb::FloatGrid& grid,
                            const ProcessingOptions& options) {
  auto bounds = grid.evalActiveVoxelBoundingBox();
  for (auto value = grid.cbeginValueAll(); value; ++value)
    if (*value != grid.background()) bounds.expand(value.getBoundingBox());
  if (bounds.empty()) throw std::invalid_argument("cannot resample an empty grid");
  std::array<double, 3> minimum, maximum;
  minimum.fill(std::numeric_limits<double>::infinity());
  maximum.fill(-std::numeric_limits<double>::infinity());
  // Include interpolation support before the target SDF band is added.
  for (int corner = 0; corner < 8; ++corner) {
    openvdb::Vec3d index;
    for (size_t axis = 0; axis < 3; ++axis)
      index[axis] = (corner & (1 << axis)) ? double(bounds.max()[axis]) + 1.0
                                         : double(bounds.min()[axis]) - 1.0;
    const auto world = grid.indexToWorld(index);
    for (size_t axis = 0; axis < 3; ++axis) {
      minimum[axis] = std::min(minimum[axis], world[axis]);
      maximum[axis] = std::max(maximum[axis], world[axis]);
    }
  }
  validateBounds(minimum, maximum, options.voxelSize,
                 grid.getGridClass() == openvdb::GRID_LEVEL_SET
                     ? options.halfWidth + 2.0 : 2.0,
                 options);
}

double validateEditableLevelSet(const openvdb::FloatGrid& grid) {
  const auto origin = grid.indexToWorld(openvdb::Vec3d(0));
  std::array<openvdb::Vec3d, 3> basis{
      grid.indexToWorld(openvdb::Vec3d(1,0,0)) - origin,
      grid.indexToWorld(openvdb::Vec3d(0,1,0)) - origin,
      grid.indexToWorld(openvdb::Vec3d(0,0,1)) - origin};
  std::array<double, 3> lengths{};
  for (size_t axis = 0; axis < 3; ++axis) {
    lengths[axis] = std::hypot(basis[axis].x(), basis[axis].y(), basis[axis].z());
    if (!std::isfinite(lengths[axis]) || lengths[axis] <= 0.0)
      throw std::invalid_argument("level-set editing requires a finite nonsingular transform");
    basis[axis] /= lengths[axis];
  }
  // Some upstream map types use an absolute uniformity tolerance. Normalize
  // first so tiny nonuniform scales/shears cannot slip through that test.
  constexpr double relativeTolerance = 1e-6;
  if (!grid.hasUniformVoxels() ||
      std::abs(lengths[1] / lengths[0] - 1.0) > relativeTolerance ||
      std::abs(lengths[2] / lengths[0] - 1.0) > relativeTolerance ||
      std::abs(basis[0].dot(basis[1])) > relativeTolerance ||
      std::abs(basis[0].dot(basis[2])) > relativeTolerance ||
      std::abs(basis[1].dot(basis[2])) > relativeTolerance)
    throw std::invalid_argument("level-set editing requires uniform voxels; resample the input first");
  const double voxelSize = grid.voxelSize().x();
  // Float32 tracking uses reciprocal/squared gradients, WENO polynomials and
  // a fixed 1e-8 normalization tolerance. Restrict the supported scale before
  // constructing the tracker, rather than overflowing/regularizing away an
  // otherwise finite source field during its first normalization step.
  if (voxelSize < 1e-6 || voxelSize > 1e6)
    throw std::invalid_argument("Float32 level-set tracking requires a source voxel size in [1e-6,1e6] world units; rescale the input first");
  const double halfWidth = double(grid.background()) / voxelSize;
  if (!std::isfinite(halfWidth) || halfWidth < 3.0 - 1e-6 || halfWidth > 64.0 + 1e-6)
    throw std::invalid_argument("level-set editing requires a band half width in [3,64] voxels; resample the input first");
  if (grid.empty()) throw std::invalid_argument("cannot edit an empty level set");
  const double maximumDistance = double(grid.background()) * (1.0 + 1e-6);
  for (auto value = grid.cbeginValueAll(); value; ++value)
    if (std::abs(double(*value)) > maximumDistance)
      throw std::invalid_argument("level-set editing requires stored distances within the input band; rebuild the level set first");
  return voxelSize;
}

// LevelSetTracker::track() dilates by one face-neighbour voxel before
// normalization/pruning. Bound each small operation independently so erosion
// does not reserve the entire travelled distance around a shrinking object.
void validateFilterWorkspace(const openvdb::FloatGrid& grid, uint32_t trackingPasses,
                             uint32_t stencilRadius, const ProcessingOptions& options) {
  const auto bounds = grid.evalActiveVoxelBoundingBox();
  if (bounds.empty()) return; // Erosion can remove the entire surface.
  std::array<int64_t, 3> minimum{}, maximum{};
  long double voxels = 1;
  for (size_t axis = 0; axis < 3; ++axis) {
    minimum[axis] = int64_t(bounds.min()[axis]) - trackingPasses;
    maximum[axis] = int64_t(bounds.max()[axis]) + trackingPasses;
    if (minimum[axis] - stencilRadius < std::numeric_limits<int32_t>::min() ||
        maximum[axis] + stencilRadius > std::numeric_limits<int32_t>::max())
      throw std::invalid_argument("level-set filter exceeds OpenVDB coordinate limits");
    voxels *= static_cast<long double>(maximum[axis] - minimum[axis] + 1);
  }
  if (voxels > options.maxVoxelCount)
    throw std::invalid_argument("level-set filter expansion exceeds the voxel budget");
  auto nodeCount = [&](int64_t dimension) {
    long double count = 1;
    for (size_t axis = 0; axis < 3; ++axis)
      count *= std::floor(static_cast<long double>(maximum[axis]) / dimension) -
               std::floor(static_cast<long double>(minimum[axis]) / dimension) + 1;
    return count;
  };
  using Leaf = openvdb::FloatTree::LeafNodeType;
  using Upper = openvdb::FloatTree::RootNodeType::ChildNodeType;
  using Lower = Upper::ChildNodeType;
  const auto leaves = nodeCount(Leaf::DIM);
  uint64_t outsideLeaves = 0;
  for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
    const auto stored = leaf->getNodeBoundingBox();
    bool outside = false;
    for (size_t axis = 0; axis < 3; ++axis)
      outside |= int64_t(stored.max()[axis]) < minimum[axis] ||
                 int64_t(stored.min()[axis]) > maximum[axis];
    outsideLeaves += outside;
  }
  // Remote inactive/background leaves remain in LeafManager until pruning.
  // They need auxiliary buffers alongside all prospective active-region leaves.
  const long double peakLeaves = leaves + outsideLeaves;
  const auto maxLeaves = options.maxVoxelCount / Leaf::NUM_VALUES +
                         (options.maxVoxelCount % Leaf::NUM_VALUES != 0);
  if (peakLeaves > maxLeaves)
    throw std::invalid_argument("level-set filter stored topology exceeds the voxel budget");
  // Include the existing tree, prospective nodes, leaf-manager pointers,
  // normalization/box buffers and conservative temporary topology overhead.
  const long double workspace = static_cast<long double>(grid.memUsage()) +
      leaves * sizeof(Leaf) +
      peakLeaves * (4ull * Leaf::NUM_VALUES * sizeof(float) + 128ull) +
      2 * nodeCount(Lower::DIM) * sizeof(Lower) +
      2 * nodeCount(Upper::DIM) * sizeof(Upper);
  if (workspace > options.maxInputBytes)
    throw std::invalid_argument("level-set filter workspace exceeds the input-memory budget");
}
dotbim::Model loadModelInput(const std::filesystem::path& input, float importScale,
                            const ProcessingOptions& options, bool requireTransformPrecision = false) {
  std::string extension = input.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  dotbim::Model model;
  if (extension == ".bim") model = dotbim::LoadFromFile(input, importScale);
  else if (extension == ".ifc") model = ifc::LoadFromFile(input, importScale);
  else if (extension == ".ifcx") model = ifcx::LoadFromFile(input, importScale);
  else if (extension == ".usd" || extension == ".usda" ||
           extension == ".usdc" || extension == ".usdz")
    model = usd::LoadFromFile(input, importScale);
  else if (extension == ".gltf" || extension == ".glb")
    model = gltfWorldInstances(input, importScale, options, requireTransformPrecision);
  else throw std::invalid_argument("mesh input must be .bim, .ifc, .ifcx, .usd, .usda, .usdc, .usdz, .gltf or .glb");
  return model;
}

#endif
}  // namespace

ProcessingResult WriteSphere(const std::filesystem::path& output,
                              const SphereOptions& sphere,
                              const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options);
  validateOutput(output);
  if (!std::isfinite(sphere.radius) || sphere.radius <= 0.0 ||
      sphere.radius > std::numeric_limits<float>::max() ||
      !finitePoint(sphere.center))
    throw std::invalid_argument("sphere radius and center must be finite; radius must be positive");
  if (sphere.radius / options.voxelSize < 1.5)
    throw std::invalid_argument("sphere radius must span at least 1.5 voxels");
  std::array<double, 3> minimum{}, maximum{};
  for (size_t axis = 0; axis < 3; ++axis) {
    minimum[axis] = sphere.center[axis] - sphere.radius;
    maximum[axis] = sphere.center[axis] + sphere.radius;
    if (std::abs(sphere.center[axis]) > std::numeric_limits<float>::max())
      throw std::invalid_argument("sphere center exceeds float coordinate limits");
  }
  validateBounds(minimum, maximum, options.voxelSize, options.halfWidth + 1.0, options);
  initialize();
  auto grid = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(
      static_cast<float>(sphere.radius),
      openvdb::Vec3f(static_cast<float>(sphere.center[0]),
                    static_cast<float>(sphere.center[1]),
                    static_cast<float>(sphere.center[2])),
      static_cast<float>(options.voxelSize), static_cast<float>(options.halfWidth));
  return writeGrid(std::move(grid), output, options);
#else
  (void)output; (void)sphere; (void)options;
  unavailable();
#endif
}

ProcessingResult WriteMeshLevelSet(const dotbim::Model& model,
                                    const std::filesystem::path& output,
                                    const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options);
  validateOutput(output);
  validateMeshImport(model);
  std::vector<openvdb::Vec3s> points;
  std::vector<openvdb::Vec3I> triangles;
  std::array<double, 3> minimum{
      std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::infinity()};
  auto maximum = minimum;
  for (double& x : maximum) x = -x;
  std::unordered_map<uint32_t, const dotbim::MeshRange*> ranges;
  for (const auto& range : model.meshRanges) {
    if (!ranges.emplace(range.meshId, &range).second)
      throw std::invalid_argument("duplicate mesh ID in model");
  }
  for (const auto& element : model.elements) {
    validateAffine(glm::dmat4(element.transform));
    const auto found = ranges.find(element.meshId);
    if (found == ranges.end())
      throw std::invalid_argument("model element references a missing mesh");
    const auto& range = *found->second;
    if (range.indexCount % 3 != 0 ||
        static_cast<uint64_t>(range.firstIndex) + range.indexCount > model.indices.size())
      throw std::invalid_argument("model has an invalid triangle index range");
    if (range.indexCount / 3 > options.maxTriangles - triangles.size())
      throw std::invalid_argument("model exceeds the triangle budget");
    std::unordered_map<uint32_t, uint32_t> remapped;
    for (uint64_t index = range.firstIndex;
         index < static_cast<uint64_t>(range.firstIndex) + range.indexCount; index += 3) {
      openvdb::Vec3I triangle;
      for (size_t corner = 0; corner < 3; ++corner) {
        const uint32_t sourceIndex = model.indices[static_cast<size_t>(index) + corner];
        if (sourceIndex >= model.vertices.size())
          throw std::invalid_argument("model triangle references a missing vertex");
        auto [it, inserted] = remapped.emplace(sourceIndex, 0);
        if (inserted) {
          if (points.size() >= static_cast<size_t>(std::numeric_limits<int32_t>::max()))
            throw std::invalid_argument("model exceeds the vertex index limit");
          const auto world = glm::dmat4(element.transform) *
              glm::dvec4(model.vertices[sourceIndex].position, 1.0);
          std::array<double, 3> position{world.x, world.y, world.z};
          if (!finitePoint(position) || !std::isfinite(world.w) ||
              std::abs(world.w - 1.0) > 1.0e-5)
            throw std::invalid_argument("model has an invalid affine transform or vertex");
          for (size_t axis = 0; axis < 3; ++axis) {
            if (std::abs(position[axis]) > std::numeric_limits<float>::max())
              throw std::invalid_argument("model exceeds float coordinate limits");
            minimum[axis] = std::min(minimum[axis], position[axis]);
            maximum[axis] = std::max(maximum[axis], position[axis]);
          }
          it->second = static_cast<uint32_t>(points.size());
          points.emplace_back(static_cast<float>(world.x),
                              static_cast<float>(world.y), static_cast<float>(world.z));
        }
        triangle[corner] = static_cast<int32_t>(it->second);
      }
      triangles.push_back(triangle);
    }
  }
  if (triangles.empty())
    throw std::invalid_argument("model contains no triangle surfaces");
  validateBounds(minimum, maximum, options.voxelSize, options.halfWidth + 1.0, options);
  initialize();
  const auto transform = openvdb::math::Transform::createLinearTransform(options.voxelSize);
  auto grid = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(
      *transform, points, triangles, static_cast<float>(options.halfWidth));
  if (grid->empty())
    throw std::invalid_argument("mesh generated an empty level set; check its scale and closed surfaces");
  return writeGrid(std::move(grid), output, options);
#else
  (void)model; (void)output; (void)options;
  unavailable();
#endif
}

ProcessingResult VoxelizeModel(const std::filesystem::path& input,
                               const std::filesystem::path& output,
                               float importScale, const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options);
  validateOutput(output);
  validateInput(input, options);
  if (!std::isfinite(importScale) || importScale <= 0.0f)
    throw std::invalid_argument("import scale must be finite and positive");
  return WriteMeshLevelSet(loadModelInput(input, importScale, options), output, options);
#else
  (void)input; (void)output; (void)importScale; (void)options;
  unavailable();
#endif
}

MeshConversionResult VoxelizeModelDistance(const std::filesystem::path& input,
    const std::filesystem::path& output, float importScale,
    const ProcessingOptions& options, const MeshConversionOptions& conversion) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options); validateOutput(output); validateInput(input,options);
  if (!std::isfinite(importScale) || importScale<=0)
    throw std::invalid_argument("import scale must be finite and positive");
  auto remaining=options;
  const auto encoded=std::filesystem::file_size(input);
  if (encoded>=remaining.maxInputBytes)
    throw std::invalid_argument("mesh encoded input leaves no distance-conversion workspace");
  remaining.maxInputBytes-=encoded;
  return WriteMeshDistance(loadModelInput(input,importScale,remaining,true),output,remaining,conversion);
#else
  (void)input; (void)output; (void)importScale; (void)options; (void)conversion; unavailable();
#endif
}

ProcessingResult WriteCsg(const std::filesystem::path& inputA,
                          const std::filesystem::path& inputB,
                          const std::filesystem::path& output,
                          CsgOperation operation, const std::string& gridA,
                          const std::string& gridB, const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options);
  validateOutput(output);
  auto a = readGrid(inputA, gridA, true, options);
  auto b = readGrid(inputB, gridB, true, options);
  if (a->transform() != b->transform() || a->background() != b->background())
    throw std::invalid_argument("CSG grids must have identical transforms and level-set band widths");
  // Bound a union's enclosing region before combining either tree. Even a
  // sparse result must remain safe for subsequent fog or surface operations.
  if (operation == CsgOperation::Union && !a->empty() && !b->empty()) {
    auto bounds = a->evalActiveVoxelBoundingBox();
    bounds.expand(b->evalActiveVoxelBoundingBox());
    long double count = 1.0;
    for (size_t axis = 0; axis < 3; ++axis) {
      count *= static_cast<long double>(bounds.max()[axis]) -
               static_cast<long double>(bounds.min()[axis]) + 1.0L;
      if (count > static_cast<long double>(options.maxVoxelCount))
        throw std::invalid_argument("CSG union bounds exceed the voxel budget");
    }
  }
  switch (operation) {
    case CsgOperation::Union: openvdb::tools::csgUnion(*a, *b); break;
    case CsgOperation::Intersection: openvdb::tools::csgIntersection(*a, *b); break;
    case CsgOperation::Difference: openvdb::tools::csgDifference(*a, *b); break;
    default: throw std::invalid_argument("unknown CSG operation");
  }
  return writeGrid(std::move(a), output, options);
#else
  (void)inputA; (void)inputB; (void)output; (void)operation;
  (void)gridA; (void)gridB; (void)options;
  unavailable();
#endif
}

ProcessingResult WriteFog(const std::filesystem::path& input,
                          const std::filesystem::path& output,
                          const std::string& inputGrid, const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options);
  validateOutput(output);
  auto grid = readGrid(input, inputGrid, true, options);
  openvdb::tools::sdfToFogVolume(*grid);
  grid->setGridClass(openvdb::GRID_FOG_VOLUME);
  return writeGrid(std::move(grid), output, options);
#else
  (void)input; (void)output; (void)inputGrid; (void)options;
  unavailable();
#endif
}

ProcessingResult WriteResampled(const std::filesystem::path& input,
                                 const std::filesystem::path& output,
                                 const std::string& inputGrid,
                                 const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options);
  validateOutput(output);
  const auto source = readGrid(input, inputGrid, false, options);
  const auto gridClass = source->getGridClass();
  if (gridClass != openvdb::GRID_LEVEL_SET && gridClass != openvdb::GRID_FOG_VOLUME)
    throw std::invalid_argument("resampling requires a FloatGrid level set or fog volume");
  if (gridClass == openvdb::GRID_LEVEL_SET && source->background() <= 0.0f)
    throw std::invalid_argument("level set has an invalid background value");
  if (gridClass == openvdb::GRID_FOG_VOLUME) {
    if (source->background() != 0.0f)
      throw std::invalid_argument("bounded fog resampling requires a zero background");
    for (auto value = source->cbeginValueAll(); value; ++value)
      if (*value < 0.0f)
        throw std::invalid_argument("fog resampling requires nonnegative density values");
  }
  validateResampleBounds(*source, options);
  const auto transform = openvdb::math::Transform::createLinearTransform(options.voxelSize);
  openvdb::FloatGrid::Ptr result;
  if (gridClass == openvdb::GRID_LEVEL_SET) {
    result = openvdb::tools::levelSetRebuild(*source, 0.0f,
                                            static_cast<float>(options.halfWidth),
                                            transform.get());
    if (result->empty())
      throw std::invalid_argument("level-set rebuild found no closed zero surface");
    result->setGridClass(openvdb::GRID_LEVEL_SET);
  } else {
    result = openvdb::FloatGrid::create(0.0f);
    result->setTransform(transform);
    result->setGridClass(openvdb::GRID_FOG_VOLUME);
    openvdb::tools::resampleToMatch<openvdb::tools::BoxSampler>(*source, *result);
  }
  return writeGrid(std::move(result), output, options);
#else
  (void)input; (void)output; (void)inputGrid; (void)options;
  unavailable();
#endif
}

ProcessingResult WriteOffset(const std::filesystem::path& input,
                             const std::filesystem::path& output,
                             double worldDistance, const std::string& inputGrid,
                             const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options);
  validateOutput(output);
  if (!std::isfinite(worldDistance) ||
      std::abs(worldDistance) > std::numeric_limits<float>::max() ||
      (worldDistance != 0.0 && static_cast<float>(worldDistance) == 0.0f))
    throw std::invalid_argument("offset distance must be finite and representable as a float");
  auto grid = readGrid(input, inputGrid, true, options);
  const double voxelSize = validateEditableLevelSet(*grid);
  const double voxelDistance = std::abs(worldDistance) / voxelSize;
  if (!std::isfinite(voxelDistance) || voxelDistance > 256.0)
    throw std::invalid_argument("offset magnitude must not exceed 256 source voxels");
  // Upstream intentionally ignores offsets below 0.0005 voxel. Reject a
  // requested nonzero no-op rather than reporting that the surface moved.
  if (worldDistance != 0.0 && voxelDistance < 0.001)
    throw std::invalid_argument("nonzero offset magnitude must span at least 0.001 source voxels");
  if (worldDistance != 0.0) {
    validateFilterWorkspace(*grid, 1, 3, options);
    openvdb::tools::LevelSetFilter<openvdb::FloatGrid> filter(*grid);
    const float maximumStep = 0.5f * static_cast<float>(voxelSize);
    double remaining = std::abs(static_cast<float>(worldDistance));
    // Positive SDF values are exterior. LevelSetFilter adds its offset to
    // phi, so outward expansion requires the opposite sign at this boundary.
    const float sign = worldDistance > 0.0 ? -1.0f : 1.0f;
    while (remaining >= 0.001 * double(maximumStep) && !grid->empty()) {
      validateFilterWorkspace(*grid, 1, 3, options);
      const float step = static_cast<float>(std::min(remaining, double(maximumStep)));
      filter.offset(sign * step);
      remaining -= step;
    }
  }
  return writeGrid(std::move(grid), output, options);
#else
  (void)input; (void)output; (void)worldDistance; (void)inputGrid; (void)options;
  unavailable();
#endif
}

ProcessingResult WriteSmoothed(const std::filesystem::path& input,
                               const std::filesystem::path& output,
                               SmoothingOperation operation,
                               const SmoothingOptions& smoothing,
                               const std::string& inputGrid,
                               const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options);
  validateOutput(output);
  if (smoothing.width < 1 || smoothing.width > 4 ||
      smoothing.iterations < 1 || smoothing.iterations > 32)
    throw std::invalid_argument("smoothing requires width in [1,4] and iterations in [1,32]");
  if (operation != SmoothingOperation::Gaussian && operation != SmoothingOperation::Mean &&
      operation != SmoothingOperation::Median)
    throw std::invalid_argument("unknown level-set smoothing operation");
  auto grid = readGrid(input, inputGrid, true, options);
  (void)validateEditableLevelSet(*grid);
  const uint32_t passes = operation == SmoothingOperation::Gaussian ? 4 : 1;
  const uint32_t stencil = std::max(3u, smoothing.width);
  validateFilterWorkspace(*grid, passes, stencil, options);
  openvdb::tools::LevelSetFilter<openvdb::FloatGrid> filter(*grid);
  for (uint32_t iteration = 0; iteration < smoothing.iterations && !grid->empty(); ++iteration) {
    validateFilterWorkspace(*grid, passes, stencil, options);
    switch (operation) {
      case SmoothingOperation::Gaussian: filter.gaussian(int(smoothing.width)); break;
      case SmoothingOperation::Mean: filter.mean(int(smoothing.width)); break;
      case SmoothingOperation::Median: filter.median(int(smoothing.width)); break;
    }
  }
  return writeGrid(std::move(grid), output, options);
#else
  (void)input; (void)output; (void)operation; (void)smoothing; (void)inputGrid; (void)options;
  unavailable();
#endif
}

double SampleScalar(const std::filesystem::path& input,
                     const std::array<double, 3>& position,
                     const std::string& inputGrid, const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  if (options.maxVoxelCount == 0 || options.maxInputBytes == 0)
    throw std::invalid_argument("scalar sample budgets must be positive");
  if (!finitePoint(position))
    throw std::invalid_argument("sample position must be finite");
  const auto grid = readSampleGrid(input, inputGrid, options);
  const auto encoded = std::filesystem::file_size(input);
  if (grid->isType<openvdb::FloatGrid>())
    return sampleScalarGrid(*openvdb::gridPtrCast<openvdb::FloatGrid>(grid), position, options, encoded);
  return sampleScalarGrid(*openvdb::gridPtrCast<openvdb::DoubleGrid>(grid), position, options, encoded);
#else
  (void)input; (void)position; (void)inputGrid; (void)options;
  unavailable();
#endif
}

}  // namespace container::geometry::vdb
