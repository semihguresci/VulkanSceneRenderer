#include "Container/geometry/OpenVdbDenseInterchange.h"
#include "Container/geometry/OpenVdbGridSelection.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <mutex>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Dense.h>
#include <set>
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error(
      "OpenVDB dense interchange is unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr long double Fixed = 4.0L * 1024 * 1024;
constexpr uint32_t version = 1;
constexpr std::array<char, 8> magic{'C', 'V', 'D', 'B', 'A', 'R', 'R', '\0'};
void budget(long double n, const ProcessingOptions &p, const char *message) {
  if (!std::isfinite(n) || n > p.maxInputBytes)
    throw std::invalid_argument(message);
}
uint64_t add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX - a)
    throw std::invalid_argument("dense interchange size overflow");
  return a + b;
}
void options(const ProcessingOptions &p) {
  if (!p.maxVoxelCount || !p.maxInputBytes)
    throw std::invalid_argument("dense interchange budgets must be positive");
}
void outputPath(const std::filesystem::path &path) {
  if (path.empty())
    throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(path))
    throw std::invalid_argument("output already exists; choose a new path");
  if (path.has_parent_path() && !std::filesystem::is_directory(path.parent_path()))
    throw std::invalid_argument("output directory does not exist");
}
uint64_t encoded(const std::filesystem::path &path, const ProcessingOptions &p) {
  if (!std::filesystem::is_regular_file(path))
    throw std::invalid_argument("dense interchange input is not a regular file");
  const auto size = std::filesystem::file_size(path);
  budget(size + Fixed, p, "dense interchange encoded input exceeds memory budget");
  return size;
}
// OpenVDB File accepts a narrow filename. Copy through native filesystem paths
// to a private filename instead of narrowing the caller's Unicode input path.
struct InputSnapshot {
  std::filesystem::path directory, file;
  InputSnapshot(const std::filesystem::path &input, uint64_t bytes) {
    static std::atomic_uint64_t serial{};
    const auto parent = std::filesystem::temp_directory_path();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      const auto candidate = parent / (".container-dense-input-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
          "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(candidate)) {
        directory = candidate;
        break;
      }
    }
    if (directory.empty())
      throw std::runtime_error("cannot reserve dense input snapshot directory");
    file = directory / "input.vdb";
    try {
      std::ifstream source(input, std::ios::binary);
      std::ofstream target(file, std::ios::binary | std::ios::out | std::ios::noreplace);
      if (!source || !target)
        throw std::runtime_error("cannot open dense input snapshot");
      std::array<char, 65536> buffer{};
      while (bytes) {
        const auto amount = std::streamsize(std::min<uint64_t>(bytes, buffer.size()));
        source.read(buffer.data(), amount);
        if (source.gcount() != amount)
          throw std::runtime_error("dense input changed size while snapshotting");
        target.write(buffer.data(), amount);
        if (!target)
          throw std::runtime_error("dense input snapshot write failed");
        bytes -= uint64_t(amount);
      }
      target.close();
      if (!target || source.bad() || source.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("dense input changed size or snapshot failed");
    } catch (...) {
      cleanup();
      throw;
    }
  }
  void cleanup() noexcept {
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    std::filesystem::remove(directory, ignored);
  }
  ~InputSnapshot() { cleanup(); }
  InputSnapshot(const InputSnapshot &) = delete;
  InputSnapshot &operator=(const InputSnapshot &) = delete;
};
void initialize() {
  static std::once_flag flag;
  std::call_once(flag, [] { openvdb::initialize(); });
}
long double metadataBytes(const openvdb::MetaMap &map) {
  long double n = sizeof(openvdb::MetaMap);
  for (auto it = map.beginMeta(); it != map.endMeta(); ++it)
    n += 2.0L * (it->first.size() + it->second->typeName().size() + it->second->size()) + 256;
  return n;
}
uint64_t cells(const DenseRegion &r, const ProcessingOptions &p) {
  uint64_t n = 1;
  for (int k = 0; k < 3; ++k) {
    if (!r.dimensions[k] || r.dimensions[k] > INT32_MAX || n > p.maxVoxelCount / r.dimensions[k])
      throw std::invalid_argument("dense region dimensions exceed cell budget");
    n *= r.dimensions[k];
    if (int64_t(r.origin[k]) < INT32_MIN + 8ll ||
        int64_t(r.origin[k]) + r.dimensions[k] - 1 > INT32_MAX - 8ll)
      throw std::invalid_argument("dense region exceeds safe integer coordinates");
  }
  return n;
}
openvdb::CoordBBox box(const DenseRegion &r) {
  openvdb::Coord a(r.origin.data()), b;
  for (int k = 0; k < 3; ++k)
    b[k] = int32_t(int64_t(r.origin[k]) + r.dimensions[k] - 1);
  return {a, b};
}
size_t scalarWidth(size_t type) { return type == 0 || type == 2 ? 4 : 8; }
size_t components(size_t type) { return type < 2 ? 1 : 3; }
uint64_t payloadBytes(uint64_t count, size_t type) {
  const uint64_t width = scalarWidth(type) * components(type) + 1;
  if (count > UINT64_MAX / width)
    throw std::invalid_argument("dense payload length overflow");
  return count * width;
}
template <class T> struct Native {
  using Type = T;
  using Grid =
      std::conditional_t<std::is_same_v<T, float>, openvdb::FloatGrid, openvdb::DoubleGrid>;
  static Type to(T v) { return v; }
  static T from(Type v) { return v; }
};
template <> struct Native<std::array<float, 3>> {
  using Type = openvdb::Vec3f;
  using Grid = openvdb::Vec3SGrid;
  static Type to(const std::array<float, 3> &v) { return Type(v.data()); }
  static auto from(const Type &v) { return std::array<float, 3>{v.x(), v.y(), v.z()}; }
};
template <> struct Native<std::array<double, 3>> {
  using Type = openvdb::Vec3d;
  using Grid = openvdb::Vec3DGrid;
  static Type to(const std::array<double, 3> &v) { return Type(v.data()); }
  static auto from(const Type &v) { return std::array<double, 3>{v.x(), v.y(), v.z()}; }
};
template <class T> void finite(const T &v) {
  if constexpr (std::is_floating_point_v<T>) {
    if (!std::isfinite(v))
      throw std::invalid_argument("dense values/background must be finite");
  } else
    for (auto x : v)
      finite(x);
}
template <class T> bool equalBits(const T &a, const T &b) {
  if constexpr (std::is_same_v<T, float>)
    return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
  else if constexpr (std::is_same_v<T, double>)
    return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
  else
    return equalBits(a[0], b[0]) && equalBits(a[1], b[1]) && equalBits(a[2], b[2]);
}
void strings(const DenseArrayVolume &v) {
  if (v.gridName.empty() || v.gridName.size() > 256 || v.gridName.find('\0') != std::string::npos)
    throw std::invalid_argument("dense grid name must contain 1..256 bytes without NUL");
  if (v.diagnostics.omittedMetadata.size() > 4096)
    throw std::invalid_argument("dense metadata diagnostic exceeds 4096 entries");
  uint64_t bytes = 0;
  for (const auto &n : v.diagnostics.omittedMetadata) {
    bytes = add(bytes, n.size());
    if (bytes > 1024 * 1024)
      throw std::invalid_argument("dense metadata diagnostic exceeds 1MiB");
  }
}
openvdb::math::Transform::Ptr affine(const DenseArrayVolume &v) {
  openvdb::math::Mat4d matrix = openvdb::math::Mat4d::identity();
  std::array<openvdb::Vec3d, 3> axes;
  double minSize = INFINITY;
  for (int c = 0; c < 3; ++c) {
    for (int r = 0; r < 3; ++r) {
      const double value = v.linear[3 * r + c];
      if (!std::isfinite(value))
        throw std::invalid_argument("dense affine coefficients must be finite");
      matrix[c][r] = value;
    }
    axes[c] = {v.linear[c], v.linear[3 + c], v.linear[6 + c]};
    const double size = std::hypot(axes[c].x(), axes[c].y(), axes[c].z());
    if (!std::isfinite(size) || size < 1e-6 || size > 1e6)
      throw std::invalid_argument("dense affine axis lengths must span [1e-6,1e6]");
    minSize = std::min(minSize, size);
  }
  const double determinant = axes[0].dot(axes[1].cross(axes[2]));
  const double scale = axes[0].length() * axes[1].length() * axes[2].length();
  if (!std::isfinite(determinant) || std::abs(determinant) < 3e-15 ||
      std::abs(determinant / scale) < 1e-12)
    throw std::invalid_argument("dense affine is singular/ill-conditioned or below OpenVDB "
                                "inverse-map scale; rescale world units");
  for (int k = 0; k < 3; ++k) {
    if (!std::isfinite(v.translation[k]))
      throw std::invalid_argument("dense translation must be finite");
    matrix[3][k] = v.translation[k];
  }
  auto result = detail::MakeAffineTransformExact(matrix);
  const auto map = result->baseMap();
  const auto origin = map->applyMap(openvdb::Vec3d(0));
  for (unsigned corner = 0; corner < 8; ++corner) {
    openvdb::Vec3d index;
    for (int k = 0; k < 3; ++k)
      index[k] =
          double(v.region.origin[k]) + ((corner & (1u << k)) ? v.region.dimensions[k] - 1 : 0);
    const auto world = map->applyMap(index);
    const auto restored = map->applyInverseJacobian(world - origin);
    for (int k = 0; k < 3; ++k) {
      const auto spacing = [](double x) {
        return std::max(std::nextafter(x, INFINITY) - x, x - std::nextafter(x, -INFINITY));
      };
      // A small final coordinate can hide large product/translation rounding.
      // Charge every operand and partial sum, not only the final world ULP.
      double error = spacing(v.translation[k]), partial = 0;
      for (int c = 0; c < 3; ++c) {
        const double term = v.linear[3 * k + c] * index[c];
        partial += term;
        error += spacing(term) + spacing(partial);
      }
      error += spacing(partial + v.translation[k]);
      const double gap = std::max(std::nextafter(world[k], INFINITY) - world[k],
                                  world[k] - std::nextafter(world[k], -INFINITY));
      if (!std::isfinite(world[k]) || !std::isfinite(restored[k]) || gap > minSize * 1e-4 ||
          !std::isfinite(error) || error > minSize * 1e-4 ||
          std::abs(restored[k] - index[k]) > 1e-4)
        throw std::invalid_argument(
            "dense affine loses world/index precision; recenter the region");
    }
  }
  return result;
}
uint64_t validate(const DenseArrayVolume &v, const ProcessingOptions &p) {
  options(p);
  const auto n = cells(v.region, p);
  if (v.values.index() != v.background.index() || v.active.size() != n)
    throw std::invalid_argument("dense value/background types or active-mask length do not match");
  if (uint32_t(v.gridClass) > 3 || uint32_t(v.vectorType) > 4)
    throw std::invalid_argument("dense class/vector enum out of range");
  strings(v);
  budget(3.0L * payloadBytes(n, v.values.index()) + Fixed, p,
         "dense array workspace exceeds memory budget");
  for (uint8_t a : v.active)
    if (a > 1)
      throw std::invalid_argument("dense active mask must contain only0/1");
  std::visit(
      [&](const auto &values) {
        using T = typename std::decay_t<decltype(values)>::value_type;
        if (values.size() != n)
          throw std::invalid_argument("dense value count does not match dimensions");
        finite(std::get<T>(v.background));
        for (const auto &x : values)
          finite(x);
      },
      v.values);
  (void)affine(v);
  return n;
}
openvdb::GridClass nativeClass(DenseGridClass c) {
  switch (c) {
  case DenseGridClass::Unknown:
    return openvdb::GRID_UNKNOWN;
  case DenseGridClass::LevelSet:
    return openvdb::GRID_LEVEL_SET;
  case DenseGridClass::FogVolume:
    return openvdb::GRID_FOG_VOLUME;
  case DenseGridClass::Staggered:
    return openvdb::GRID_STAGGERED;
  }
  throw std::invalid_argument("invalid dense class");
}
DenseGridClass publicClass(openvdb::GridClass c) {
  switch (c) {
  case openvdb::GRID_UNKNOWN:
    return DenseGridClass::Unknown;
  case openvdb::GRID_LEVEL_SET:
    return DenseGridClass::LevelSet;
  case openvdb::GRID_FOG_VOLUME:
    return DenseGridClass::FogVolume;
  case openvdb::GRID_STAGGERED:
    return DenseGridClass::Staggered;
  default:
    throw std::invalid_argument("unsupported dense grid class");
  }
}
openvdb::VecType nativeVector(DenseVectorType t) {
  switch (t) {
  case DenseVectorType::Invariant:
    return openvdb::VEC_INVARIANT;
  case DenseVectorType::Covariant:
    return openvdb::VEC_COVARIANT;
  case DenseVectorType::CovariantNormalize:
    return openvdb::VEC_COVARIANT_NORMALIZE;
  case DenseVectorType::ContravariantRelative:
    return openvdb::VEC_CONTRAVARIANT_RELATIVE;
  case DenseVectorType::ContravariantAbsolute:
    return openvdb::VEC_CONTRAVARIANT_ABSOLUTE;
  }
  throw std::invalid_argument("invalid dense vector type");
}
DenseVectorType publicVector(openvdb::VecType t) {
  switch (t) {
  case openvdb::VEC_INVARIANT:
    return DenseVectorType::Invariant;
  case openvdb::VEC_COVARIANT:
    return DenseVectorType::Covariant;
  case openvdb::VEC_COVARIANT_NORMALIZE:
    return DenseVectorType::CovariantNormalize;
  case openvdb::VEC_CONTRAVARIANT_RELATIVE:
    return DenseVectorType::ContravariantRelative;
  case openvdb::VEC_CONTRAVARIANT_ABSOLUTE:
    return DenseVectorType::ContravariantAbsolute;
  default:
    throw std::invalid_argument("unsupported dense vector semantics");
  }
}
bool structural(const std::string &key) {
  for (const auto *s :
       {openvdb::GridBase::META_GRID_NAME, openvdb::GridBase::META_GRID_CLASS,
        openvdb::GridBase::META_IS_LOCAL_SPACE, openvdb::GridBase::META_VECTOR_TYPE,
        openvdb::GridBase::META_FILE_BBOX_MIN, openvdb::GridBase::META_FILE_BBOX_MAX,
        openvdb::GridBase::META_FILE_COMPRESSION, openvdb::GridBase::META_FILE_MEM_BYTES,
        openvdb::GridBase::META_FILE_VOXEL_COUNT, openvdb::GridBase::META_FILE_DELAYED_LOAD})
    if (key == s)
      return true;
  return false;
}
openvdb::GridBase::Ptr readGrid(const std::filesystem::path &path, const std::string &selection,
                                const ProcessingOptions &p, uint64_t bytes, uint64_t denseCount) {
  detail::RequireSeekableFile(path);
  openvdb::io::File file(path.string());
  file.open(false);
  std::set<std::string> names;
  long double retained = bytes + Fixed;
  uint64_t denseBytes = 0;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto name = it.gridName();
    retained += name.size() * 2.0L + 128;
    budget(retained, p, "dense grid descriptor catalog exceeds memory budget");
    if (!names.insert(name).second)
      throw std::invalid_argument("dense input has colliding descriptors; rename grids first");
  }
  std::string name;
  bool found = false;
  for (auto it = file.beginName(); it != file.endName(); ++it) {
    const auto meta = file.readGridMetadata(it.gridName());
    if (!(meta->isType<openvdb::FloatGrid>() || meta->isType<openvdb::DoubleGrid>() ||
          meta->isType<openvdb::Vec3SGrid>() || meta->isType<openvdb::Vec3DGrid>()) ||
        !GridSelectionMatches(selection, it.gridName(), meta->getName()))
      continue;
    if (found)
      throw std::invalid_argument("dense input grid is ambiguous; use descriptor:<exact name>");
    const size_t type = meta->isType<openvdb::FloatGrid>()    ? 0
                        : meta->isType<openvdb::DoubleGrid>() ? 1
                        : meta->isType<openvdb::Vec3SGrid>()  ? 2
                                                              : 3;
    denseBytes = payloadBytes(denseCount, type);
    const auto count = meta->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_VOXEL_COUNT),
               memory = meta->getMetadata<openvdb::Int64Metadata>(
                   openvdb::GridBase::META_FILE_MEM_BYTES);
    if (!count || !memory || count->value() < 0 || memory->value() < 0 ||
        uint64_t(count->value()) > p.maxVoxelCount)
      throw std::invalid_argument(
          "dense input requires valid recorded topology/memory within budgets");
    budget(retained + 3.0L * denseBytes + 3.0L * memory->value() + 4 * metadataBytes(*meta), p,
           "dense source recorded workspace exceeds memory budget");
    name = it.gridName();
    found = true;
    if (selection.empty())
      break;
  }
  if (!found)
    throw std::invalid_argument("no matching Float/Double/Vec3S/Vec3D grid for dense interchange");
  auto grid = file.readGrid(name);
  file.close();
  const auto maxLeaves = p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0);
  if (grid->activeVoxelCount() > p.maxVoxelCount || grid->baseTree().leafCount() > maxLeaves)
    throw std::invalid_argument("dense input stored topology exceeds budget");
  budget(retained + 3.0L * denseBytes + 3.0L * grid->memUsage() + 4 * metadataBytes(*grid), p,
         "dense loaded source workspace exceeds memory budget");
  return grid;
}
template <class T>
DenseArrayVolume toArray(const typename Native<T>::Grid &grid, const DenseRegion &region,
                         uint64_t count) {
  using N = typename Native<T>::Type;
  DenseArrayVolume out;
  out.region = region;
  out.gridName = grid.getName();
  out.gridClass = publicClass(grid.getGridClass());
  out.vectorType = publicVector(grid.getVectorType());
  out.worldSpace = grid.isInWorldSpace();
  out.background = Native<T>::from(grid.background());
  finite(std::get<T>(out.background));
  const auto map = grid.transform().baseMap();
  if (!grid.transform().isLinear())
    throw std::invalid_argument("dense interchange requires affine grid transforms");
  const auto origin = map->applyMap(openvdb::Vec3d(0));
  for (int k = 0; k < 3; ++k)
    out.translation[k] = origin[k];
  for (int c = 0; c < 3; ++c) {
    openvdb::Vec3d axis(0);
    axis[c] = 1;
    const auto v = map->applyJacobian(axis);
    for (int r = 0; r < 3; ++r)
      out.linear[3 * r + c] = v[r];
  }
  const auto bounds = box(region);
  (void)affine(out);
  std::vector<N> native(static_cast<size_t>(count));
  openvdb::tools::Dense<N, openvdb::tools::LayoutXYZ> dense(bounds, native.data());
  openvdb::tools::copyToDense(grid, dense, true);
  std::vector<T> values;
  values.reserve(size_t(count));
  out.active.resize(size_t(count));
  uint64_t active = 0;
  for (uint64_t i = 0; i < count; ++i) {
    const auto c = dense.offsetToLocalCoord(size_t(i)) + bounds.min();
    auto value = Native<T>::from(native[size_t(i)]);
    finite(value);
    values.push_back(value);
    const bool on = grid.tree().isValueOn(c);
    out.active[size_t(i)] = uint8_t(on);
    active += on;
  }
  out.values = std::move(values);
  if (active > grid.activeVoxelCount())
    throw std::runtime_error("dense active count disagrees with source topology");
  out.diagnostics.activeVoxelsOutside = grid.activeVoxelCount() - active;
  for (auto value = grid.cbeginValueAll(); value; ++value) {
    const auto v = Native<T>::from(*value);
    finite(v);
    if (!equalBits(v, std::get<T>(out.background))) {
      const auto stored = value.getBoundingBox();
      if (!bounds.isInside(stored.min()) || !bounds.isInside(stored.max()))
        out.diagnostics.storedNonBackgroundOutside = true;
    }
  }
  for (auto it = grid.beginMeta(); it != grid.endMeta(); ++it)
    if (!structural(it->first))
      out.diagnostics.omittedMetadata.push_back(it->first);
  strings(out);
  return out;
}
int64_t block(int64_t x, int64_t d) { return x >= 0 ? x / d : (x + 1) / d - 1; }
long double nodes(const DenseRegion &r, int64_t d) {
  long double n = 1;
  for (int k = 0; k < 3; ++k)
    n *= block(int64_t(r.origin[k]) + r.dimensions[k] - 1, d) - block(r.origin[k], d) + 1;
  return n;
}
template <class T>
openvdb::GridBase::Ptr toGrid(const DenseArrayVolume &v, const ProcessingOptions &p,
                              uint64_t count) {
  using G = typename Native<T>::Grid;
  using N = typename Native<T>::Type;
  using Tree = typename G::TreeType;
  using Leaf = typename Tree::LeafNodeType;
  using Upper = typename Tree::RootNodeType::ChildNodeType;
  using Lower = typename Upper::ChildNodeType;
  const long double leafCount = nodes(v.region, 8);
  if (leafCount > p.maxVoxelCount / 512 + (p.maxVoxelCount % 512 != 0))
    throw std::invalid_argument("dense import prospective leaf slots exceed voxel budget");
  const long double tree = leafCount * (sizeof(Leaf) + 512.0L * sizeof(N) + 256) +
                           nodes(v.region, 128) * (sizeof(Lower) + 512.0L) +
                           nodes(v.region, 4096) * (sizeof(Upper) + 512.0L);
  budget(3 * tree + 3.0L * payloadBytes(count, v.values.index()) + Fixed, p,
         "dense import full-region tree/array/writer workspace exceeds memory budget");
  const auto &values = std::get<std::vector<T>>(v.values);
  std::vector<N> native;
  native.reserve(size_t(count));
  for (const auto &x : values)
    native.push_back(Native<T>::to(x));
  const auto bounds = box(v.region);
  openvdb::tools::Dense<N, openvdb::tools::LayoutXYZ> dense(bounds, native.data());
  auto grid = G::create(Native<T>::to(std::get<T>(v.background)));
  openvdb::tools::copyFromDense(dense, *grid, N(0), true);
  // Numeric background equality treats +0 and -0 as equal. Materialize the
  // already-budgeted leaves so exact inactive values cannot disappear through
  // setValueOff's background shortcut.
  for (int64_t z = block(bounds.min().z(), 8); z <= block(bounds.max().z(), 8); ++z)
    for (int64_t y = block(bounds.min().y(), 8); y <= block(bounds.max().y(), 8); ++y)
      for (int64_t x = block(bounds.min().x(), 8); x <= block(bounds.max().x(), 8); ++x)
        grid->tree().touchLeaf(openvdb::Coord(int32_t(x * 8), int32_t(y * 8), int32_t(z * 8)));
  auto accessor = grid->getAccessor();
  for (uint64_t i = 0; i < count; ++i) {
    const auto c = dense.offsetToLocalCoord(size_t(i)) + bounds.min();
    if (v.active[size_t(i)])
      accessor.setValueOn(c, native[size_t(i)]);
    else
      accessor.setValueOff(c, native[size_t(i)]);
  }
  grid->setTransform(affine(v));
  grid->setName(v.gridName);
  grid->setGridClass(nativeClass(v.gridClass));
  grid->setVectorType(nativeVector(v.vectorType));
  grid->setIsInWorldSpace(v.worldSpace);
  grid->insertMeta("container:dense_array_order", openvdb::StringMetadata("x-fastest"));
  grid->insertMeta("container:dense_source_active_outside",
                   openvdb::StringMetadata(std::to_string(v.diagnostics.activeVoxelsOutside)));
  grid->insertMeta("container:dense_source_values_outside",
                   openvdb::BoolMetadata(v.diagnostics.storedNonBackgroundOutside));
  grid->insertMeta("container:dense_omitted_metadata_count",
                   openvdb::Int64Metadata(int64_t(v.diagnostics.omittedMetadata.size())));
  budget(3.0L * grid->memUsage() + 3.0L * payloadBytes(count, v.values.index()) +
             4 * metadataBytes(*grid) + Fixed,
         p, "dense imported output exceeds memory budget");
  return grid;
}
class BinaryWriter {
  std::ostream &out;
  std::array<char, 65536> buffer{};
  size_t used = 0;

public:
  explicit BinaryWriter(std::ostream &s) : out(s) {}
  void byte(uint8_t v) {
    buffer[used++] = char(v);
    if (used == buffer.size())
      flush();
  }
  void integer(uint64_t v, size_t width) {
    for (size_t i = 0; i < width; ++i)
      byte(uint8_t(v >> (8 * i)));
  }
  template <class T> void number(T value) {
    if constexpr (std::is_same_v<T, float>)
      integer(std::bit_cast<uint32_t>(value), 4);
    else if constexpr (std::is_same_v<T, double>)
      integer(std::bit_cast<uint64_t>(value), 8);
    else
      for (auto x : value)
        number(x);
  }
  void text(const std::string &s) {
    integer(s.size(), 4);
    for (unsigned char c : s)
      byte(c);
  }
  void flush() {
    if (used) {
      out.write(buffer.data(), std::streamsize(used));
      used = 0;
    }
  }
};
class BinaryReader {
  std::istream &in;
  uint64_t remainingBytes;

public:
  BinaryReader(std::istream &s, uint64_t n) : in(s), remainingBytes(n) {}
  uint64_t remaining() const { return remainingBytes; }
  uint8_t byte() {
    if (!remainingBytes)
      throw std::invalid_argument("dense array is truncated");
    const auto c = in.get();
    if (c == std::char_traits<char>::eof())
      throw std::invalid_argument("dense array changed size or is truncated");
    --remainingBytes;
    return uint8_t(c);
  }
  uint64_t integer(size_t width) {
    uint64_t v = 0;
    for (size_t i = 0; i < width; ++i)
      v |= uint64_t(byte()) << (8 * i);
    return v;
  }
  template <class T> T number() {
    if constexpr (std::is_same_v<T, float>)
      return std::bit_cast<float>(uint32_t(integer(4)));
    else if constexpr (std::is_same_v<T, double>)
      return std::bit_cast<double>(integer(8));
    else {
      T v;
      for (auto &x : v)
        x = number<typename T::value_type>();
      return v;
    }
  }
  std::string text(uint64_t maximum) {
    const auto n = integer(4);
    if (n > maximum || n > remainingBytes)
      throw std::invalid_argument("dense array string length exceeds limit");
    std::string s(size_t(n), '\0');
    for (char &c : s)
      c = char(byte());
    return s;
  }
};
uint64_t fileLength(const DenseArrayVolume &v, uint64_t count) {
  uint64_t length = 200 + scalarWidth(v.values.index()) * components(v.values.index());
  length = add(length, v.gridName.size());
  for (const auto &n : v.diagnostics.omittedMetadata)
    length = add(length, add(4, n.size()));
  return add(length, payloadBytes(count, v.values.index()));
}
void serialize(BinaryWriter &w, const DenseArrayVolume &v, uint64_t count) {
  for (char c : magic)
    w.byte(uint8_t(c));
  w.integer(version, 4);
  w.integer(v.values.index(), 4);
  w.integer(uint32_t(v.gridClass), 4);
  w.integer(uint32_t(v.vectorType), 4);
  w.integer(v.worldSpace ? 1 : 0, 4);
  w.integer(0, 4);
  for (int32_t x : v.region.origin)
    w.integer(std::bit_cast<uint32_t>(x), 4);
  for (uint32_t x : v.region.dimensions)
    w.integer(x, 4);
  for (double x : v.linear)
    w.number(x);
  for (double x : v.translation)
    w.number(x);
  std::visit([&](const auto &x) { w.number(x); }, v.background);
  w.integer(v.gridName.size(), 4);
  w.integer(v.diagnostics.omittedMetadata.size(), 4);
  w.integer(v.diagnostics.activeVoxelsOutside, 8);
  w.integer(v.diagnostics.storedNonBackgroundOutside ? 1 : 0, 4);
  w.integer(0, 4);
  w.integer(count, 8);
  w.integer(payloadBytes(count, v.values.index()), 8);
  w.integer(fileLength(v, count), 8);
  for (unsigned char c : v.gridName)
    w.byte(c);
  for (const auto &key : v.diagnostics.omittedMetadata)
    w.text(key);
  std::visit(
      [&](const auto &values) {
        for (const auto &x : values)
          w.number(x);
      },
      v.values);
  for (uint8_t x : v.active)
    w.byte(x);
  w.flush();
}
DenseArrayVolume deserialize(BinaryReader &r, uint64_t size, const ProcessingOptions &p) {
  for (char c : magic)
    if (r.byte() != uint8_t(c))
      throw std::invalid_argument("invalid dense array magic");
  if (r.integer(4) != version)
    throw std::invalid_argument("unsupported dense array version");
  const uint64_t type = r.integer(4);
  if (type > 3)
    throw std::invalid_argument("invalid dense array value type");
  DenseArrayVolume v;
  v.gridClass = DenseGridClass(r.integer(4));
  v.vectorType = DenseVectorType(r.integer(4));
  if (uint32_t(v.gridClass) > 3 || uint32_t(v.vectorType) > 4)
    throw std::invalid_argument("invalid dense array class/vector enum");
  const auto world = r.integer(4);
  if (world > 1 || r.integer(4) != 0)
    throw std::invalid_argument("invalid dense array flags");
  v.worldSpace = world != 0;
  for (auto &x : v.region.origin)
    x = std::bit_cast<int32_t>(uint32_t(r.integer(4)));
  for (auto &x : v.region.dimensions)
    x = uint32_t(r.integer(4));
  const auto count = cells(v.region, p);
  const auto bytes = payloadBytes(count, size_t(type));
  budget(size + 3.0L * bytes + Fixed, p, "dense array readback workspace exceeds memory budget");
  for (auto &x : v.linear)
    x = r.number<double>();
  for (auto &x : v.translation)
    x = r.number<double>();
  switch (type) {
  case 0:
    v.background = r.number<float>();
    break;
  case 1:
    v.background = r.number<double>();
    break;
  case 2:
    v.background = r.number<std::array<float, 3>>();
    break;
  case 3:
    v.background = r.number<std::array<double, 3>>();
    break;
  }
  const auto nameLength = r.integer(4), omitted = r.integer(4);
  v.diagnostics.activeVoxelsOutside = r.integer(8);
  const auto outside = r.integer(4);
  if (outside > 1 || r.integer(4) != 0 || r.integer(8) != count || r.integer(8) != bytes ||
      r.integer(8) != size)
    throw std::invalid_argument("dense array lengths/counts/flags disagree");
  v.diagnostics.storedNonBackgroundOutside = outside != 0;
  if (nameLength < 1 || nameLength > 256 || omitted > 4096 || nameLength > r.remaining())
    throw std::invalid_argument("dense array name/metadata counts out of range");
  v.gridName.clear();
  for (uint64_t i = 0; i < nameLength; ++i)
    v.gridName.push_back(char(r.byte()));
  uint64_t stringBytes = 0;
  v.diagnostics.omittedMetadata.reserve(size_t(omitted));
  for (uint64_t i = 0; i < omitted; ++i) {
    auto key = r.text(1024 * 1024 - stringBytes);
    stringBytes += key.size();
    v.diagnostics.omittedMetadata.push_back(std::move(key));
  }
  if (r.remaining() != bytes)
    throw std::invalid_argument("dense array payload/trailing length disagrees");
  strings(v);
  std::visit([](const auto &x) { finite(x); }, v.background);
  (void)affine(v);
  const auto readValues = [&]<class T>() {
    std::vector<T> values;
    values.reserve(size_t(count));
    for (uint64_t i = 0; i < count; ++i) {
      auto x = r.number<T>();
      finite(x);
      values.push_back(x);
    }
    v.values = std::move(values);
  };
  switch (type) {
  case 0:
    readValues.operator()<float>();
    break;
  case 1:
    readValues.operator()<double>();
    break;
  case 2:
    readValues.operator()<std::array<float, 3>>();
    break;
  case 3:
    readValues.operator()<std::array<double, 3>>();
    break;
  }
  v.active.resize(size_t(count));
  for (auto &x : v.active)
    x = r.byte();
  (void)validate(v, p);
  return v;
}
#endif
} // namespace

