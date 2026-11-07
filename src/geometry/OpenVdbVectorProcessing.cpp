#include "Container/geometry/OpenVdbVectorProcessing.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/GridOperators.h>
#include <openvdb/tools/Interpolation.h>
#include <tbb/task_arena.h>
#include <mutex>
#include <set>
#include <type_traits>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB differential processing is unavailable in this build. Configure with -DENABLE_OPENVDB=ON.");
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
bool scalarOperation(DifferentialOperation operation) {
  switch (operation) {
  case DifferentialOperation::Gradient: case DifferentialOperation::Laplacian: return true;
  case DifferentialOperation::Divergence: case DifferentialOperation::Curl:
  case DifferentialOperation::Magnitude: case DifferentialOperation::Normalize: return false;
  }
  throw std::invalid_argument("unknown differential operation");
}

bool derivative(DifferentialOperation operation) {
  return operation != DifferentialOperation::Magnitude && operation != DifferentialOperation::Normalize;
}

void validateOptions(const ProcessingOptions& options) {
  if (!options.maxVoxelCount || !options.maxInputBytes)
    throw std::invalid_argument("differential processing budgets must be positive");
  if (options.gridName.empty() || options.gridName.size() > 256)
    throw std::invalid_argument("output grid name must contain between 1 and 256 bytes");
}

void validateOutput(const std::filesystem::path& output) {
  if (output.empty()) throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(output))
    throw std::invalid_argument("output already exists; choose a new .vdb path");
  if (!output.parent_path().empty() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("output directory does not exist");
}

long double metadataBytes(const openvdb::GridBase& grid) {
  long double bytes = sizeof(openvdb::MetaMap);
  for (auto field = grid.beginMeta(); field != grid.endMeta(); ++field)
    bytes += 2.0L * (field->first.size() + field->second->typeName().size() +
                     static_cast<long double>(field->second->size())) + 256;
  return bytes;
}

openvdb::GridBase::Ptr readGrid(const std::filesystem::path& input,
                               const std::string& selection, bool scalar,
                               const ProcessingOptions& options, uintmax_t& encodedBytes) {
  if (!std::filesystem::is_regular_file(input)) throw std::invalid_argument("input is not a regular file");
  encodedBytes = std::filesystem::file_size(input);
  if (encodedBytes > options.maxInputBytes)
    throw std::invalid_argument("differential input exceeds the file-size budget");
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  detail::RequireSeekableFile(input);
  openvdb::io::File file(input.string());
  file.open(false);
  // OpenVDB's synthesized duplicate-name descriptors can collide with a
  // literal grid name (e.g. field, field, field[0]). A string lookup then
  // addresses a different grid. Reject that unaddressable archive before
  // looking up any metadata, even when the caller wants the first grid.
  {
    std::set<std::string> descriptors;
    long double descriptorBytes = 0;
    for (auto it = file.beginName(); it != file.endName(); ++it) {
      const auto descriptor = it.gridName();
      descriptorBytes += 2.0L * descriptor.size() + 128;
      if (static_cast<long double>(encodedBytes) + descriptorBytes > options.maxInputBytes)
        throw std::invalid_argument("differential descriptor discovery exceeds the input-memory budget");
      if (!descriptors.insert(descriptor).second)
        throw std::invalid_argument("colliding OpenVDB grid descriptors cannot be selected safely; rename grids to unique names first");
    }
  }
  std::string descriptor;
  bool found = false;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto metadata = file.readGridMetadata(it.gridName());
    if (!GridSelectionMatches(selection, it.gridName(), metadata->getName())) continue;
    const bool supported = scalar
        ? metadata->isType<openvdb::FloatGrid>() || metadata->isType<openvdb::DoubleGrid>()
        : metadata->isType<openvdb::Vec3SGrid>() || metadata->isType<openvdb::Vec3DGrid>();
    if (!supported) {
      if (!selection.empty()) throw std::invalid_argument("selected grid has an unsupported value type for this operation");
      continue;
    }
    if (found) throw std::invalid_argument("input grid name is ambiguous; use descriptor:<exact descriptor name>");
    for (const char* key : {openvdb::GridBase::META_FILE_VOXEL_COUNT, openvdb::GridBase::META_FILE_MEM_BYTES}) {
      const auto value = metadata->getMetadata<openvdb::Int64Metadata>(key);
      const long double budget = key == openvdb::GridBase::META_FILE_VOXEL_COUNT
                                     ? options.maxVoxelCount : options.maxInputBytes;
      if (value && (value->value() < 0 || static_cast<long double>(value->value()) > budget))
        throw std::invalid_argument("differential input metadata exceeds processing budgets");
    }
    descriptor = it.gridName();
    found = true;
    if (selection.empty()) break;
  }
  if (!found) throw std::invalid_argument(scalar ? "no matching Float/Double scalar grid found"
                                               : "no matching Vec3S/Vec3D vector grid found");
  auto result = file.readGrid(descriptor);
  file.close();
  return result;
}

