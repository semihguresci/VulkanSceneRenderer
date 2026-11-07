#include "Container/geometry/OpenVdbPoints.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/points/PointConversion.h>
#include <openvdb/points/PointCount.h>
#include "OpenVdbFileUtils.h"
#include <tbb/task_arena.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iomanip>
#include <mutex>
#include <streambuf>
#include <locale>
#include <set>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB points are unavailable in this build; configure with -DENABLE_OPENVDB=ON.");
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using Grid = openvdb::points::PointDataGrid;
using Tree = Grid::TreeType;
using Leaf = Tree::LeafNodeType;
using Upper = Tree::RootNodeType::ChildNodeType;
using Lower = Upper::ChildNodeType;
using IndexGrid = openvdb::tools::PointIndexGrid;
using IndexLeaf = IndexGrid::TreeType::LeafNodeType;
using IndexUpper = IndexGrid::TreeType::RootNodeType::ChildNodeType;
using IndexLower = IndexUpper::ChildNodeType;
using PositionArray = openvdb::points::TypedAttributeArray<openvdb::Vec3f, openvdb::points::NullCodec>;
using VelocityArray = openvdb::points::TypedAttributeArray<openvdb::Vec3d, openvdb::points::NullCodec>;
using RadiusArray = openvdb::points::TypedAttributeArray<double, openvdb::points::NullCodec>;
constexpr long double FixedWorkspace = 4.0L * 1024 * 1024;

void initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    openvdb::initialize();
    if (!VelocityArray::isRegistered()) VelocityArray::registerType();
    if (!RadiusArray::isRegistered()) RadiusArray::registerType();
  });
}

void validateOptions(const ProcessingOptions& p, const PointOptions& o, bool generation) {
  if (p.maxInputBytes == 0 || p.maxVoxelCount == 0 || o.maxPoints == 0)
    throw std::invalid_argument("point processing budgets must be positive");
  if (p.gridName.empty() || p.gridName.size() > 256)
    throw std::invalid_argument("point output grid name must contain 1..256 bytes");
  if (generation && (!std::isfinite(p.voxelSize) || p.voxelSize < 1.5e-5 || p.voxelSize > 1e6))
    throw std::invalid_argument("point generation voxel size must be in [1.5e-5,1e6] world units");
}

void validateOutput(const std::filesystem::path& output) {
  if (output.empty()) throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(output)) throw std::invalid_argument("output already exists; choose a new path");
  if (!output.parent_path().empty() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("output directory does not exist");
}

void validateRecord(const PointRecord& point) {
  for (size_t axis = 0; axis < 3; ++axis)
    if (!std::isfinite(point.position[axis]) || !std::isfinite(point.velocity[axis]))
      throw std::invalid_argument("point position and velocity must be finite");
  if (!std::isfinite(point.radius) || point.radius <= 0)
    throw std::invalid_argument("point radius must be finite and positive");
}

void validateCount(size_t count, const ProcessingOptions& p, const PointOptions& o) {
  if (count > o.maxPoints || count >= std::numeric_limits<openvdb::Index32>::max())
    throw std::invalid_argument("point count exceeds --max-points or OpenVDB's index range");
  if (static_cast<long double>(count) * sizeof(PointRecord) * 3 + FixedWorkspace > p.maxInputBytes)
    throw std::invalid_argument("decoded point records exceed the input-memory workspace budget");
}

long double metadataBytes(const openvdb::MetaMap& map) {
  long double count = sizeof(openvdb::MetaMap);
  for (auto it = map.beginMeta(); it != map.endMeta(); ++it)
    count += 2.0L * (it->first.size() + it->second->typeName().size() + it->second->size()) + 256;
  return count;
}

void validateCoordinate(const openvdb::Vec3d& index) {
  for (size_t axis = 0; axis < 3; ++axis)
    if (!std::isfinite(index[axis]) ||
        index[axis] < double(std::numeric_limits<int32_t>::min()) + 16 ||
        index[axis] > double(std::numeric_limits<int32_t>::max()) - 16)
      throw std::invalid_argument("point position exceeds safe OpenVDB integer coordinates");
}

