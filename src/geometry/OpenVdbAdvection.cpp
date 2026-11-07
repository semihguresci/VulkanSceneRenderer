#include "Container/geometry/OpenVdbAdvection.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include "OpenVdbTransformUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/VolumeAdvect.h>
#include <tbb/task_arena.h>
#include <mutex>
#endif

namespace container::geometry::vdb {
namespace {

[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB advection is unavailable in this build. Configure with -DENABLE_OPENVDB=ON.");
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
void initialize() {
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
}

void validateOptions(const ProcessingOptions& options) {
  if (!std::isfinite(options.voxelSize) || options.voxelSize < 1e-6 || options.voxelSize > 1e6)
    throw std::invalid_argument("advection requires a voxel size in [1e-6,1e6] world units");
  if (!options.maxVoxelCount || !options.maxInputBytes || !options.maxTriangles || options.gridName.empty())
    throw std::invalid_argument("processing budgets and output grid name must not be empty");
}

void validateOutput(const std::filesystem::path& output) {
  if (output.empty()) throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(output))
    throw std::invalid_argument("output already exists; choose a new .vdb path");
  if (!output.parent_path().empty() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("output directory does not exist");
}

uintmax_t inputBytes(const std::filesystem::path& input, const ProcessingOptions& options) {
  if (!std::filesystem::is_regular_file(input))
    throw std::invalid_argument("input is not a regular file");
  const auto bytes = std::filesystem::file_size(input);
  if (bytes > options.maxInputBytes)
    throw std::invalid_argument("input exceeds the file-size budget");
  return bytes;
}

openvdb::GridBase::Ptr readSelected(const std::filesystem::path& path,
                                   const std::string& selection, bool velocity,
                                   const ProcessingOptions& options, uintmax_t residentBytes = 0) {
  openvdb::io::File file(path.string());
  file.open(false);
  std::string descriptor;
  bool found = false;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto name = it.gridName();
    const auto metadata = file.readGridMetadata(name);
    const bool supported = velocity
        ? metadata->isType<openvdb::Vec3SGrid>() || metadata->isType<openvdb::Vec3DGrid>()
        : metadata->isType<openvdb::FloatGrid>() && metadata->getGridClass() == openvdb::GRID_FOG_VOLUME;
    if (!supported || !GridSelectionMatches(selection, name, metadata->getName())) continue;
    if (found)
      throw std::invalid_argument("input grid name is ambiguous; use descriptor:<unique descriptor name>");
    for (const char* key : {openvdb::GridBase::META_FILE_VOXEL_COUNT, openvdb::GridBase::META_FILE_MEM_BYTES}) {
      const auto value = metadata->getMetadata<openvdb::Int64Metadata>(key);
      const uintmax_t budget = key == openvdb::GridBase::META_FILE_VOXEL_COUNT
                                  ? options.maxVoxelCount : options.maxInputBytes - residentBytes;
      if (value && (value->value() < 0 || uintmax_t(value->value()) > budget))
        throw std::invalid_argument("input grid metadata exceeds processing budgets");
    }
    descriptor = name;
    found = true;
    if (selection.empty()) break;
  }
  if (!found)
    throw std::invalid_argument(velocity ? "no matching Vec3S/Vec3D velocity grid found"
                                         : "no matching FloatGrid fog volume found");
  auto grid = file.readGrid(descriptor);
  file.close();
  return grid;
}

void validateStoredTopology(const openvdb::GridBase& grid, uint64_t leaves,
                            const ProcessingOptions& options) {
  constexpr uint64_t leafValues = openvdb::FloatTree::LeafNodeType::NUM_VALUES;
  const uint64_t leafBudget = options.maxVoxelCount / leafValues + (options.maxVoxelCount % leafValues != 0);
  if (grid.activeVoxelCount() > options.maxVoxelCount || leaves > leafBudget ||
      uintmax_t(grid.memUsage()) > options.maxInputBytes)
    throw std::invalid_argument("stored grid topology exceeds processing budgets");
}

double validateTransform(const openvdb::GridBase& grid, bool uniform) {
  if (!grid.transform().isLinear())
    throw std::invalid_argument("advection requires nonsingular affine grid transforms");
  const auto origin = grid.indexToWorld(openvdb::Vec3d(0));
  const auto map = grid.transform().baseMap();
  std::array<openvdb::Vec3d, 3> basis{
      map->applyJacobian(openvdb::Vec3d(1,0,0)),
      map->applyJacobian(openvdb::Vec3d(0,1,0)),
      map->applyJacobian(openvdb::Vec3d(0,0,1))};
  std::array<double, 3> lengths{};
  for (size_t axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(origin[axis]))
      throw std::invalid_argument("grid transform has non-finite world coordinates");
    lengths[axis] = std::hypot(basis[axis].x(), basis[axis].y(), basis[axis].z());
    if (!std::isfinite(lengths[axis]) || lengths[axis] < 1e-6 || lengths[axis] > 1e6)
      throw std::invalid_argument("advection grid voxel axes must span [1e-6,1e6] world units");
    basis[axis] /= lengths[axis];
  }
  const double determinant = basis[0].dot(basis[1].cross(basis[2]));
  if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
    throw std::invalid_argument("advection grid transform is singular or ill-conditioned");
  if (uniform && (!grid.hasUniformVoxels() ||
      std::abs(lengths[1] / lengths[0] - 1.0) > 1e-6 ||
      std::abs(lengths[2] / lengths[0] - 1.0) > 1e-6 ||
      std::abs(basis[0].dot(basis[1])) > 1e-6 ||
      std::abs(basis[0].dot(basis[2])) > 1e-6 ||
      std::abs(basis[1].dot(basis[2])) > 1e-6))
    throw std::invalid_argument("fog advection requires uniform affine voxels; resample the fog first");
  return grid.voxelSize().x();
}

void validateBounds(const openvdb::CoordBBox& bounds, const ProcessingOptions& options) {
  if (bounds.empty()) return;
  long double count = 1;
  for (size_t axis = 0; axis < 3; ++axis) {
    count *= static_cast<long double>(bounds.max()[axis]) - bounds.min()[axis] + 1;
    if (count > options.maxVoxelCount)
      throw std::invalid_argument("grid bounds exceed the voxel budget");
  }
}

void validateFog(const openvdb::FloatGrid& grid, const ProcessingOptions& options) {
  validateStoredTopology(grid, grid.tree().leafCount(), options);
  (void)validateTransform(grid, true);
  if (grid.getGridClass() != openvdb::GRID_FOG_VOLUME || grid.background() != 0.0f)
    throw std::invalid_argument("fog advection requires a zero-background FloatGrid fog volume");
  for (auto value = grid.cbeginValueAll(); value; ++value)
    if (!std::isfinite(*value) || *value < 0.0f || *value > 1e6f ||
        (!value.isValueOn() && *value != 0.0f))
      throw std::invalid_argument("fog density must be finite in [0,1e6] with inactive values zero");
  validateBounds(grid.evalActiveVoxelBoundingBox(), options);
}

template<class Grid>
double validateVelocity(const Grid& grid, const ProcessingOptions& options) {
  validateStoredTopology(grid, grid.tree().leafCount(), options);
  (void)validateTransform(grid, false);
  if (!grid.isInWorldSpace() || grid.getGridClass() == openvdb::GRID_STAGGERED ||
      (grid.getVectorType() != openvdb::VEC_INVARIANT &&
       grid.getVectorType() != openvdb::VEC_CONTRAVARIANT_RELATIVE))
    throw std::invalid_argument("velocity must be collocated world-space invariant or contravariant-relative vectors");
  const auto speed = [](const auto& vector) {
    for (size_t axis = 0; axis < 3; ++axis)
      if (!std::isfinite(double(vector[axis])) || std::abs(double(vector[axis])) > 1e12)
        throw std::invalid_argument("velocity components must be finite with magnitude at most 1e12 world units per second");
    return std::hypot(double(vector.x()), double(vector.y()), double(vector.z()));
  };
  double maximum = speed(grid.background());
  auto bounds = grid.evalActiveVoxelBoundingBox();
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    maximum = std::max(maximum, speed(*value));
    if (*value != grid.background()) bounds.expand(value.getBoundingBox());
  }
  validateBounds(bounds, options);
  return maximum;
}