void validateTransform(const openvdb::GridBase& grid) {
  if (!grid.transform().isLinear())
    throw std::invalid_argument("differential processing requires a nonsingular affine transform");
  const auto map = grid.transform().baseMap();
  const auto origin = map->applyMap(openvdb::Vec3d(0));
  std::array<openvdb::Vec3d, 3> basis;
  for (size_t axis = 0; axis < 3; ++axis) {
    openvdb::Vec3d unit(0); unit[axis] = 1;
    basis[axis] = map->applyJacobian(unit);
    const double length = std::hypot(basis[axis].x(), basis[axis].y(), basis[axis].z());
    if (!std::isfinite(origin[axis]) || !std::isfinite(length) || length < 1e-6 || length > 1e6)
      throw std::invalid_argument("differential grid axes must span [1e-6,1e6] world units");
    basis[axis] /= length;
    const auto inverse = map->applyInverseJacobian(unit);
    for (size_t component = 0; component < 3; ++component)
      if (!std::isfinite(inverse[component]) || std::abs(inverse[component]) > 1e8)
        throw std::invalid_argument("differential grid transform is singular or ill-conditioned");
  }
  const double determinant = basis[0].dot(basis[1].cross(basis[2]));
  if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8)
    throw std::invalid_argument("differential grid transform is singular or ill-conditioned");
}

template<class T> bool finiteValue(const T& value, bool bound) {
  if constexpr (std::is_arithmetic_v<T>)
    return std::isfinite(double(value)) && (!bound || std::abs(double(value)) <= 1e12);
  else {
    for (size_t axis = 0; axis < 3; ++axis)
      if (!std::isfinite(double(value[axis])) || (bound && std::abs(double(value[axis])) > 1e12)) return false;
    return true;
  }
}

long double boxCount(const openvdb::CoordBBox& box, int64_t dimension = 1) {
  if (box.empty()) return 0;
  long double count = 1;
  for (size_t axis = 0; axis < 3; ++axis)
    count *= std::floor(static_cast<long double>(box.max()[axis]) / dimension) -
             std::floor(static_cast<long double>(box.min()[axis]) / dimension) + 1;
  return count;
}