void validatePrecision(const std::array<double, 3>& world, double minimumVoxel) {
  for (double value : world) {
    const double spacing = std::abs(std::nextafter(value, std::numeric_limits<double>::infinity()) - value);
    if (!std::isfinite(value) || !std::isfinite(spacing) || spacing > minimumVoxel * 1e-4)
      throw std::invalid_argument("point world coordinates have insufficient double precision for the source voxel size");
  }
}

uint64_t coarsenAndCount(std::vector<openvdb::Coord>& coordinates, uint32_t dimension) {
  for (auto& coordinate : coordinates) coordinate &= ~(dimension - 1);
  std::sort(coordinates.begin(), coordinates.end());
  coordinates.erase(std::unique(coordinates.begin(), coordinates.end()), coordinates.end());
  return coordinates.size();
}

void preflightWrite(std::span<const PointRecord> records, const ProcessingOptions& p, const PointOptions& o,
                    const openvdb::math::Transform& transform) {
  validateCount(records.size(), p, o);
  std::vector<openvdb::Coord> coordinates;
  coordinates.reserve(records.size());
  for (const auto& record : records) {
    validateRecord(record);
    const openvdb::Vec3d world(record.position[0], record.position[1], record.position[2]);
    const auto index = transform.worldToIndex(world);
    validateCoordinate(index);
    validatePrecision(record.position, p.voxelSize);
    coordinates.emplace_back(transform.worldToIndexCellCentered(world));
  }
  if (coarsenAndCount(coordinates, 1) > p.maxVoxelCount)
    throw std::invalid_argument("point occupied voxels exceed the voxel budget");
  const auto leaves = coarsenAndCount(coordinates, Leaf::DIM);
  const auto lowers = coarsenAndCount(coordinates, Lower::DIM);
  const auto uppers = coarsenAndCount(coordinates, Upper::DIM);
  const auto maxLeaves = p.maxVoxelCount / Leaf::NUM_VALUES + (p.maxVoxelCount % Leaf::NUM_VALUES != 0);
  if (leaves > maxLeaves) throw std::invalid_argument("point stored leaf topology exceeds the voxel budget");
  const long double pointNodes = leaves * (sizeof(Leaf) + Leaf::NUM_VALUES * sizeof(openvdb::Index32) + 2048.0L) +
      lowers * (sizeof(Lower) + 128.0L) + uppers * (sizeof(Upper) + 384.0L);
  const long double indexNodes = leaves * (sizeof(IndexLeaf) + IndexLeaf::NUM_VALUES * sizeof(openvdb::Index32) + 256.0L) +
      lowers * (sizeof(IndexLower) + 128.0L) + uppers * (sizeof(IndexUpper) + 384.0L);
  // Retained decoded records, coordinate/position/velocity/radius arrays,
  // point-index partitioner and attribute/compression scratch. Exact distinct
  // node counts keep dispersed point clouds sparse rather than filling a box.
  const long double workspace = 3 * pointNodes + 2 * indexNodes +
      static_cast<long double>(records.size()) * 384 + FixedWorkspace + 8 * p.gridName.size();
  if (workspace > p.maxInputBytes)
    throw std::invalid_argument("point creation topology/attribute workspace exceeds the input-memory budget");
}

double validateTransform(const openvdb::math::Transform& transform) {
  if (!transform.isLinear()) throw std::invalid_argument("point readback requires an affine transform");
  const auto origin = transform.indexToWorld(openvdb::Vec3d(0));
  const auto map=transform.baseMap();
  std::array<openvdb::Vec3d, 3> axes{
      map->applyJacobian(openvdb::Vec3d(1,0,0)),
      map->applyJacobian(openvdb::Vec3d(0,1,0)),
      map->applyJacobian(openvdb::Vec3d(0,0,1))};
  double minimum = std::numeric_limits<double>::infinity();
  for (size_t axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(origin[axis])) throw std::invalid_argument("point transform has non-finite translation");
    const double length = std::hypot(axes[axis].x(), axes[axis].y(), axes[axis].z());
    if (!std::isfinite(length) || length < 1e-6 || length > 1e6)
      throw std::invalid_argument("point transform voxel axes must be in [1e-6,1e6] world units");
    minimum = std::min(minimum, length);
    axes[axis] /= length;
  }
  if (std::abs(axes[0].dot(axes[1].cross(axes[2]))) < 1e-8)
    throw std::invalid_argument("point transform is singular or insufficiently conditioned");
  return minimum;
}