openvdb::Vec3DGrid::Ptr promoteVelocity(const openvdb::Vec3SGrid& source) {
  // OpenVDB's value-converting tree constructor rejects vec3s->vec3d at
  // runtime. Copy topology separately and convert stored values explicitly.
  auto result = openvdb::Vec3DGrid::create(); result->insertMeta(source);
  result->setTransform(detail::CopyTransformExact(source.transform()));
  const auto background = source.background();
  result->setTree(openvdb::Vec3DTree::Ptr(new openvdb::Vec3DTree(source.tree(),
      openvdb::Vec3d(background.x(), background.y(), background.z()), openvdb::TopologyCopy())));
  auto target = result->beginValueAll();
  for (auto value = source.cbeginValueAll(); value; ++value, ++target) {
    if (!target || value.getLevel() != target.getLevel() || value.getCoord() != target.getCoord() ||
        value.isValueOn() != target.isValueOn())
      throw std::logic_error("velocity topology changed during Vec3S promotion");
    const auto vector = *value;
    target.setValue(openvdb::Vec3d(vector.x(), vector.y(), vector.z()));
    // ValueAll::setValue marks the stored item active, including a tile.
    target.setActiveState(value.isValueOn());
  }
  if (target) throw std::logic_error("velocity topology changed during Vec3S promotion");
  return result;
}