template<class Grid>
void validateGrid(const Grid& grid, const ProcessingOptions& options, bool input = true) {
  validateTransform(grid);
  if constexpr (!std::is_arithmetic_v<typename Grid::ValueType>)
    if (!grid.isInWorldSpace() || grid.getGridClass() == openvdb::GRID_STAGGERED)
      throw std::invalid_argument("vector fields must be collocated and tagged as world-space components");
  constexpr uint64_t leafValues = Grid::TreeType::LeafNodeType::NUM_VALUES;
  const uint64_t maxLeaves = options.maxVoxelCount / leafValues + (options.maxVoxelCount % leafValues != 0);
  if (grid.activeVoxelCount() > options.maxVoxelCount || grid.tree().leafCount() > maxLeaves ||
      grid.memUsage() + metadataBytes(grid) > options.maxInputBytes)
    throw std::invalid_argument("differential stored topology exceeds processing budgets");
  if (!finiteValue(grid.background(), input))
    throw std::invalid_argument("stored components must be finite and input magnitudes at most 1e12");
  for (auto value = grid.cbeginValueAll(); value; ++value)
    if (!finiteValue(*value, input))
      throw std::invalid_argument("stored components must be finite and input magnitudes at most 1e12");
  const auto bounds = grid.evalActiveVoxelBoundingBox();
  if (boxCount(bounds) > options.maxVoxelCount)
    throw std::invalid_argument("differential active bounds exceed the voxel budget");
  // CD_2ND uses +/-1 neighbors, including mixed second derivatives. Reject
  // stored leaf boundaries at the integer edge before the upstream stencil.
  for (auto leaf = grid.tree().cbeginLeaf(); leaf; ++leaf) {
    const auto box = leaf->getNodeBoundingBox();
    for (size_t axis = 0; axis < 3; ++axis)
      if (int64_t(box.min()[axis]) <= std::numeric_limits<int32_t>::min() ||
          int64_t(box.max()[axis]) >= std::numeric_limits<int32_t>::max())
        throw std::invalid_argument("differential stencils exceed safe OpenVDB coordinate limits");
  }
  if (!bounds.empty())
    for (size_t axis = 0; axis < 3; ++axis) {
      if (int64_t(bounds.min()[axis]) < int64_t(std::numeric_limits<int32_t>::min()) + 2 ||
          int64_t(bounds.max()[axis]) > int64_t(std::numeric_limits<int32_t>::max()) - 2)
        throw std::invalid_argument("differential stencils exceed safe OpenVDB coordinate limits");
      for (const auto coordinate : {bounds.min(), bounds.max()})
        if (!finiteValue(grid.indexToWorld(coordinate.asVec3d()), false))
          throw std::invalid_argument("differential grid bounds have non-finite world coordinates");
    }
}

template<class Grid>
void preflight(const Grid& grid, uintmax_t encoded, bool densify, const ProcessingOptions& options) {
  using LargestTree = openvdb::Vec3DTree;
  using Leaf = LargestTree::LeafNodeType;
  using Upper = LargestTree::RootNodeType::ChildNodeType;
  using Lower = Upper::ChildNodeType;
  const auto count = grid.tree().nodeCount();
  const auto bounds = grid.evalActiveVoxelBoundingBox();
  const long double leaves = count[0] + (densify ? boxCount(bounds, Leaf::DIM) : 0);
  const long double lower = count[1] + (densify ? boxCount(bounds, Lower::DIM) : 0);
  const long double upper = count[2] + (densify ? boxCount(bounds, Upper::DIM) : 0);
  const uint64_t maxLeaves = options.maxVoxelCount / Leaf::NUM_VALUES + (options.maxVoxelCount % Leaf::NUM_VALUES != 0);
  // Existing and prospective regions may overlap; counting both deliberately
  // overestimates topology instead of missing inactive remote leaves.
  if (leaves > maxLeaves)
    throw std::invalid_argument("differential expanded leaf topology exceeds the voxel budget");
  const long double outputTree = leaves * (sizeof(Leaf) + Leaf::NUM_VALUES * sizeof(openvdb::Vec3d) + 128.0L) +
      lower * (sizeof(Lower) + 128.0L) + upper * (sizeof(Upper) + 384.0L) + sizeof(LargestTree);
  const long double bytes = static_cast<long double>(encoded) + grid.memUsage() +
      3 * outputTree + 4 * metadataBytes(grid) + 4.0L * 1024 * 1024 + 8 * (options.gridName.size() + 16);
  if (bytes > options.maxInputBytes)
    throw std::invalid_argument("differential output/scratch workspace exceeds the input-memory budget");
}