struct LoadedPoints {
  std::vector<PointRecord> records;
  PointResult result;
};

LoadedPoints readPoints(const std::filesystem::path& input, const std::string& selection,
                        const ProcessingOptions& p, const PointOptions& o) {
  if (!std::filesystem::is_regular_file(input)) throw std::invalid_argument("point input is not a regular file");
  const auto encoded = std::filesystem::file_size(input);
  if (encoded > p.maxInputBytes) throw std::invalid_argument("point input exceeds the file-size budget");
  detail::RequireSeekableFile(input);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded point readback requires OpenVDB delayed loading; enable delayed-loading support and unset OPENVDB_DISABLE_DELAYED_LOAD");
  openvdb::io::File file(input.string());
#ifdef OPENVDB_USE_DELAYED_LOADING
  if (encoded >= std::numeric_limits<openvdb::Index64>::max())
    throw std::invalid_argument("point input exceeds the delayed-copy size range");
  file.setCopyMaxBytes(static_cast<openvdb::Index64>(encoded) + 1);
#endif
  file.open(true);
  {
    std::set<std::string> descriptors;
    long double catalogBytes = encoded;
    for (auto it = file.beginName(); it != file.endName(); ++it) {
      const auto name = it.gridName();
      catalogBytes += name.size() + 128.0L;
      if (catalogBytes > p.maxInputBytes)
        throw std::invalid_argument("point descriptor catalog exceeds the input-memory budget");
      if (!descriptors.insert(name).second)
        throw std::invalid_argument("point input has colliding grid descriptors; rename colliding grids before reading");
    }
  }
  std::string descriptor;
  bool found = false;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto metadata = file.readGridMetadata(it.gridName());
    if (!GridSelectionMatches(selection, it.gridName(), metadata->getName()) || !metadata->isType<Grid>()) continue;
    if (found) throw std::invalid_argument("point grid name is ambiguous; use descriptor:<exact descriptor name>");
    const auto memoryHint = metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
    const auto voxelHint = metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!memoryHint || memoryHint->value() < 0 || !voxelHint || voxelHint->value() < 0)
      throw std::invalid_argument("point readback requires recorded nonnegative file_mem_bytes and file_voxel_count statistics; rewrite with OpenVDB grid statistics enabled");
    if (uint64_t(voxelHint->value()) > p.maxVoxelCount)
      throw std::invalid_argument("point topology metadata exceeds the voxel budget");
    if (const auto hint = metadata->getMetadata<openvdb::Int64Metadata>("container:point_count");
        hint && (hint->value() < 0 || uint64_t(hint->value()) > o.maxPoints))
      throw std::invalid_argument("point count metadata exceeds --max-points");
    if (static_cast<long double>(encoded) + memoryHint->value() + metadataBytes(*metadata) > p.maxInputBytes)
      throw std::invalid_argument("point topology metadata exceeds the input-memory budget");
    descriptor = it.gridName();
    found = true;
    if (selection.empty()) break;
  }
  if (!found) throw std::invalid_argument("no matching PointDataGrid found in input");
  auto grid = openvdb::gridPtrCast<Grid>(file.readGrid(descriptor));
  file.close();
  const double dx = validateTransform(grid->transform());
  const auto maxLeaves = p.maxVoxelCount / Leaf::NUM_VALUES + (p.maxVoxelCount % Leaf::NUM_VALUES != 0);
  if (grid->tree().leafCount() > maxLeaves || grid->activeVoxelCount() > p.maxVoxelCount)
    throw std::invalid_argument("point stored topology exceeds the voxel budget");
  if (grid->tree().hasActiveTiles())
    throw std::invalid_argument("PointDataGrid cannot store points in active tiles without leaf attributes");
  uint64_t count = 0;
  long double decoded = grid->memUsage() + metadataBytes(*grid);
  for (auto leaf = grid->tree().cbeginLeaf(); leaf; ++leaf) {
    const auto& attributes = leaf->attributeSet();
    const auto& layout = attributes.descriptor();
    if (layout.map().size() != 3 || !layout.groupMap().empty() ||
        !leaf->hasAttribute("P") || !leaf->hasAttribute("v") || !leaf->hasAttribute("pscale"))
      throw std::invalid_argument("point readback requires exactly P, v and pscale attributes without groups; missing or extra attributes cannot be exported losslessly");
    const auto& position = leaf->constAttributeArray("P");
    const auto& velocity = leaf->constAttributeArray("v");
    const auto& radius = leaf->constAttributeArray("pscale");
    if (!position.isType<PositionArray>() || !velocity.isType<VelocityArray>() || !radius.isType<RadiusArray>() ||
        position.stride() != 1 || velocity.stride() != 1 || radius.stride() != 1)
      throw std::invalid_argument("incompatible point attribute layout: require P Vec3f, v Vec3d, pscale double, NullCodec, stride one");
    leaf->validateOffsets();
    const auto leafCount = leaf->pointCount();
    if (leafCount > o.maxPoints - std::min<uint64_t>(count, o.maxPoints))
      throw std::invalid_argument("point count exceeds --max-points");
    count += leafCount;
    if (position.size() != leafCount || velocity.size() != leafCount || radius.size() != leafCount)
      throw std::invalid_argument("point attribute lengths disagree with voxel point offsets");
    decoded += position.memUsageIfLoaded() + velocity.memUsageIfLoaded() + radius.memUsageIfLoaded() +
               metadataBytes(layout.getMetadata());
  }
  validateCount(static_cast<size_t>(count), p, o);
  if (static_cast<long double>(encoded) + 2 * decoded + count * (3.0L * sizeof(PointRecord)) + FixedWorkspace > p.maxInputBytes)
    throw std::invalid_argument("point readback attribute/record workspace exceeds the input-memory budget");
  LoadedPoints output;
  output.result = {grid->getName(), count, grid->voxelSize().x()};
  output.records.reserve(static_cast<size_t>(count));
  const auto map=grid->transform().baseMap();
  const auto origin=map->applyMap(openvdb::Vec3d(0));
  const std::array<openvdb::Vec3d,3> basis{
      map->applyJacobian(openvdb::Vec3d(1,0,0)),map->applyJacobian(openvdb::Vec3d(0,1,0)),
      map->applyJacobian(openvdb::Vec3d(0,0,1))};
  for (auto leaf = grid->tree().cbeginLeaf(); leaf; ++leaf) {
    openvdb::points::AttributeHandle<openvdb::Vec3f, openvdb::points::NullCodec> positions(leaf->constAttributeArray("P"));
    openvdb::points::AttributeHandle<openvdb::Vec3d, openvdb::points::NullCodec> velocities(leaf->constAttributeArray("v"));
    openvdb::points::AttributeHandle<double, openvdb::points::NullCodec> radii(leaf->constAttributeArray("pscale"));
    for (auto point = leaf->beginIndexAll(); point; ++point) {
      const auto local = positions.get(*point);
      for (size_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(local[axis]) || std::abs(local[axis]) > .500001f)
          throw std::invalid_argument("point P must contain finite voxel-local offsets in [-0.5,0.5]");
      const auto index = point.getCoord().asVec3d() + openvdb::Vec3d(local);
      validateCoordinate(index);
      // Do not add P to a remote integer coordinate before transformation:
      // that addition can discard local Float offsets despite a small final
      // world position. Sum affine products with their FMA residuals instead.
      openvdb::Vec3d world;
      for (size_t axis=0;axis<3;++axis) {
        double sum=origin[axis],correction=0;
        const auto add=[&](double term) {
          const double next=sum+term;
          correction+=std::abs(sum)>=std::abs(term) ? (sum-next)+term : (term-next)+sum;
          sum=next;
        };
        for (size_t sourceAxis=0;sourceAxis<3;++sourceAxis) {
          const double coefficient=basis[sourceAxis][axis];
          for (double coordinate:{double(point.getCoord()[sourceAxis]),double(local[sourceAxis])}) {
            const double product=coefficient*coordinate;
            add(product); add(std::fma(coefficient,coordinate,-product));
          }
        }
        world[axis]=sum+correction;
      }
      const auto velocity = velocities.get(*point);
      PointRecord record{{world.x(), world.y(), world.z()}, {velocity.x(), velocity.y(), velocity.z()}, radii.get(*point)};
      validateRecord(record);
      validatePrecision(record.position, dx);
      output.records.push_back(record);
    }
  }
  if (output.records.size() != count) throw std::invalid_argument("point iterator count disagrees with stored offsets");
  std::sort(output.records.begin(), output.records.end(), [](const auto& a, const auto& b) {
    return std::tie(a.position, a.velocity, a.radius) < std::tie(b.position, b.velocity, b.radius);
  });
  return output;
}