DenseArrayVolume ReadDenseVdb(const std::filesystem::path &input, const DenseRegion &region,
                              const std::string &selected, const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(p);
  initialize();
  tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto count = cells(region, p);
    const auto bytes = encoded(input, p);
    InputSnapshot snapshot(input, bytes);
    auto grid = readGrid(snapshot.file, selected, p, bytes, count);
    DenseArrayVolume out;
    if (grid->isType<openvdb::FloatGrid>())
      out = toArray<float>(*openvdb::gridConstPtrCast<openvdb::FloatGrid>(grid), region, count);
    else if (grid->isType<openvdb::DoubleGrid>())
      out = toArray<double>(*openvdb::gridConstPtrCast<openvdb::DoubleGrid>(grid), region, count);
    else if (grid->isType<openvdb::Vec3SGrid>())
      out = toArray<std::array<float, 3>>(*openvdb::gridConstPtrCast<openvdb::Vec3SGrid>(grid),
                                          region, count);
    else
      out = toArray<std::array<double, 3>>(*openvdb::gridConstPtrCast<openvdb::Vec3DGrid>(grid),
                                           region, count);
    (void)validate(out, p);
    return out;
  });
#else
  (void)input;
  (void)region;
  (void)selected;
  (void)p;
  unavailable();
#endif
}
ProcessingResult WriteDenseVdb(const std::filesystem::path &output, const DenseArrayVolume &volume,
                               const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  outputPath(output);
  const auto count = validate(volume, p);
  initialize();
  tbb::task_arena arena(1);
  return arena.execute([&] {
    auto grid = std::visit(
        [&](const auto &values) {
          using T = typename std::decay_t<decltype(values)>::value_type;
          return toGrid<T>(volume, p, count);
        },
        volume.values);
    // Active-mask compression compares +0 and -0 as equal and can discard
    // inactive sign bits. ZIP alone retains the explicit native value buffer.
    detail::WriteFileExclusive(output, {grid}, p.maxInputBytes, openvdb::io::COMPRESS_ZIP);
    return ProcessingResult{grid->getName(),
                            openvdb::GridBase::gridClassToString(grid->getGridClass()),
                            grid->activeVoxelCount(), grid->voxelSize().x()};
  });
#else
  (void)output;
  (void)volume;
  (void)p;
  unavailable();
#endif
}
DenseArrayVolume ReadDenseArrayFile(const std::filesystem::path &input,
                                    const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(p);
  const auto bytes = encoded(input, p);
  std::ifstream stream(input, std::ios::binary);
  if (!stream)
    throw std::invalid_argument("cannot open dense array input");
  BinaryReader reader(stream, bytes);
  auto volume = deserialize(reader, bytes, p);
  if (reader.remaining() != 0 || stream.peek() != std::char_traits<char>::eof())
    throw std::invalid_argument("dense array has trailing bytes or changed size");
  return volume;
#else
  (void)input;
  (void)p;
  unavailable();
#endif
}
void WriteDenseArrayFile(const std::filesystem::path &output, const DenseArrayVolume &volume,
                         const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  outputPath(output);
  const auto count = validate(volume, p);
  const auto size = fileLength(volume, count);
  budget(size + 3.0L * payloadBytes(count, volume.values.index()) + Fixed, p,
         "dense array encoded/output workspace exceeds memory budget");
  std::ofstream stream;
  bool created = false;
  try {
    stream.open(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    created = stream.is_open();
    if (!created)
      throw std::runtime_error("cannot exclusively create dense array output");
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    BinaryWriter writer(stream);
    serialize(writer, volume, count);
    stream.close();
    if (std::filesystem::file_size(output) != size)
      throw std::runtime_error("dense array written length disagrees with schema");
  } catch (...) {
    stream.exceptions(std::ios::goodbit);
    if (stream.is_open())
      stream.close();
    if (created) {
      std::error_code ignored;
      std::filesystem::remove(output, ignored);
    }
    throw;
  }
#else
  (void)output;
  (void)volume;
  (void)p;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
