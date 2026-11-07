#include "Container/geometry/OpenVdbNativePointAdvection.h"
#include "OpenVdbPointMotionCore.h"

namespace container::geometry::vdb {
namespace {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB

struct VelocitySource {
  std::unique_ptr<Snapshot> snapshot;
  openvdb::GridBase::Ptr grid;
  std::unique_ptr<Affine> frame;
  long double resident{}, metadata{}, validationWork{}, full{};
  V background{0}, envelope{0}, constantWorld{0};
  double worldSpeedBound{}, indexSpeedBound{};
  bool mac{}, constant{true};
  uint64_t leafSlots{}, active{};
};

template<class Function>
decltype(auto) velocityTyped(const openvdb::GridBase& grid, Function&& function) {
  if (grid.isType<openvdb::Vec3SGrid>())
    return function(static_cast<const openvdb::Vec3SGrid&>(grid));
  return function(static_cast<const openvdb::Vec3DGrid&>(grid));
}

V mappedVector(const std::array<V, 3>& columns, const V& value,
               double* uncertainty = nullptr) {
  V result;
  for (size_t row = 0; row != 3; ++row) {
    Sum sum;
    for (size_t axis = 0; axis != 3; ++axis)
      sum.product(columns[axis][row], value[axis]);
    result[row] = sum.get();
    if (uncertainty) *uncertainty = std::max(*uncertainty, sum.uncertainty());
  }
  return result;
}

V indexVector(const Affine& frame, const V& value) {
  V result;
  for (size_t row = 0; row != 3; ++row) {
    Sum sum;
    for (size_t axis = 0; axis != 3; ++axis)
      sum.product(frame.inverseRows[row][axis], value[axis]);
    result[row] = sum.get();
  }
  return result;
}

double length(const V& value) {
  const double result = std::hypot(value[0], value[1], value[2]);
  if (!std::isfinite(result))
    throw std::invalid_argument("native point advection vector norm exceeds Double range");
  return result;
}

void velocitySemantics(const openvdb::GridBase& grid,
                       NativePointVelocityLayout requested, bool& mac) {
  for (const char* key : {openvdb::GridBase::META_GRID_CLASS,
                         openvdb::GridBase::META_VECTOR_TYPE}) {
    if (grid[key] && !grid.getMetadata<openvdb::StringMetadata>(key))
      throw std::invalid_argument("native point velocity has wrongly typed class/vector metadata");
  }
  if (grid[openvdb::GridBase::META_IS_LOCAL_SPACE] &&
      !grid.getMetadata<openvdb::BoolMetadata>(openvdb::GridBase::META_IS_LOCAL_SPACE))
    throw std::invalid_argument("native point velocity has wrongly typed world/local metadata");
  if (const auto label = grid.getMetadata<openvdb::StringMetadata>(openvdb::GridBase::META_GRID_CLASS);
      label && label->value() != openvdb::GridBase::gridClassToString(grid.getGridClass()))
    throw std::invalid_argument("native point velocity has an unrecognized grid class label");
  if (const auto label = grid.getMetadata<openvdb::StringMetadata>(openvdb::GridBase::META_VECTOR_TYPE);
      label && label->value() != openvdb::GridBase::vecTypeToString(grid.getVectorType()))
    throw std::invalid_argument("native point velocity has an unrecognized vector semantic label");
  mac = grid.getGridClass() == openvdb::GRID_STAGGERED;
  if ((requested == NativePointVelocityLayout::World && mac) ||
      (requested == NativePointVelocityLayout::NativeMacIndex && !mac))
    throw std::invalid_argument("native point velocity layout disagrees with the selected grid class");
  if (mac) {
    const auto layout = grid.getMetadata<openvdb::StringMetadata>("container:mac_layout");
    const auto units = grid.getMetadata<openvdb::StringMetadata>("container:velocity_units");
    if (grid.isInWorldSpace() || grid.getVectorType() != openvdb::VEC_CONTRAVARIANT_RELATIVE ||
        !layout || layout->value() != "negative_faces" || !units ||
        units->value() != "voxels_per_second")
      throw std::invalid_argument("native point MAC velocity requires local negative_faces/voxels_per_second relative tags");
  } else {
    const auto type = grid.getVectorType();
    if (grid.getGridClass() != openvdb::GRID_UNKNOWN || !grid.isInWorldSpace() ||
        type < openvdb::VEC_INVARIANT || type > openvdb::VEC_CONTRAVARIANT_ABSOLUTE)
      throw std::invalid_argument("native point velocity requires UNKNOWN already-WORLD XYZ or declared native MAC");
  }
}

VelocitySource loadVelocity(const std::filesystem::path& input,
                            const std::string& selector, const Source& pointsSource,
                            const NativePointAdvectionOptions& options,
                            const ProcessingOptions& processing) {
  VelocitySource result;
  if (!std::filesystem::is_regular_file(input))
    throw std::invalid_argument("native point velocity input must be a regular file");
  budget(pointsSource.resident + std::filesystem::file_size(input) + Fixed,
         processing, "native point and velocity snapshots exceed shared memory budget");
  result.validationWork = 2.0L * std::filesystem::file_size(input);
  if (pointsSource.validationWork + result.validationWork > options.maxWork)
    throw std::invalid_argument("native point velocity encoded snapshot copy exceeds joint maxWork before copy");
  result.snapshot = std::make_unique<Snapshot>(input, processing);
  detail::RequireSeekableFile(result.snapshot->file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("native point advection requires delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD");

  auto streamMetadata = std::make_shared<openvdb::io::StreamMetadata>();
  streamMetadata->setSeekable(true);
  std::ifstream stream(result.snapshot->file, std::ios::binary);
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  MetadataArchive archive;
  archive.begin(stream, streamMetadata);
  openvdb::MetaMap fileMetadata;
  fileMetadata.readMeta(stream);
  result.metadata = metadataBytes(fileMetadata);
  long double catalog = result.snapshot->encoded + Fixed, catalogWork = 0;
  const auto check = [&] {
    if (pointsSource.metadata + result.metadata > options.maxMetadataBytes)
      throw std::invalid_argument("native point advection aggregate file/grid/descriptor metadata exceeds maxMetadataBytes");
    if (pointsSource.validationWork + result.validationWork + 8 * result.metadata + catalogWork > options.maxWork)
      throw std::invalid_argument("native point velocity aggregate encoded/catalog/metadata work exceeds maxWork");
    budget(pointsSource.resident + catalog + 12 * result.metadata, processing,
           "native point advection combined catalog/metadata exceeds byte budget");
  };
  check();
  const auto count = archive.count(stream);
  if (count < 0 || uint64_t(count) + pointsSource.catalogGridCount > options.maxGrids)
    throw std::invalid_argument("native point advection aggregate catalogs exceed maxGrids");
  std::set<std::string> descriptors;
  std::string chosen;
  openvdb::math::Transform::Ptr exact;
  bool found = false;
  for (int32_t i = 0; i != count; ++i) {
    openvdb::io::GridDescriptor descriptor;
    auto grid = archive.next(stream, descriptor, result.snapshot->encoded);
    const auto name = openvdb::io::GridDescriptor::nameAsString(descriptor.uniqueName());
    catalog += 4.0L * name.size() + 256;
    catalogWork += 4.0L * name.size() + 256;
    if (!descriptors.insert(name).second)
      throw std::invalid_argument("native point velocity has colliding grid descriptors");
    result.metadata += metadataBytes(*grid);
    check();
    if (!GridSelectionMatches(selector, name, grid->getName())) continue;
    const bool type = grid->isType<openvdb::Vec3SGrid>() || grid->isType<openvdb::Vec3DGrid>();
    if (selector.empty() && !type) continue;
    if (!type) throw std::invalid_argument("native point velocity selector requires Vec3S/Vec3D");
    if (found) throw std::invalid_argument("native point velocity selector is ambiguous; use descriptor:<exact name>");
    velocitySemantics(*grid, options.velocityLayout, result.mac);
    const auto memory = grid->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
    const auto voxels = grid->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!memory || !voxels || memory->value() < 0 || voxels->value() < 0 ||
        uint64_t(voxels->value()) > processing.maxVoxelCount)
      throw std::invalid_argument("native point velocity requires recorded memory/voxel statistics within budget");
    budget(pointsSource.resident + catalog + 12 * result.metadata + memory->value(),
           processing, "native point velocity recorded allocation exceeds shared byte budget");
    exact = detail::CopyTransformExact(grid->transform());
    result.frame = std::make_unique<Affine>(*exact, result.mac);
    chosen = name;
    found = true;
  }
  if (!found) throw std::invalid_argument("no selected Vec3S/Vec3D native point velocity grid");
  result.validationWork += 8 * result.metadata + catalogWork;
  stream.close();
  openvdb::io::File file(result.snapshot->file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  if (file.getUniqueTag() != archive.getUniqueTag())
    throw std::runtime_error("native point velocity private snapshot changed");
  result.grid = file.readGrid(chosen);
  if (!sameMap(result.grid->transform(), *exact))
    throw std::runtime_error("native point velocity read changed exact affine coefficients");
  result.grid->setTransform(exact);
  velocityTyped(*result.grid, [&](const auto& grid) {
    using VelocityTree = typename std::decay_t<decltype(grid)>::TreeType;
    using VelocityUpper = typename VelocityTree::RootNodeType::ChildNodeType;
    using VelocityLower = typename VelocityUpper::ChildNodeType;
    const auto& tree = grid.tree();
    result.leafSlots = uint64_t(tree.leafCount()) * 512;
    result.active = tree.activeVoxelCount();
    if (tree.leafCount() * 512.0L + pointsSource.grid->tree().leafCount() * 512.0L > processing.maxVoxelCount ||
        tree.activeVoxelCount() * 1.0L + pointsSource.grid->activeVoxelCount() > processing.maxVoxelCount)
      throw std::invalid_argument("native point/velocity aggregate leaf slots or activity exceeds maxVoxelCount");
    const auto nodes = tree.nodeCount();
    result.validationWork += 1 + tree.root().getTableSize() + nodes[0] * 512.0L +
        nodes[1] * static_cast<long double>(VelocityLower::NUM_VALUES) +
        nodes[2] * static_cast<long double>(VelocityUpper::NUM_VALUES);
    if (pointsSource.validationWork + result.validationWork > options.maxWork)
      throw std::invalid_argument("native point velocity stored-node traversal exceeds maxWork before scan");
    result.full = openvdb::tools::memUsageIfLoaded(tree, false);
    result.resident = catalog + 12 * result.metadata + 4 * result.full + Fixed;
    budget(pointsSource.resident + result.resident, processing,
           "native point velocity complete decoded buffers exceed shared byte budget before paging");
  });
  result.grid->readNonresidentBuffers();
  if (result.grid->memUsage() > result.full)
    throw std::runtime_error("native point velocity decoded memory exceeded full typed preflight");
  file.close();
  velocityTyped(*result.grid, [&](const auto& grid) {
    const auto bg = grid.background();
    result.background = V(bg[0], bg[1], bg[2]);
    const auto inspect = [&](const auto& value) {
      const V v(value[0], value[1], value[2]);
      for (size_t axis = 0; axis != 3; ++axis) {
        if (!std::isfinite(v[axis]))
          throw std::invalid_argument("native point velocity contains nonfinite stored/background components");
        if (!result.mac && std::abs(v[axis]) > 1e12)
          throw std::invalid_argument("native point world velocity components must be at most 1e12");
        result.envelope[axis] = std::max(result.envelope[axis], std::abs(v[axis]));
      }
      if (v != result.background) result.constant = false;
    };
    inspect(bg);
    for (auto value = grid.cbeginValueAll(); value; ++value) inspect(*value);
  });
  result.constantWorld = result.mac
      ? mappedVector(result.frame->axes, result.background) : result.background;
  V worldEnvelope, indexEnvelope;
  for (size_t row = 0; row != 3; ++row) {
    Sum world, index;
    for (size_t axis = 0; axis != 3; ++axis) {
      world.product(result.mac ? std::abs(result.frame->axes[axis][row]) : double(axis == row),
                    result.envelope[axis]);
      index.product(result.mac ? double(axis == row) : std::abs(result.frame->inverseRows[row][axis]),
                    result.envelope[axis]);
    }
    worldEnvelope[row] = world.get(); indexEnvelope[row] = index.get();
    if (worldEnvelope[row] > 1e12)
      throw std::invalid_argument("native MAC reconstructed world velocity envelope exceeds 1e12");
  }
  result.worldSpeedBound = result.constant ? length(result.constantWorld) : length(worldEnvelope);
  result.indexSpeedBound = result.constant
      ? length(result.mac ? result.background : indexVector(*result.frame, result.background))
      : length(indexEnvelope);
  if (!result.constant) {
    result.worldSpeedBound *= 1 + 128 * std::numeric_limits<double>::epsilon();
    result.indexSpeedBound *= 1 + 128 * std::numeric_limits<double>::epsilon();
  }
  return result;
}

double interpolation(double left, double right, double fraction) {
  if (fraction == 0) return left;
  if (fraction == 1) return right;
  for (const auto term : {std::pair{left, 1 - fraction}, std::pair{right, fraction}})
    if (term.first != 0 && term.second != 0 && term.first * term.second == 0)
      throw std::invalid_argument("native point velocity interpolation has a nonzero Double term underflow");
  const double result = std::lerp(left, right, fraction);
  if (!std::isfinite(result))
    throw std::invalid_argument("native point velocity interpolation overflows Double");
  return result;
}

template<class VelocityGrid>
double componentSample(const VelocityGrid& grid, const V& position, size_t component) {
  C base;
  V fraction;
  for (size_t axis = 0; axis != 3; ++axis) {
    if (!std::isfinite(position[axis]) || position[axis] < INT32_MIN + 4.0 ||
        position[axis] > INT32_MAX - 4.0)
      throw std::invalid_argument("native point velocity footprint exceeds safe native coordinates");
    base[axis] = int32_t(std::floor(position[axis]));
    fraction[axis] = position[axis] - base[axis];
    if (fraction[axis] == 1)
      throw std::invalid_argument("native point velocity loses a nonzero interpolation complement; recenter the field");
  }
  double yz[2][2];
  for (int y = 0; y != 2; ++y) for (int z = 0; z != 2; ++z) {
    const double left = double(grid.tree().getValue(base + C(0, y, z))[component]);
    const double right = double(grid.tree().getValue(base + C(1, y, z))[component]);
    yz[y][z] = interpolation(left, right, fraction[0]);
  }
  return interpolation(interpolation(yz[0][0], yz[1][0], fraction[1]),
                       interpolation(yz[0][1], yz[1][1], fraction[1]), fraction[2]);
}

struct Trajectories {
  const VelocitySource& velocity;
  const NativePointAdvectionOptions& options;
  NativePointAdvectionResult& report;
  double precisionWorld, dt;

  V fieldIndex(const V& world) const {
    const auto& frame = *velocity.frame;
    const V value = frame.index(world);
    // A small index ULP can still represent a large WORLD error on a coarse
    // field relative to fine points. Enforce the tighter world tolerance too.
    const double allowed = std::min(1e-4, precisionWorld / frame.maximum);
    double envelope = 1;
    for (size_t axis = 0; axis != 3; ++axis)
      envelope = std::max(envelope, std::abs(world[axis]) + std::abs(frame.origin[axis]));
    const long double arithmetic = 128.0L * std::numeric_limits<double>::epsilon() *
        frame.condition * frame.inverseNorm * envelope;
    if (arithmetic > allowed)
      throw std::invalid_argument("native point/velocity joint-map uncertainty loses world sampling precision; recenter maps");
    for (size_t axis = 0; axis != 3; ++axis) if (spacing(value[axis]) > allowed)
      throw std::invalid_argument("native point velocity index ULP loses point world precision; recenter field indices");
    return value;
  }

  V sample(const V& world) {
    ++report.velocitySampleCount;
    if (velocity.constant) return velocity.constantWorld;
    const V index = fieldIndex(world);
    V raw;
    velocityTyped(*velocity.grid, [&](const auto& grid) {
      for (size_t axis = 0; axis != 3; ++axis) {
        V position = index;
        if (velocity.mac) {
          const double shifted = position[axis] + .5;
          if (shifted - position[axis] != .5)
            throw std::invalid_argument("native point MAC half-face offset loses precision");
          position[axis] = shifted;
        }
        raw[axis] = componentSample(grid, position, axis);
      }
    });
    if (!velocity.mac) return raw;
    double uncertainty = 0;
    const auto result = mappedVector(velocity.frame->axes, raw, &uncertainty);
    if (uncertainty * std::abs(options.timeStep) > precisionWorld)
      throw std::invalid_argument("native point MAC world conversion loses trajectory precision");
    return result;
  }

  V increment(const V& point, const V& speed, double time) const {
    V result;
    for (size_t axis = 0; axis != 3; ++axis) {
      Sum sum;
      sum.add(point[axis]); sum.product(speed[axis], time);
      result[axis] = sum.get();
      report.maximumEndpointArithmeticEnvelopeWorld = std::max(
          report.maximumEndpointArithmeticEnvelopeWorld, sum.uncertainty());
      if (sum.uncertainty() > precisionWorld)
        throw std::invalid_argument("native point RK world endpoint loses coordinate precision");
      if (speed[axis] != 0 && time != 0 && result[axis] == point[axis])
        throw std::invalid_argument("native point nonzero RK displacement is unresolved in Double world coordinates");
    }
    return result;
  }

  V step(const V& point) {
    const V k1 = sample(point);
    const V k2 = sample(increment(point, k1, dt * .5));
    if (options.integration == NativePointIntegration::RK2) return increment(point, k2, dt);
    const V k3 = sample(increment(point, k2, dt * .5));
    const V k4 = sample(increment(point, k3, dt));
    V combined;
    for (size_t axis = 0; axis != 3; ++axis) {
      Sum sum;
      sum.product(k1[axis], 1.0 / 6); sum.product(k2[axis], 1.0 / 3);
      sum.product(k3[axis], 1.0 / 3); sum.product(k4[axis], 1.0 / 6);
      combined[axis] = sum.get();
    }
    return increment(point, combined, dt);
  }

  V operator()(const V& original) {
    V current = original;
    double pathIndex = 0, pathWorld = 0;
    for (uint32_t i = 0; i != options.steps; ++i) {
      const V next = step(current);
      V delta;
      for (size_t axis = 0; axis != 3; ++axis) {
        Sum sum; sum.add(next[axis]); sum.add(-current[axis]); delta[axis] = sum.get();
      }
      const double indexDistance = length(indexVector(*velocity.frame, delta));
      const double worldDistance = length(delta);
      pathIndex += indexDistance; pathWorld += worldDistance;
      if (!std::isfinite(pathIndex) || !std::isfinite(pathWorld) ||
          indexDistance > options.maxStepTravelVelocityVoxels * (1 + 1e-12) ||
          pathIndex > options.maxTravelVelocityVoxels * (1 + 1e-12))
        throw std::invalid_argument("native point trajectory exceeds velocity-index L2 step/total travel cap");
      report.maximumStepTravelVelocityVoxels = std::max(report.maximumStepTravelVelocityVoxels, indexDistance);
      current = next;
    }
    report.maximumTravelVelocityVoxels = std::max(report.maximumTravelVelocityVoxels, pathIndex);
    report.maximumWorldTravel = std::max(report.maximumWorldTravel, pathWorld);
    return current;
  }
};

NativePointAdvectionResult advect(const std::filesystem::path& input,
    const std::filesystem::path& velocityPath, const std::filesystem::path& output,
    const NativePointAdvectionOptions& options, const std::string& pointGrid,
    const std::string& velocityGrid, const ProcessingOptions& processing,
    const PointAttributeOptions& limits) {
  PointMotionOptions motion;
  motion.groups = options.groups; motion.targetIndexToWorld = options.targetIndexToWorld;
  motion.maxPositionErrorVoxels = options.maxPositionErrorVoxels;
  motion.rejectActivityMerges = options.rejectActivityMerges;
  motion.maxWork = options.maxWork; motion.maxGrids = options.maxGrids;
  motion.maxMetadataBytes = options.maxMetadataBytes;
  auto source = loadSource(input, pointGrid, motion, processing, limits);
  auto velocity = loadVelocity(velocityPath, velocityGrid, source, options, processing);
  source.resident += velocity.resident; source.metadata += velocity.metadata;
  source.validationWork += velocity.validationWork;

  NativePointAdvectionResult result;
  result.velocityGridName = velocity.grid->getName();
  result.velocityValueType = velocity.grid->valueType();
  result.velocitySemantics = velocity.mac ? "local negative-face voxels_per_second mapped through J" :
      "stored WORLD XYZ physical velocity; vector tag not reapplied";
  result.maximumWorldSpeedBound = velocity.worldSpeedBound;
  const Affine pointFrame(source.grid->transform());
  const auto target = options.targetIndexToWorld ? targetMap(*options.targetIndexToWorld)
                                               : detail::CopyTransformExact(source.grid->transform());
  const Affine targetFrame(*target, bool(options.targetIndexToWorld));
  const double precision = std::min({pointFrame.minimum, targetFrame.minimum, velocity.frame->minimum}) * 1e-4;
  uint64_t selectedCount = 0;
  for (const auto* leaf : source.leaves) {
    Predicate selected(*leaf, options.groups);
    for (auto point = leaf->beginIndexAll(); point; ++point) selectedCount += selected(*point);
  }
  const bool integrate = options.timeStep != 0 && velocity.worldSpeedBound != 0 && selectedCount != 0;
  const double dt = options.timeStep / options.steps;
  if (integrate && (dt == 0 || dt * .5 == 0))
    throw std::invalid_argument("native point RK substep or midpoint time underflows Double");
  const double travel = velocity.indexSpeedBound * std::abs(options.timeStep);
  const double stepTravel = velocity.indexSpeedBound * std::abs(dt);
  if (integrate && velocity.indexSpeedBound != 0 && travel == 0)
    throw std::invalid_argument("native point nonzero velocity-index travel underflows Double");
  if (integrate && (!std::isfinite(travel) || travel > options.maxTravelVelocityVoxels ||
      !std::isfinite(stepTravel) || stepTravel > options.maxStepTravelVelocityVoxels))
    throw std::invalid_argument("native point velocity envelope exceeds velocity-index L2 step/total travel cap");

  const long double integrationWork = integrate ? selectedCount * static_cast<long double>(options.steps) *
      (options.integration == NativePointIntegration::RK2 ? 2 : 4) * 1024 : 0;
  source.validationWork += integrationWork;
  if (source.validationWork > options.maxWork)
    throw std::invalid_argument("native point integration/stored-source work exceeds maxWork before trajectories");
  budget(source.resident + source.schema.pointCount * 2048.0L + Fixed, processing,
         "native point trajectory/native planning workspace exceeds shared byte budget");
  Trajectories trajectories{velocity, options, result, precision, dt};
  std::function<V(const V&)> callback;
  if (integrate) callback = [&](const V& world) { return trajectories(world); };
  const MotionTopologyContext topology{true, velocity.leafSlots, velocity.active};
  auto planResult = plan(source, motion, processing, callback, integrate ? precision : 0, topology);
  if (result.velocitySampleCount) {
    result.executedSteps = options.steps;
    result.integrationSkipped = false;
  }
  result.diagnostics.emplace_back("Static Double RK trajectories precede one native PointMove; v/pscale and every non-P payload remain unchanged.");
  result.diagnostics.emplace_back("Travel limits and reported path lengths use the velocity grid index frame; codec error is not an integration accuracy certificate.");
  if (velocity.mac)
    result.diagnostics.emplace_back("Native MAC samples use positive half-axis offsets for stored negative faces, then J*u physical world conversion.");
  else
    result.diagnostics.emplace_back("Already-world vector components are physical XYZ, including normalized/absolute metadata tags without reinterpretation.");
  static_assert(std::is_nothrow_move_constructible_v<NativePointAdvectionResult>);
  (void)finishMovement(source, planResult, output, motion, processing,
      [&](PointMotionResult& report) { result.motion = std::move(report); });
  return result;
}
#endif
} // namespace

NativePointAdvectionResult WriteAdvectedNativePoints(
    const std::filesystem::path& points, const std::filesystem::path& velocity,
    const std::filesystem::path& output, const NativePointAdvectionOptions& options,
    const std::string& pointGrid, const std::string& velocityGrid,
    const ProcessingOptions& processing, const PointAttributeOptions& attributeLimits) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(processing, attributeLimits, true);
  if (!std::isfinite(options.timeStep) || std::abs(options.timeStep) > 3600 ||
      !options.steps || options.steps > 64 ||
      options.integration < NativePointIntegration::RK2 || options.integration > NativePointIntegration::RK4 ||
      options.velocityLayout < NativePointVelocityLayout::Auto || options.velocityLayout > NativePointVelocityLayout::NativeMacIndex ||
      !std::isfinite(options.maxStepTravelVelocityVoxels) || options.maxStepTravelVelocityVoxels <= 0 || options.maxStepTravelVelocityVoxels > 16 ||
      !std::isfinite(options.maxTravelVelocityVoxels) || options.maxTravelVelocityVoxels <= 0 || options.maxTravelVelocityVoxels > 256 ||
      !std::isfinite(options.maxPositionErrorVoxels) || options.maxPositionErrorVoxels <= 0 || options.maxPositionErrorVoxels > .25 ||
      !options.maxWork || options.maxWork > 10'000'000'000ULL || !options.maxGrids || options.maxGrids > 4096 || !options.maxMetadataBytes)
    throw std::invalid_argument("invalid native point advection time/steps/layout/travel/codec/work/catalog/metadata controls");
  if (options.targetIndexToWorld) matrix(*options.targetIndexToWorld);
  size_t terms = 0;
  for (const auto* names : {&options.groups.anyOf, &options.groups.allOf, &options.groups.noneOf})
    for (const auto& group : *names) {
      name(group);
      if (++terms > 3 * attributeLimits.maxGroups)
        throw std::invalid_argument("native point advection group predicate exceeds count cap");
    }
  if (output.empty() || std::filesystem::exists(output) ||
      !std::filesystem::is_directory(output.has_parent_path() ? output.parent_path() : std::filesystem::path(".")))
    throw std::invalid_argument("native point advection output must be new in an existing directory");
  initialize();
  tbb::task_arena arena(1);
  return arena.execute([&] {
    return advect(points, velocity, output, options, pointGrid, velocityGrid, processing, attributeLimits);
  });
#else
  (void)points; (void)velocity; (void)output; (void)options;
  (void)pointGrid; (void)velocityGrid; (void)processing; (void)attributeLimits;
  throw std::runtime_error("OpenVDB native point advection is unavailable; configure with -DENABLE_OPENVDB=ON.");
#endif
}
} // namespace container::geometry::vdb