template<class Grid>
typename Grid::Ptr preconditionNormalize(const Grid& input) {
  auto result = detail::DeepCopyGridExact(input);
  const auto scaled = [](const typename Grid::ValueType& vector) {
    const double scale = std::max({std::abs(double(vector[0])), std::abs(double(vector[1])), std::abs(double(vector[2]))});
    if (scale == 0) return typename Grid::ValueType(0);
    return typename Grid::ValueType(double(vector[0]) / scale, double(vector[1]) / scale, double(vector[2]) / scale);
  };
  result->tree().root().setBackground(scaled(input.background()), false);
  for (auto value = result->beginValueAll(); value; ++value) {
    const bool active = value.isValueOn();
    value.setValue(scaled(*value));
    value.setActiveState(active);
  }
  return result;
}

template<class Grid>
openvdb::GridBase::Ptr applyScalar(const Grid& input, DifferentialOperation operation) {
  if (operation == DifferentialOperation::Gradient) return openvdb::tools::gradient(input, false);
  return openvdb::tools::laplacian(input, false);
}

template<class Grid>
openvdb::GridBase::Ptr applyVector(const Grid& input, DifferentialOperation operation) {
  if (operation == DifferentialOperation::Divergence) return openvdb::tools::divergence(input, false);
  if (operation == DifferentialOperation::Curl) return openvdb::tools::curl(input, false);
  if (operation == DifferentialOperation::Normalize)
    return openvdb::tools::normalize(*preconditionNormalize(input), false);
  auto result = openvdb::tools::magnitude(input, false);
  // Upstream length squares in the source component type, underflowing for
  // valid tiny vectors. Repair pointwise magnitudes with scaled std::hypot;
  // topology and active/inactive output semantics remain the upstream result.
  using Scalar = typename decltype(result)::element_type::ValueType;
  const auto length = [](const auto& vector) { return std::hypot(double(vector[0]), double(vector[1]), double(vector[2])); };
  const Scalar background = Scalar(length(input.background()));
  result->tree().root().setBackground(background, false);
  for (auto value = result->beginValueAll(); value; ++value) {
    const bool active = value.isValueOn();
    value.setValue(active ? Scalar(length(input.tree().getValue(value.getCoord()))) : background);
    value.setActiveState(active);
  }
  return result;
}

template<class Function>
auto dispatch(const openvdb::GridBase::Ptr& grid, Function&& function) {
  if (auto typed = openvdb::gridPtrCast<openvdb::FloatGrid>(grid)) return function(*typed);
  if (auto typed = openvdb::gridPtrCast<openvdb::DoubleGrid>(grid)) return function(*typed);
  if (auto typed = openvdb::gridPtrCast<openvdb::Vec3SGrid>(grid)) return function(*typed);
  if (auto typed = openvdb::gridPtrCast<openvdb::Vec3DGrid>(grid)) return function(*typed);
  throw std::invalid_argument("unsupported grid value type");
}

ProcessingResult writeGrid(const openvdb::GridBase::Ptr& result,
                           const std::filesystem::path& output, const ProcessingOptions& options) {
  detail::WriteFileExclusive(output, openvdb::GridPtrVec{result}, options.maxInputBytes);
  return {result->getName(), "unknown", uint64_t(result->activeVoxelCount()), result->voxelSize().x()};
}