PointResult writePoints(std::span<const PointRecord> records, const std::filesystem::path& output,
                        const ProcessingOptions& p, const PointOptions& o) {
  const auto transform = openvdb::math::Transform::createLinearTransform(p.voxelSize);
  preflightWrite(records, p, o, *transform);
  Grid::Ptr grid;
  if (records.empty()) {
    grid = Grid::create();
    grid->setTransform(transform);
  } else {
    std::vector<openvdb::Vec3d> positions, velocities;
    std::vector<double> radii;
    positions.reserve(records.size()); velocities.reserve(records.size()); radii.reserve(records.size());
    for (const auto& record : records) {
      positions.emplace_back(record.position[0], record.position[1], record.position[2]);
      velocities.emplace_back(record.velocity[0], record.velocity[1], record.velocity[2]);
      radii.push_back(record.radius);
    }
    const openvdb::points::PointAttributeVector<openvdb::Vec3d> positionList(positions), velocityList(velocities);
    const openvdb::points::PointAttributeVector<double> radiusList(radii);
    const auto indices = openvdb::tools::createPointIndexGrid<IndexGrid>(positionList, *transform);
    grid = openvdb::points::createPointDataGrid<openvdb::points::NullCodec, Grid>(*indices, positionList, *transform);
    openvdb::points::appendAttribute<openvdb::Vec3d>(grid->tree(), "v");
    openvdb::points::appendAttribute<double>(grid->tree(), "pscale", 1.0);
    openvdb::points::populateAttribute(grid->tree(), indices->tree(), "v", velocityList);
    openvdb::points::populateAttribute(grid->tree(), indices->tree(), "pscale", radiusList);
  }
  grid->setName(p.gridName);
  grid->insertMeta("container:point_count", openvdb::Int64Metadata(static_cast<int64_t>(records.size())));
  const auto maxLeaves = p.maxVoxelCount / Leaf::NUM_VALUES + (p.maxVoxelCount % Leaf::NUM_VALUES != 0);
  if (grid->tree().leafCount() > maxLeaves || grid->activeVoxelCount() > p.maxVoxelCount ||
      grid->memUsage() + metadataBytes(*grid) > p.maxInputBytes)
    throw std::invalid_argument("created point attributes exceed the input-memory budget");
  detail::WriteFileExclusive(output, openvdb::GridPtrVec{grid}, p.maxInputBytes);
  return {grid->getName(), records.size(), p.voxelSize};
}