ProcessingResult writeGrid(const openvdb::GridBase::Ptr& grid,
                           const std::filesystem::path& output,
                           const ProcessingOptions& options, const char* gridClass) {
  detail::RemoveBoundaryProjectionMetadata(*grid);
  grid->setName(options.gridName);
  ProcessingResult result{grid->getName(), gridClass, grid->activeVoxelCount(), grid->voxelSize().x()};
  detail::WriteFileExclusive(output, openvdb::GridPtrVec{grid}, options.maxInputBytes);
  return result;
}

// Conservatively reserve both expanded scalar trees, all existing remote
// inactive leaves, prospective internal nodes and traversal/dilation scratch.
void preflight(const openvdb::FloatGrid& fog, const openvdb::Vec3DGrid& velocity,
               double displacement, const ProcessingOptions& options) {
  const auto bounds = fog.evalActiveVoxelBoundingBox();
  if (bounds.empty()) return;
  const int64_t padding = int64_t(std::ceil(std::sqrt(3.0) * displacement)) + 3;
  std::array<int64_t, 3> minimum{}, maximum{};
  long double voxels = 1;
  for (size_t axis = 0; axis < 3; ++axis) {
    minimum[axis] = int64_t(bounds.min()[axis]) - padding;
    maximum[axis] = int64_t(bounds.max()[axis]) + padding;
    const int64_t samplingHalo = int64_t(std::ceil(displacement)) + 3;
    if (minimum[axis] - samplingHalo < std::numeric_limits<int32_t>::min() ||
        maximum[axis] + samplingHalo > std::numeric_limits<int32_t>::max())
      throw std::invalid_argument("advection sampling exceeds OpenVDB coordinate limits");
    voxels *= static_cast<long double>(maximum[axis] - minimum[axis] + 1);
  }
  if (voxels > options.maxVoxelCount)
    throw std::invalid_argument("advection expansion exceeds the voxel budget");
  auto nodes = [&](int64_t dimension) {
    long double count = 1;
    for (size_t axis = 0; axis < 3; ++axis)
      count *= std::floor(static_cast<long double>(maximum[axis]) / dimension) -
               std::floor(static_cast<long double>(minimum[axis]) / dimension) + 1;
    return count;
  };
  using Leaf = openvdb::FloatTree::LeafNodeType;
  using Upper = openvdb::FloatTree::RootNodeType::ChildNodeType;
  using Lower = Upper::ChildNodeType;
  const long double leaves = nodes(Leaf::DIM) + fog.tree().leafCount();
  const uint64_t leafBudget = options.maxVoxelCount / Leaf::NUM_VALUES + (options.maxVoxelCount % Leaf::NUM_VALUES != 0);
  if (leaves > leafBudget)
    throw std::invalid_argument("advection stored topology exceeds the voxel budget");
  const long double expanded = nodes(Leaf::DIM) * sizeof(Leaf) +
      nodes(Lower::DIM) * sizeof(Lower) + nodes(Upper::DIM) * sizeof(Upper);
  const long double workspace = velocity.memUsage() + 4.0L * fog.memUsage() +
      4.0L * expanded + leaves * (4ull * Leaf::NUM_VALUES * sizeof(float) + 256ull);
  if (workspace > options.maxInputBytes)
    throw std::invalid_argument("advection workspace exceeds the input-memory budget");
  // MID backtracking samples velocity at world-space output positions and at
  // a midpoint up to one displacement away. Bound both affine samplers before
  // upstream performs unchecked floor-to-int coordinate conversions.
  const double worldMargin = displacement * fog.voxelSize().x();
  for (unsigned corner = 0; corner < 8; ++corner) {
    openvdb::Vec3d index;
    for (size_t axis = 0; axis < 3; ++axis)
      index[axis] = (corner & (1u << axis)) ? double(maximum[axis]) : double(minimum[axis]);
    const auto center = fog.indexToWorld(index);
    for (size_t axis = 0; axis < 3; ++axis) {
      auto next = index;
      next[axis] += 1.0;
      openvdb::Vec3d unit(0.0);
      unit[axis] = 1.0;
      const auto expected = fog.transform().baseMap()->applyJacobian(unit);
      const auto error = fog.indexToWorld(next) - center - expected;
      if (std::hypot(error.x(), error.y(), error.z()) > 1e-4 * fog.voxelSize().x())
        throw std::invalid_argument("fog world coordinates exceed advection integration precision; recenter the grid");
    }
    for (unsigned margin = 0; margin < 8; ++margin) {
      auto world = center;
      for (size_t axis = 0; axis < 3; ++axis)
        world[axis] += (margin & (1u << axis)) ? worldMargin : -worldMargin;
      for (size_t axis = 0; axis < 3; ++axis) {
        // A full voxel can remain exactly representable while fractional RK2
        // displacements round to a substantially different motion. Bound the
        // actual world-space ULP over the complete backtracking envelope too.
        const double upper = std::nextafter(world[axis], std::numeric_limits<double>::infinity());
        const double lower = std::nextafter(world[axis], -std::numeric_limits<double>::infinity());
        const double spacing = std::max(upper - world[axis], world[axis] - lower);
        if (!std::isfinite(world[axis]) || !std::isfinite(spacing) ||
            spacing > 1e-4 * fog.voxelSize().x())
          throw std::invalid_argument("fog world coordinates exceed advection integration precision; recenter the grid");
      }
      const auto sampled = velocity.worldToIndex(world);
      for (size_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(sampled[axis]) ||
            sampled[axis] < double(std::numeric_limits<int32_t>::min()) + 3.0 ||
            sampled[axis] > double(std::numeric_limits<int32_t>::max()) - 3.0)
          throw std::invalid_argument("velocity sampling exceeds OpenVDB coordinate limits");
    }
  }
}
#endif
} // namespace