template<class Grid>
std::array<double, 3> sample(const Grid& grid, const std::array<double, 3>& position) {
  if constexpr (std::is_arithmetic_v<typename Grid::ValueType>)
    throw std::invalid_argument("vector sampling requires a Vec3S/Vec3D grid");
  else {
    const openvdb::Vec3d world(position[0], position[1], position[2]);
    if (!finiteValue(world, false)) throw std::invalid_argument("sample world position must be finite");
    // AffineMap's absolute inverse-matrix multiplication can cancel several
    // index cells when a modest voxel scale accompanies a large translation.
    // Subtract the world origin first, then apply only the inverse Jacobian.
    const auto map = grid.transform().baseMap();
    const auto index = map->applyInverseJacobian(world - map->applyMap(openvdb::Vec3d(0)));
    for (size_t axis = 0; axis < 3; ++axis) {
      if (!std::isfinite(index[axis]) || index[axis] < double(std::numeric_limits<int32_t>::min()) + 2 ||
          index[axis] > double(std::numeric_limits<int32_t>::max()) - 2)
        throw std::invalid_argument("vector sample exceeds safe OpenVDB coordinate limits");
      const double spacing = std::max(
          std::nextafter(index[axis], std::numeric_limits<double>::infinity()) - index[axis],
          index[axis] - std::nextafter(index[axis], -std::numeric_limits<double>::infinity()));
      if (spacing > 1e-8)
        throw std::invalid_argument("vector sample cannot resolve subvoxel coordinates to 1e-8 voxels; recenter the source grid");
    }
    auto access = grid.getConstAccessor();
    openvdb::tools::GridSampler<decltype(access), openvdb::tools::BoxSampler> sampler(access, grid.transform());
    const auto value = sampler.isSample(index);
    if (!finiteValue(value, false)) throw std::invalid_argument("vector sample produced a non-finite value");
    return {double(value[0]), double(value[1]), double(value[2])};
  }
}
#endif
} // namespace

ProcessingResult WriteDifferential(const std::filesystem::path& input,
                                  const std::filesystem::path& output,
                                  DifferentialOperation operation, const std::string& inputGrid,
                                  const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  const bool scalar = scalarOperation(operation);
  validateOptions(options);
  validateOutput(output);
  tbb::task_arena arena(1);
  return arena.execute([&] {
    uintmax_t encoded = 0;
    auto inputData = readGrid(input, inputGrid, scalar, options, encoded);
    auto result = dispatch(inputData, [&](const auto& grid) -> openvdb::GridBase::Ptr {
      validateGrid(grid, options);
      preflight(grid, encoded, derivative(operation), options);
      if constexpr (std::is_arithmetic_v<typename std::decay_t<decltype(grid)>::ValueType>) return applyScalar(grid, operation);
      else return applyVector(grid, operation);
    });
    const auto vectorType = result->getVectorType();
    // Upstream operators copy through Transform::copy(), which may simplify.
    result->setTransform(detail::CopyTransformExact(inputData->transform()));
    result->insertMeta(*inputData);
    detail::RemoveBoundaryProjectionMetadata(*result);
    result->setName(options.gridName);
    result->setGridClass(openvdb::GRID_UNKNOWN);
    result->setIsInWorldSpace(true);
    result->setVectorType(result->isType<openvdb::FloatGrid>() || result->isType<openvdb::DoubleGrid>()
                              ? openvdb::VEC_INVARIANT : vectorType);
    dispatch(result, [&](const auto& grid) { validateGrid(grid, options, false); return 0; });
    if (static_cast<long double>(encoded) + inputData->memUsage() + result->memUsage() +
        metadataBytes(*inputData) + 2 * metadataBytes(*result) > options.maxInputBytes)
      throw std::invalid_argument("differential retained output exceeds the input-memory budget");
    return writeGrid(result, output, options);
  });
#else
  (void)input; (void)output; (void)operation; (void)inputGrid; (void)options;
  unavailable();
#endif
}

std::array<double, 3> SampleVector(const std::filesystem::path& input,
                                 const std::array<double, 3>& position,
                                 const std::string& inputGrid, const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  if (!options.maxVoxelCount || !options.maxInputBytes)
    throw std::invalid_argument("vector sample budgets must be positive");
  tbb::task_arena arena(1);
  return arena.execute([&] {
    uintmax_t encoded = 0;
    auto inputData = readGrid(input, inputGrid, false, options, encoded);
    return dispatch(inputData, [&](const auto& grid) -> std::array<double, 3> {
      validateGrid(grid, options);
      if (static_cast<long double>(encoded) + grid.memUsage() + metadataBytes(grid) > options.maxInputBytes)
        throw std::invalid_argument("vector sample resident input exceeds the input-memory budget");
      return sample(grid, position);
    });
  });
#else
  (void)input; (void)position; (void)inputGrid; (void)options;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