class BoundedJsonInput : public std::streambuf {
 public:
  BoundedJsonInput(std::ifstream& source, uintmax_t remaining) : source_(source), remaining_(remaining) {}
  uintmax_t remaining() const { return remaining_; }
  bool tokenExceeded() const { return tokenExceeded_; }
 protected:
  int_type underflow() override {
    if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
    if (remaining_ == 0) return traits_type::eof();
    source_.read(buffer_, static_cast<std::streamsize>(std::min<uintmax_t>(remaining_, sizeof(buffer_))));
    const auto received = source_.gcount();
    remaining_ -= static_cast<uintmax_t>(received);
    if (received == 0) return traits_type::eof();
    for (std::streamsize index = 0; index < received; ++index) {
      const char value = buffer_[index];
      if (inString_) {
        ++tokenBytes_;
        if (!escaped_ && value == '"') inString_ = false;
        if (!escaped_ && value == '\\') escaped_ = true;
        else escaped_ = false;
      } else if (value == '"') {
        inString_ = true; escaped_ = false; tokenBytes_ = 1;
      } else if (value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
                 value == '[' || value == ']' || value == '{' || value == '}' || value == ':' || value == ',') {
        tokenBytes_ = 0;
      } else ++tokenBytes_;
      if (tokenBytes_ > 128) { tokenExceeded_ = true; return traits_type::eof(); }
    }
    setg(buffer_, buffer_, buffer_ + received);
    return traits_type::to_int_type(*gptr());
  }
 private:
  std::ifstream& source_;
  uintmax_t remaining_;
  size_t tokenBytes_{0};
  bool inString_{false}, escaped_{false}, tokenExceeded_{false};
  char buffer_[4096];
};