ProcessingResult WriteConstantVelocity(const std::filesystem::path& output,
                                      const std::array<double, 3>& vector,
                                      const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options);
  validateOutput(output);
  if (options.voxelSize < 1.5e-5)
    throw std::invalid_argument("constant velocity generation requires voxel size in [1.5e-5,1e6] world units");
  for (double value : vector)
    if (!std::isfinite(value) || std::abs(value) > 1e12)
      throw std::invalid_argument("velocity components must be finite with magnitude at most 1e12 world units per second");
  initialize();
  auto velocity = openvdb::Vec3DGrid::create(openvdb::Vec3d(vector[0], vector[1], vector[2]));
  velocity->setTransform(openvdb::math::Transform::createLinearTransform(options.voxelSize));
  velocity->setGridClass(openvdb::GRID_UNKNOWN);
  velocity->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  velocity->setIsInWorldSpace(true);
  (void)validateVelocity(*velocity, options);
  return writeGrid(velocity, output, options, "vector field");
#else
  (void)output; (void)vector; (void)options;
  unavailable();
#endif
}

ProcessingResult WriteAdvectedFog(const std::filesystem::path& input,
                                 const std::filesystem::path& velocityPath,
                                 const std::filesystem::path& output,
                                 const AdvectionOptions& advection,
                                 const std::string& inputGrid, const std::string& velocityGrid,
                                 const ProcessingOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(options);
  validateOutput(output);
  if (!std::isfinite(advection.timeStep) || std::abs(advection.timeStep) > 3600.0 ||
      advection.steps < 1 || advection.steps > 64)
    throw std::invalid_argument("advection requires time in [-3600,3600] seconds and steps in [1,64]");
  const auto scalarFileBytes = inputBytes(input, options);
  const auto velocityFileBytes = inputBytes(velocityPath, options);
  if (!std::filesystem::equivalent(input, velocityPath) &&
      velocityFileBytes > options.maxInputBytes - scalarFileBytes)
    throw std::invalid_argument("combined advection files exceed the input-memory budget");
  initialize();
  auto fog = openvdb::gridPtrCast<openvdb::FloatGrid>(readSelected(input, inputGrid, false, options));
  validateFog(*fog, options);
  auto loadedVelocity = readSelected(velocityPath, velocityGrid, true, options, fog->memUsage());
  double maximumSpeed;
  openvdb::Vec3DGrid::Ptr velocity;
  if (loadedVelocity->isType<openvdb::Vec3SGrid>()) {
    const auto floats = openvdb::gridPtrCast<openvdb::Vec3SGrid>(loadedVelocity);
    maximumSpeed = validateVelocity(*floats, options);
    // Reserve both velocity types during conversion; Vec3D nodes are <2x the
    // Vec3S footprint, and both coexist with the scalar input briefly.
    if (static_cast<long double>(fog->memUsage()) + 3.0L * floats->memUsage() > options.maxInputBytes)
      throw std::invalid_argument("velocity conversion workspace exceeds the input-memory budget");
    velocity = promoteVelocity(*floats);
    maximumSpeed = validateVelocity(*velocity, options);
  } else {
    velocity = openvdb::gridPtrCast<openvdb::Vec3DGrid>(loadedVelocity);
    maximumSpeed = validateVelocity(*velocity, options);
  }
  loadedVelocity.reset();
  if (static_cast<long double>(fog->memUsage()) + velocity->memUsage() > options.maxInputBytes)
    throw std::invalid_argument("combined advection grids exceed the input-memory budget");
  // Morphology uses its own parallel mask-tree pools even when the advector's
  // grain size is zero. A one-thread arena bounds those upstream allocations.
  tbb::task_arena arena(1);
  return arena.execute([&]() -> ProcessingResult {
  const double voxelSize = fog->voxelSize().x();
  const double totalDisplacement = maximumSpeed * std::abs(advection.timeStep) / voxelSize;
  const double displacement = totalDisplacement / advection.steps;
  if (!std::isfinite(totalDisplacement) || totalDisplacement > 256.0 || displacement > 16.0)
    throw std::invalid_argument("advection displacement must be <=256 source voxels total and <=16 per step; increase steps or reduce time");
  if (advection.timeStep != 0.0 && maximumSpeed != 0.0 && !fog->empty()) {
    // VolumeAdvection internally calls Grid::copyWithNewTree(), whose native
    // Transform::copy() can change translated scales or tiny shear. Until this
    // upstream algorithm is conditioned in a local frame, reject such maps
    // rather than silently sampling an altered lattice.
    const auto expected=fog->transform().baseMap()->getAffineMap()->getMat4();
    const auto copied=[&] {
      try {return fog->transform().copy()->baseMap()->getAffineMap()->getMat4();}
      catch(const openvdb::ArithmeticError&) {
        throw std::invalid_argument("upstream fog advection cannot copy this affine map exactly; recenter the source first");
      }
    }();
    for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c)
      if(expected[r][c]!=copied[r][c]) throw std::invalid_argument("upstream fog advection cannot copy this affine map exactly; recenter the source first");
    const double dt = advection.timeStep / advection.steps;
    if (dt == 0.0 || displacement == 0.0)
      throw std::invalid_argument("nonzero advection timestep is below integration precision");
    preflight(*fog, *velocity, displacement, options);
    // Upstream prunes values approximately equal to zero using Delta<float>
    // (1e-5). Work in peak-normalized units so this threshold is relative and
    // an otherwise valid, uniformly dilute density field is not erased.
    float peak = 0.0f;
    for (auto value = fog->cbeginValueOn(); value; ++value) peak = std::max(peak, *value);
    if (peak == 0.0f) return writeGrid(fog, output, options, "fog volume");
    for (auto value = fog->beginValueOn(); value; ++value)
      value.setValue(static_cast<float>(double(*value) / double(peak)));
    openvdb::tools::VolumeAdvection<openvdb::Vec3DGrid, false> advector(*velocity);
    advector.setIntegrator(openvdb::tools::Scheme::MID);
    advector.setSubSteps(1);
    advector.setGrainSize(0); // Bounded serial scratch; no per-worker caches.
    const double engineDisplacement = advector.getMaxVelocity() * std::abs(dt) / voxelSize;
    if (!std::isfinite(engineDisplacement) || engineDisplacement > 16.0 + 1e-9)
      throw std::invalid_argument("velocity magnitude exceeds the bounded advection step");
    const int upstreamPadding = int(std::ceil(engineDisplacement));
    const int safePadding = int(std::ceil(std::sqrt(3.0) * engineDisplacement)) + 3;
    for (uint32_t step = 0; step < advection.steps && !fog->empty(); ++step) {
      preflight(*fog, *velocity, engineDisplacement, options);
      // Upstream dilates with face neighbours using L2 travel. An additional
      // background halo covers diagonal L1 travel plus interpolation support.
      openvdb::tools::dilateActiveValues(fog->tree(), safePadding - upstreamPadding,
                                        openvdb::tools::NN_FACE, openvdb::tools::EXPAND_TILES);
      fog = advector.advect<openvdb::FloatGrid, openvdb::tools::BoxSampler>(*fog, dt);
      validateFog(*fog, options);
    }
    for (auto value = fog->beginValueOn(); value; ++value)
      value.setValue(static_cast<float>(double(*value) * double(peak)));
    validateFog(*fog, options);
  }
  return writeGrid(fog, output, options, "fog volume");
  });
#else
  (void)input; (void)velocityPath; (void)output; (void)advection;
  (void)inputGrid; (void)velocityGrid; (void)options;
  unavailable();
#endif
}

} // namespace container::geometry::vdb