struct PointJson : nlohmann::json_sax<nlohmann::json> {
  enum class State { Start, Array, Object, Position, Velocity, Done } state{State::Start};
  enum class Field { None, Position, Velocity, Radius } field{Field::None};
  const ProcessingOptions& processing;
  const PointOptions& options;
  long double encodedReserve;
  std::vector<PointRecord> records;
  PointRecord current;
  size_t coordinates{0};
  bool position{false}, velocity{false}, radius{false};
  std::string error;
  PointJson(const ProcessingOptions& p, const PointOptions& o, uintmax_t encoded)
      : processing(p), options(o), encodedReserve(3.0L * encoded) {}
  bool fail(const char* message) { error = message; return false; }
  bool null() override { return fail("point values must be numeric"); }
  bool boolean(bool) override { return fail("point values must be numeric"); }
  bool string(string_t&) override { return fail("point values must be numeric"); }
  bool binary(binary_t&) override { return fail("binary point values are unsupported"); }
  bool number_integer(number_integer_t value) override { return number(static_cast<double>(value)); }
  bool number_unsigned(number_unsigned_t value) override { return number(static_cast<double>(value)); }
  bool number_float(number_float_t value, const string_t&) override { return number(value); }
  bool number(double value) {
    if (!std::isfinite(value)) return fail("point values must be finite");
    if ((state == State::Position || state == State::Velocity) && coordinates < 3) {
      auto& destination = state == State::Position ? current.position : current.velocity;
      destination[coordinates++] = value;
      return true;
    }
    if (state == State::Object && field == Field::Radius) {
      current.radius = value; radius = true; field = Field::None; return true;
    }
    return fail("unexpected numeric value in point JSON");
  }
  bool start_object(std::size_t) override {
    if (state != State::Array) return fail("point JSON must be an array of objects");
    if (records.size() >= options.maxPoints || records.size() + 1 >= std::numeric_limits<openvdb::Index32>::max())
      return fail("point count exceeds --max-points or OpenVDB's index range");
    if (encodedReserve + (static_cast<long double>(records.size()) + 1) * sizeof(PointRecord) * 3 > processing.maxInputBytes)
      return fail("decoded point JSON exceeds the input-memory budget");
    current = {}; coordinates = 0; position = velocity = radius = false;
    field = Field::None; state = State::Object; return true;
  }
  bool key(string_t& name) override {
    if (state != State::Object || field != Field::None) return fail("invalid point object");
    if (name == "position" && !position) field = Field::Position;
    else if (name == "velocity" && !velocity) field = Field::Velocity;
    else if (name == "radius" && !radius) field = Field::Radius;
    else return fail("unknown or duplicate point field");
    return true;
  }
  bool end_object() override {
    if (state != State::Object || field != Field::None || !position) return fail("each point requires a three-number position");
    records.push_back(current); state = State::Array; return true;
  }
  bool start_array(std::size_t) override {
    if (state == State::Start) { state = State::Array; return true; }
    if (state == State::Object && (field == Field::Position || field == Field::Velocity)) {
      state = field == Field::Position ? State::Position : State::Velocity;
      coordinates = 0; return true;
    }
    return fail("unexpected array in point JSON");
  }
  bool end_array() override {
    if ((state == State::Position || state == State::Velocity) && coordinates == 3) {
      if (state == State::Position) position = true;
      else velocity = true;
      field = Field::None; state = State::Object; return true;
    }
    if (state == State::Array) { state = State::Done; return true; }
    return fail("point position and velocity must contain exactly three numbers");
  }
  bool parse_error(std::size_t location, const std::string&, const nlohmann::detail::exception&) override {
    error = "invalid point JSON at byte " + std::to_string(location); return false;
  }
};
#endif
}  // namespace

PointResult WritePoints(std::span<const PointRecord> points, const std::filesystem::path& output,
                        const ProcessingOptions& processing, const PointOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(processing, options, true);
  validateOutput(output);
  initialize();
  tbb::task_arena arena(1);
  return arena.execute([&] { return writePoints(points, output, processing, options); });
#else
  (void)points; (void)output; (void)processing; (void)options; unavailable();
#endif
}

PointResult WritePointsFile(const std::filesystem::path& input, const std::filesystem::path& output,
                            const ProcessingOptions& processing, const PointOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(processing, options, true);
  validateOutput(output);
  if (!std::filesystem::is_regular_file(input)) throw std::invalid_argument("point JSON input is not a regular file");
  const auto encoded = std::filesystem::file_size(input);
  if (3.0L * encoded > processing.maxInputBytes)
    throw std::invalid_argument("point JSON input exceeds the file-size or parsing workspace budget");
  std::ifstream source(input, std::ios::binary);
  if (!source) throw std::invalid_argument("could not open point JSON input");
  BoundedJsonInput buffer(source, encoded);
  std::istream stream(&buffer);
  PointJson parser(processing, options, encoded);
  const bool parsed = nlohmann::json::sax_parse(stream, &parser);
  if (buffer.tokenExceeded()) throw std::invalid_argument("point JSON token exceeds the 128-byte parsing limit");
  if (!parsed) throw std::invalid_argument(parser.error.empty() ? "invalid point JSON" : parser.error);
  if (source.bad()) throw std::runtime_error("failed while reading point JSON input");
  if (buffer.remaining() != 0 || source.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("point JSON input changed size while being read");
  return WritePoints(parser.records, output, processing, options);
#else
  (void)input; (void)output; (void)processing; (void)options; unavailable();
#endif
}

std::vector<PointRecord> ReadPoints(const std::filesystem::path& input, const std::string& inputGrid,
                                    const ProcessingOptions& processing, const PointOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(processing, options, false);
  initialize();
  tbb::task_arena arena(1);
  return arena.execute([&] { return readPoints(input, inputGrid, processing, options).records; });
#else
  (void)input; (void)inputGrid; (void)processing; (void)options; unavailable();
#endif
}

PointResult ExportPoints(const std::filesystem::path& input, const std::filesystem::path& output,
                          const std::string& inputGrid, const ProcessingOptions& processing,
                          const PointOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(processing, options, false);
  validateOutput(output);
  initialize();
  tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto points = readPoints(input, inputGrid, processing, options);
    // Seven max_digits10 numbers, punctuation and a sign/exponent allowance.
    if (points.records.size() * 256.0L + 3 > processing.maxInputBytes)
      throw std::invalid_argument("point JSON output estimate exceeds the file-size budget");
    bool created = false;
    try {
      std::ofstream stream(output, std::ios::out | std::ios::noreplace | std::ios::binary);
      if (!stream) throw std::runtime_error("could not exclusively create point JSON output");
      created = true;
      stream.imbue(std::locale::classic());
      stream << std::setprecision(std::numeric_limits<double>::max_digits10) << '[';
      for (size_t index = 0; index < points.records.size(); ++index) {
        const auto& point = points.records[index];
        stream << (index ? ",\n" : "\n") << "{\"position\":[" << point.position[0] << ',' << point.position[1] << ',' << point.position[2]
               << "],\"velocity\":[" << point.velocity[0] << ',' << point.velocity[1] << ',' << point.velocity[2]
               << "],\"radius\":" << point.radius << '}';
      }
      stream << "\n]\n";
      stream.close();
      if (!stream) throw std::runtime_error("failed while writing point JSON output");
    } catch (...) {
      if (created) { std::error_code ignored; std::filesystem::remove(output, ignored); }
      throw;
    }
    return points.result;
  });
#else
  (void)input; (void)output; (void)inputGrid; (void)processing; (void)options; unavailable();
#endif
}

}  // namespace container::geometry::vdb
