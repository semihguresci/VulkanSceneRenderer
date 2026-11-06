#include "Container/geometry/OpenVdbPointFieldSampling.h"
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbPointAttributesInternal.h"
#include <tbb/task_arena.h>
#include <variant>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB native point field sampling is "
                           "unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using namespace point_attribute_detail;
using V = openvdb::Vec3d;
struct Work {
  uint64_t used = 0, maximum;
  void add(uint64_t n = 1) {
    if (n > maximum - used)
      throw std::invalid_argument("point field sampling exceeds maxWork");
    used += n;
  }
};
double gap(double x) {
  return std::max(std::abs(std::nextafter(x, INFINITY) - x),
                  std::abs(x - std::nextafter(x, -INFINITY)));
}
struct Sum {
  double value = 0, correction = 0;
  void add(double x) {
    const double next = value + x;
    correction += std::abs(value) >= std::abs(x) ? (value - next) + x
                                                 : (x - next) + value;
    value = next;
  }
  void product(double a, double b, bool rejectUnderflow = false) {
    const double x = a * b;
    if (rejectUnderflow && a != 0 && b != 0 && x == 0)
      throw std::invalid_argument("MAC world-vector transform term underflows "
                                  "Double; rescale the field");
    add(x);
    add(std::fma(a, b, -x));
  }
  double get() const { return value + correction; }
};
struct Affine {
  V origin;
  std::array<V, 3> basis, inverse;
  double minimum = INFINITY;
  explicit Affine(const openvdb::math::Transform &t) {
    if (!t.isLinear())
      throw std::invalid_argument(
          "native point field sampling requires affine transforms");
    origin = t.indexToWorld(V(0));
    const auto map = t.baseMap();
    auto normalized = basis;
    for (size_t k = 0; k < 3; ++k) {
      V e(0);
      e[k] = 1;
      basis[k] = map->applyJacobian(e);
      inverse[k] = map->applyInverseJacobian(e);
      const double length = std::hypot(basis[k][0], basis[k][1], basis[k][2]);
      if (!std::isfinite(origin[k]) || !std::isfinite(length) ||
          length < 1e-6 || length > 1e6)
        throw std::invalid_argument(
            "sampling affine axes must be finite in [1e-6,1e6]");
      minimum = std::min(minimum, length);
      normalized[k] = basis[k] / length;
      for (size_t r = 0; r < 3; ++r)
        if (!std::isfinite(inverse[k][r]))
          throw std::invalid_argument(
              "sampling inverse transform must be finite");
    }
    if (std::abs(normalized[0].dot(normalized[1].cross(normalized[2]))) < 1e-8)
      throw std::invalid_argument(
          "sampling affine transform is insufficiently conditioned");
  }
  V world(const openvdb::Coord &c, const openvdb::Vec3f &p,
          double accuracy) const {
    for (size_t k = 0; k < 3; ++k)
      if (!std::isfinite(p[k]) || std::abs(p[k]) > .500001f)
        throw std::invalid_argument("selected point P must be finite "
                                    "voxel-local offsets within 0.500001");
    V result;
    for (size_t r = 0; r < 3; ++r) {
      Sum sum;
      sum.add(origin[r]);
      for (size_t k = 0; k < 3; ++k) {
        sum.product(basis[k][r], double(c[k]));
        sum.product(basis[k][r], p[k]);
      }
      result[r] = sum.get();
      if (!std::isfinite(result[r]) || gap(result[r]) > accuracy * 1e-4)
        throw std::invalid_argument(
            "point world position loses voxel precision; recenter source");
    }
    return result;
  }
  V index(const V &world, double accuracy) const {
    V result;
    for (size_t r = 0; r < 3; ++r) {
      Sum sum;
      for (size_t k = 0; k < 3; ++k) {
        sum.product(inverse[k][r], world[k]);
        sum.product(inverse[k][r], -origin[k]);
      }
      result[r] = sum.get();
      if (!std::isfinite(result[r]) || result[r] < double(INT32_MIN) + 4 ||
          result[r] > double(INT32_MAX) - 4)
        throw std::invalid_argument(
            "field sampling coordinate exceeds safe OpenVDB stencil range");
    }
    double resolution = 0;
    for (size_t k = 0; k < 3; ++k)
      resolution +=
          gap(result[k]) * std::hypot(basis[k][0], basis[k][1], basis[k][2]);
    if (!std::isfinite(resolution) || resolution > accuracy * 1e-4)
      throw std::invalid_argument(
          "field index precision is insufficient; recenter field transform");
    return result;
  }
  V vector(const V &x) const {
    V result;
    for (size_t r = 0; r < 3; ++r) {
      Sum sum;
      for (size_t k = 0; k < 3; ++k)
        sum.product(basis[k][r], x[k], true);
      result[r] = sum.get();
      if (!std::isfinite(result[r]))
        throw std::invalid_argument("MAC world-vector conversion overflows");
    }
    return result;
  }
};
struct Predicate {
  std::array<std::vector<points::GroupHandle>, 3> h;
  Predicate(const Leaf &leaf, const PointGroupSelection &s) {
    size_t i = 0;
    for (const auto *names : {&s.anyOf, &s.allOf, &s.noneOf}) {
      for (const auto &n : *names)
        h[i].push_back(leaf.groupHandle(n));
      ++i;
    }
  }
  bool operator()(openvdb::Index n) const {
    bool any = h[0].empty();
    for (const auto &g : h[0])
      any |= g.get(n);
    if (!any)
      return false;
    for (const auto &g : h[1])
      if (!g.get(n))
        return false;
    for (const auto &g : h[2])
      if (g.get(n))
        return false;
    return true;
  }
};
long double selectionReserve(const PointGroupSelection &s,
                             const PointAttributeOptions &limits,
                             const PointAttributeSchema *schema = nullptr) {
  size_t count = 0;
  long double bytes = 4096;
  for (const auto *list : {&s.anyOf, &s.allOf, &s.noneOf})
    for (const auto &n : *list) {
      name(n);
      if (++count > 3 * limits.maxGroups)
        throw std::invalid_argument(
            "point sampling selection exceeds group limits");
      if (schema && std::find(schema->groups.begin(), schema->groups.end(),
                              n) == schema->groups.end())
        throw std::invalid_argument("point sampling group not found: " + n);
      bytes += 8.0L * n.size() + 1024;
    }
  return bytes;
}
bool supported(const openvdb::GridBase &g) {
  return g.isType<openvdb::BoolGrid>() || g.isType<openvdb::Int32Grid>() ||
         g.isType<openvdb::Int64Grid>() || g.isType<openvdb::FloatGrid>() ||
         g.isType<openvdb::DoubleGrid>() || g.isType<openvdb::Vec3SGrid>() ||
         g.isType<openvdb::Vec3DGrid>();
}
struct Field {
  std::unique_ptr<Snapshot> snapshot;
  openvdb::GridBase::Ptr grid;
  long double resident = 0, metadata = 0;
  bool mac = false;
};
struct PointCatalog {
  long double bytes = 0, metadata = 0;
  openvdb::MetaMap::Ptr fileMetadata;
};
PointCatalog pointCatalog(const Snapshot &snapshot,
                          const PointFieldSamplingOptions &o,
                          const ProcessingOptions &p, Work &work) {
  detail::RequireSeekableFile(snapshot.file);
  openvdb::io::File file(snapshot.file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  PointCatalog catalog;
  catalog.fileMetadata = file.getMetadata();
  catalog.metadata = metadataBytes(*catalog.fileMetadata);
  if (catalog.metadata > o.maxMetadataBytes)
    throw std::invalid_argument("point file metadata exceeds maxMetadataBytes");
  catalog.bytes = Fixed + snapshot.encoded + 8 * catalog.metadata;
  budget(catalog.bytes, p, "point file metadata exceeds memory budget");
  std::set<std::string> names;
  for (auto i = file.beginName(); i != file.endName(); ++i) {
    work.add();
    if (names.size() >= o.maxGrids)
      throw std::invalid_argument(
          "point sampling source catalog exceeds maxGrids");
    const auto n = i.gridName();
    const long double nameBytes = 4.0L * n.size() + 256;
    catalog.bytes += nameBytes;
    catalog.metadata += nameBytes;
    if (catalog.metadata > o.maxMetadataBytes)
      throw std::invalid_argument(
          "point file metadata/catalog exceeds maxMetadataBytes");
    budget(catalog.bytes, p,
           "point sampling source catalog exceeds memory budget");
    if (!names.insert(n).second)
      throw std::invalid_argument(
          "point source has colliding descriptors; rename grids first");
  }
  return catalog;
}
Field loadField(const std::filesystem::path &path, const std::string &selection,
                const PointFieldSamplingOptions &o, const ProcessingOptions &p,
                Work &work) {
  Field f;
  f.snapshot = std::make_unique<Snapshot>(path, p);
  detail::RequireSeekableFile(f.snapshot->file);
  openvdb::io::File file(f.snapshot->file.string());
  file.setCopyMaxBytes(0);
  file.open(true);
  const auto fileMetadata = file.getMetadata();
  const long double fileMetadataBytes = metadataBytes(*fileMetadata);
  if (fileMetadataBytes > o.maxMetadataBytes)
    throw std::invalid_argument("field file metadata exceeds maxMetadataBytes");
  std::set<std::string> names;
  long double catalog = Fixed + f.snapshot->encoded + 8 * fileMetadataBytes;
  long double catalogMetadata = fileMetadataBytes;
  budget(catalog, p, "field file metadata exceeds memory budget");
  for (auto i = file.beginName(); i != file.endName(); ++i) {
    work.add();
    if (names.size() >= o.maxGrids)
      throw std::invalid_argument(
          "point sampling field catalog exceeds maxGrids");
    const auto n = i.gridName();
    catalog += 4.0L * n.size() + 256;
    catalogMetadata += 4.0L * n.size() + 256;
    if (catalogMetadata > o.maxMetadataBytes)
      throw std::invalid_argument(
          "field file metadata/catalog exceeds maxMetadataBytes");
    budget(catalog, p, "point sampling field catalog exceeds memory budget");
    if (!names.insert(n).second)
      throw std::invalid_argument(
          "point sampling field has colliding descriptors; rename grids first");
  }
  std::string chosen;
  for (auto i = file.beginName(); i != file.endName(); ++i) {
    work.add();
    const auto meta = file.readGridMetadata(i.gridName());
    const auto bytes = metadataBytes(*meta);
    budget(catalog + 4 * bytes, p,
           "point sampling field metadata exceeds budget");
    if (catalogMetadata + bytes > o.maxMetadataBytes)
      throw std::invalid_argument("field metadata exceeds maxMetadataBytes");
    if (!supported(*meta) ||
        !GridSelectionMatches(selection, i.gridName(), meta->getName()))
      continue;
    if (!chosen.empty())
      throw std::invalid_argument("point sampling field selector is ambiguous; "
                                  "use descriptor:<exact name>");
    const auto mem = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_MEM_BYTES);
    const auto voxels = meta->getMetadata<openvdb::Int64Metadata>(
        openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!mem || !voxels || mem->value() < 0 || voxels->value() < 0 ||
        uint64_t(voxels->value()) > p.maxVoxelCount)
      throw std::invalid_argument("field requires valid recorded memory/voxel "
                                  "statistics within budget");
    budget(catalog + 4.0L * mem->value() + 8 * bytes, p,
           "recorded field tree/writer reserves exceed shared memory budget");
    chosen = i.gridName();
    if (selection.empty())
      break;
  }
  if (chosen.empty())
    throw std::invalid_argument(
        "no matching supported scalar/world-vector field found");
  f.grid = file.readGrid(chosen);
  f.metadata = catalogMetadata + metadataBytes(*f.grid);
  const auto fullMemory = [&]<class G>() -> uintmax_t {
    return openvdb::tools::memUsageIfLoaded(
        openvdb::gridPtrCast<G>(f.grid)->tree(), false);
  };
  uintmax_t decoded = 0;
  if (f.grid->isType<openvdb::BoolGrid>())
    decoded = fullMemory.operator()<openvdb::BoolGrid>();
  else if (f.grid->isType<openvdb::Int32Grid>())
    decoded = fullMemory.operator()<openvdb::Int32Grid>();
  else if (f.grid->isType<openvdb::Int64Grid>())
    decoded = fullMemory.operator()<openvdb::Int64Grid>();
  else if (f.grid->isType<openvdb::FloatGrid>())
    decoded = fullMemory.operator()<openvdb::FloatGrid>();
  else if (f.grid->isType<openvdb::DoubleGrid>())
    decoded = fullMemory.operator()<openvdb::DoubleGrid>();
  else if (f.grid->isType<openvdb::Vec3SGrid>())
    decoded = fullMemory.operator()<openvdb::Vec3SGrid>();
  else
    decoded = fullMemory.operator()<openvdb::Vec3DGrid>();
  f.resident = catalog + 4.0L * decoded + 8 * f.metadata;
  budget(f.resident, p, "fully decoded sampling field exceeds memory budget");
  f.grid->readNonresidentBuffers();
  if (f.grid->memUsage() > decoded)
    throw std::runtime_error(
        "decoded sampling field exceeded preflight estimate");
  file.close();
  const bool vector = f.grid->isType<openvdb::Vec3SGrid>() ||
                      f.grid->isType<openvdb::Vec3DGrid>();
  if (vector) {
    f.mac = f.grid->getGridClass() == openvdb::GRID_STAGGERED;
    if (f.mac) {
      const auto layout = f.grid->getMetadata<openvdb::StringMetadata>(
                     "container:mac_layout"),
                 units = f.grid->getMetadata<openvdb::StringMetadata>(
                     "container:velocity_units");
      if (f.grid->isInWorldSpace() ||
          f.grid->getVectorType() != openvdb::VEC_CONTRAVARIANT_RELATIVE ||
          !layout || layout->value() != "negative_faces" || !units ||
          units->value() != "voxels_per_second")
        throw std::invalid_argument(
            "native MAC sampling requires negative_faces, voxels_per_second, "
            "index-space contravariant-relative tags");
      if (o.interpolation == PointFieldInterpolation::Quadratic)
        throw std::invalid_argument("native MAC quadratic interpolation is "
                                    "unsupported; choose nearest or linear");
    } else if (!f.grid->isInWorldSpace() ||
               f.grid->getGridClass() != openvdb::GRID_UNKNOWN ||
               int(f.grid->getVectorType()) < 0 ||
               int(f.grid->getVectorType()) >= openvdb::NUM_VEC_TYPES)
      throw std::invalid_argument(
          "collocated vector sampling requires class UNKNOWN, world-space "
          "components and a valid vector tag");
  }
  return f;
}
template <class T> void finite(const T &x, bool mac = false) {
  if constexpr (std::is_floating_point_v<T>) {
    if (!std::isfinite(x))
      throw std::invalid_argument(
          "stored sampling field values must be finite");
  } else if constexpr (!std::is_arithmetic_v<T>)
    for (size_t k = 0; k < 3; ++k) {
      if (!std::isfinite(x[k]) || (mac && std::abs(double(x[k])) > 1e24))
        throw std::invalid_argument("stored vector field values must be finite "
                                    "(native MAC components <=1e24)");
    }
}
template <class G>
void validateField(const G &g, const ProcessingOptions &p, Work &work,
                   bool mac) {
  const uint64_t leaves = p.maxVoxelCount / 512;
  if (g.activeVoxelCount() > p.maxVoxelCount || g.tree().leafCount() > leaves)
    throw std::invalid_argument(
        "sampling field leaf/voxel topology exceeds budget");
  work.add();
  finite(g.background(), mac);
  for (auto v = g.cbeginValueAll(); v; ++v) {
    work.add();
    finite(*v, mac);
  }
}
double quadratic(double a, double b, double c, double t) {
  // Integer centers are exact even when irrelevant neighbors differ by more
  // than the complete representable exponent range.
  if (t == 0)
    return b;
  if (t == 1)
    return c;
  const double scale = std::max({std::abs(a), std::abs(b), std::abs(c)});
  if (scale == 0)
    return 0;
  const double wm = .5 * t * (t - 1), wp = .5 * t * (t + 1), w0 = 1 - t * t;
  Sum s;
  const auto term = [&](double x, double w) {
    if (x == 0 || w == 0)
      return;
    const double normalized = x / scale, product = normalized * w;
    if (normalized == 0 || product == 0 ||
        std::fpclassify(normalized) == FP_SUBNORMAL ||
        std::fpclassify(product) == FP_SUBNORMAL)
      throw std::invalid_argument(
          "quadratic field dynamic range underflows normalized contributions; "
          "rescale or choose nearest/linear");
    s.product(normalized, w);
  };
  term(a, wm);
  term(b, w0);
  term(c, wp);
  const double normalized = s.get(), value = normalized * scale;
  if (!std::isfinite(value) || (normalized != 0 && value == 0))
    throw std::invalid_argument(
        "quadratic field interpolation overflows or underflows Double");
  return value;
}
template <class Accessor>
double component(const Accessor &a, const V &p, PointFieldInterpolation mode,
                 size_t channel, Work &work) {
  const auto get = [&](const openvdb::Coord &c) {
    work.add();
    const auto value = a.getValue(c);
    if constexpr (std::is_arithmetic_v<std::decay_t<decltype(value)>>)
      return double(value);
    else
      return double(value[channel]);
  };
  if (mode == PointFieldInterpolation::Nearest)
    return get(openvdb::Coord::round(p));
  const openvdb::Coord c = openvdb::Coord::floor(p);
  const V t = p - c.asVec3d();
  if (mode == PointFieldInterpolation::Linear) {
    double xy[2];
    for (int z = 0; z < 2; ++z) {
      double x[2];
      for (int y = 0; y < 2; ++y)
        x[y] = std::lerp(get(c + openvdb::Coord(0, y, z)),
                         get(c + openvdb::Coord(1, y, z)), t[0]);
      xy[z] = std::lerp(x[0], x[1], t[1]);
    }
    const double value = std::lerp(xy[0], xy[1], t[2]);
    if (!std::isfinite(value))
      throw std::invalid_argument(
          "linear field interpolation overflows Double");
    return value;
  }
  double yz[3];
  for (int z = -1; z <= 1; ++z) {
    double yv[3];
    for (int y = -1; y <= 1; ++y)
      yv[y + 1] = quadratic(get(c + openvdb::Coord(-1, y, z)),
                            get(c + openvdb::Coord(0, y, z)),
                            get(c + openvdb::Coord(1, y, z)), t[0]);
    yz[z + 1] = quadratic(yv[0], yv[1], yv[2], t[1]);
  }
  return quadratic(yz[0], yz[1], yz[2], t[2]);
}
template <class T> T narrow(double x) {
  const T v = T(x);
  if (!std::isfinite(v) || (x != 0 && v == 0))
    throw std::invalid_argument("sampled value overflows or underflows "
                                "requested native attribute precision");
  return v;
}
template <class G, class Out>
uint64_t apply(const G &field, Grid &target, const Loaded &source,
               const PointFieldSamplingOptions &o, const Affine &pointMap,
               const Affine &fieldMap, bool mac, Work &work) {
  using In = typename G::ValueType;
  constexpr bool vector = !std::is_arithmetic_v<In>;
  if constexpr (std::is_integral_v<In>)
    if (o.interpolation != PointFieldInterpolation::Nearest)
      throw std::invalid_argument(
          "Bool/int32/int64 fields support exact nearest sampling only");
  if (o.operation == PointEditOperation::Add)
    points::appendAttribute<Out, points::NullCodec>(target.tree(),
                                                    o.attributeName, Out{});
  else
    for (const auto *leaf : source.leaves) {
      const auto &a = leaf->constAttributeArray(o.attributeName);
      if (a.type() !=
              points::TypedAttributeArray<Out,
                                          points::NullCodec>::attributeType() ||
          !a.hasConstantStride() || a.stride() != 1 || points::isGroup(a))
        throw std::invalid_argument(
            "Set requires exact sampled native type, NullCodec and stride one");
    }
  const auto accessor = field.getConstAccessor();
  uint64_t count = 0;
  for (const auto *leaf : source.leaves) {
    Predicate selected(*leaf, o.groups);
    points::AttributeHandle<openvdb::Vec3f> positions(
        leaf->constAttributeArray("P"));
    auto *destination = target.tree().probeLeaf(leaf->origin());
    points::AttributeWriteHandle<Out, points::NullCodec> output(
        destination->attributeArray(o.attributeName));
    for (auto i = leaf->beginIndexAll(); i; ++i) {
      work.add();
      if (!selected(*i))
        continue;
      const V world =
                  pointMap.world(i.getCoord(), positions.get(*i),
                                 std::min(pointMap.minimum, fieldMap.minimum)),
              q = fieldMap.index(world,
                                 std::min(pointMap.minimum, fieldMap.minimum));
      Out result{};
      if constexpr (std::is_integral_v<In>) {
        work.add();
        result = accessor.getValue(openvdb::Coord::round(q));
      } else if constexpr (vector) {
        V value;
        for (size_t k = 0; k < 3; ++k) {
          V sample = q;
          if (mac)
            sample[k] += .5;
          value[k] = component(accessor, sample, o.interpolation, k, work);
        }
        if (mac)
          value = fieldMap.vector(value);
        for (size_t k = 0; k < 3; ++k)
          result[k] = narrow<typename Out::ValueType>(value[k]);
      } else
        result = narrow<Out>(component(accessor, q, o.interpolation, 0, work));
      output.set(*i, result);
      ++count;
    }
  }
  return count;
}
template <class G>
uint64_t dispatch(const G &field, Grid &target, const Loaded &source,
                  const PointFieldSamplingOptions &o, const Affine &pointMap,
                  const Affine &fieldMap, bool mac, Work &work) {
  using T = typename G::ValueType;
  if constexpr (std::is_same_v<T, float>) {
    if (o.precision == PointFieldPrecision::PreserveSource)
      return apply<G, float>(field, target, source, o, pointMap, fieldMap, mac,
                             work);
    else
      return apply<G, double>(field, target, source, o, pointMap, fieldMap, mac,
                              work);
  } else if constexpr (std::is_same_v<T, openvdb::Vec3f>) {
    if (!mac && o.precision == PointFieldPrecision::PreserveSource)
      return apply<G, openvdb::Vec3f>(field, target, source, o, pointMap,
                                      fieldMap, mac, work);
    else
      return apply<G, openvdb::Vec3d>(field, target, source, o, pointMap,
                                      fieldMap, mac, work);
  } else
    return apply<G, T>(field, target, source, o, pointMap, fieldMap, mac, work);
}

void writeSampledFile(const std::filesystem::path &output,
                      const std::filesystem::path &privateFile,
                      const Grid::Ptr &grid, const openvdb::MetaMap &metadata,
                      uintmax_t maximumBytes) {
  struct Cleanup {
    std::filesystem::path file;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(file, ignored);
    }
  };
  if (std::filesystem::exists(privateFile))
    throw std::logic_error("point sampling private output already exists");
  Cleanup cleanup{privateFile};
  openvdb::io::File writer(privateFile.string());
  writer.setCompression(openvdb::io::COMPRESS_ZIP);
  writer.write(openvdb::GridPtrVec{grid}, metadata);
  writer.close();
  if (std::filesystem::file_size(privateFile) > maximumBytes)
    throw std::invalid_argument(
        "encoded sampled point output exceeds byte budget");
  bool created = false;
  std::ofstream destination;
  try {
    std::ifstream source(privateFile, std::ios::binary);
    if (!source)
      throw std::runtime_error("cannot open sampled point private output");
    destination.open(output,
                     std::ios::binary | std::ios::out | std::ios::noreplace);
    created = destination.is_open();
    if (!created)
      throw std::runtime_error(
          "cannot exclusively create sampled point output");
    destination.exceptions(std::ios::badbit | std::ios::failbit);
    std::array<char, 64 * 1024> buffer{};
    while (source.read(buffer.data(), std::streamsize(buffer.size())) ||
           source.gcount())
      destination.write(buffer.data(), source.gcount());
    if (!source.eof())
      throw std::runtime_error("sampled point private output read failed");
    destination.flush();
    destination.close();
  } catch (...) {
    destination.exceptions(std::ios::goodbit);
    if (destination.is_open())
      destination.close();
    if (created) {
      std::error_code ignored;
      std::filesystem::remove(output, ignored);
    }
    throw;
  }
}
#endif
} // namespace

PointFieldSamplingResult WriteSampledPointAttribute(
    const std::filesystem::path &input, const std::filesystem::path &fieldPath,
    const std::filesystem::path &output, const PointFieldSamplingOptions &o,
    const std::string &pointGrid, const std::string &fieldGrid,
    const ProcessingOptions &p, const PointAttributeOptions &limits) {
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  (void)input;
  (void)fieldPath;
  (void)output;
  (void)o;
  (void)pointGrid;
  (void)fieldGrid;
  (void)p;
  (void)limits;
  unavailable();
#else
  validateOptions(p, limits, true);
  name(o.attributeName);
  if (o.attributeName == "P" || o.attributeName.starts_with("__group"))
    throw std::invalid_argument(
        "P and native group backing cannot be sampling targets");
  if ((o.operation != PointEditOperation::Add &&
       o.operation != PointEditOperation::Set) ||
      !o.maxWork || !o.maxGrids || o.maxGrids > 4096 || !o.maxMetadataBytes ||
      (o.interpolation != PointFieldInterpolation::Nearest &&
       o.interpolation != PointFieldInterpolation::Linear &&
       o.interpolation != PointFieldInterpolation::Quadratic) ||
      (o.precision != PointFieldPrecision::PromoteFloatingToDouble &&
       o.precision != PointFieldPrecision::PreserveSource))
    throw std::invalid_argument("invalid point field sampling "
                                "operation/interpolation/precision/limits");
  if (output.empty() || std::filesystem::exists(output) ||
      (output.has_parent_path() &&
       !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument(
        "sampled point output must be a new path in an existing directory");
  const auto optionBytes =
      selectionReserve(o.groups, limits) + 8.0L * o.attributeName.size();
  budget(optionBytes + Fixed, p, "point sampling options exceed memory budget");
  initialize();
  tbb::task_arena arena(1);
  return arena.execute([&] {
    Work work{0, o.maxWork};
    Snapshot pointSnapshot(input, p);
    const auto catalog = pointCatalog(pointSnapshot, o, p, work);
    const auto outer = catalog.bytes;
    budget(outer + optionBytes + 1, p,
           "point source snapshot/catalog exceeds joint memory budget");
    auto native = p;
    native.maxInputBytes -= uintmax_t(std::ceil(outer + optionBytes));
    auto source = load(pointSnapshot.file, pointGrid, native, limits);
    if (source.leaves.empty())
      throw std::invalid_argument(
          "native point field sampling needs a nonempty native descriptor; "
          "archived empty schema restoration/repopulation is separate");
    (void)selectionReserve(o.groups, limits, &source.schema);
    if (catalog.metadata + source.metadata > o.maxMetadataBytes)
      throw std::invalid_argument(
          "point descriptor/grid metadata exceeds maxMetadataBytes");
    const bool exists = source.leaves.front()->hasAttribute(o.attributeName);
    if ((o.operation == PointEditOperation::Add && exists) ||
        (o.operation == PointEditOperation::Set && !exists))
      throw std::invalid_argument("sampling Add requires a missing attribute; "
                                  "Set requires an existing attribute");
    if (o.operation == PointEditOperation::Add &&
        source.schema.attributes.size() >= limits.maxAttributes)
      throw std::invalid_argument("sampled attribute exceeds maxAttributes");
    work.add(source.schema.pointCount);
    const long double pointReserve =
        outer + source.snapshot->encoded + 6 * source.decoded +
        12 * source.metadata + source.schema.pointCount * 96.0L +
        source.leaves.size() * 4096.0L + optionBytes + Fixed;
    budget(pointReserve + Fixed + 1, p,
           "joint point clone/edit/writer reserves exceed memory budget");
    auto remaining = p;
    remaining.maxInputBytes -= uintmax_t(std::ceil(pointReserve));
    const auto pointSlots = uint64_t(source.leaves.size()) * 512;
    const auto pointVoxels =
        std::max(pointSlots, source.grid->activeVoxelCount());
    if (pointVoxels > p.maxVoxelCount)
      throw std::invalid_argument(
          "point source exceeds the shared sampling topology budget");
    remaining.maxVoxelCount -= pointVoxels;
    const bool same = std::filesystem::equivalent(input, fieldPath);
    auto field = loadField(same ? source.snapshot->file : fieldPath, fieldGrid,
                           o, remaining, work);
    if (catalog.metadata + source.metadata + field.metadata >
        o.maxMetadataBytes)
      throw std::invalid_argument(
          "aggregate point/field metadata exceeds maxMetadataBytes");
    const Affine pointMap(source.grid->transform()),
        fieldMap(field.grid->transform());
    if (field.mac) {
      const double dx = fieldMap.minimum;
      for (size_t k = 0; k < 3; ++k) {
        const double length = std::hypot(
            fieldMap.basis[k][0], fieldMap.basis[k][1], fieldMap.basis[k][2]);
        if (std::abs(length - dx) > dx * 1e-8)
          throw std::invalid_argument(
              "native MAC sampling requires uniform orthogonal axes");
        for (size_t j = 0; j < k; ++j)
          if (std::abs(fieldMap.basis[k].dot(fieldMap.basis[j])) >
              dx * dx * 1e-8)
            throw std::invalid_argument(
                "native MAC sampling requires uniform orthogonal axes");
      }
    }
    // The ZIP writer does not force paged arrays during its metadata pass.
    // Materialize the already-budgeted native payloads before cloning so both
    // edited and untouched arrays serialize their actual owned values.
    for (const auto *leaf : source.leaves)
      for (size_t index = 0; index < leaf->attributeSet().size(); ++index) {
        work.add();
        leaf->constAttributeArray(index).loadData();
      }
    auto target = detail::DeepCopyGridExact(*source.grid);
    PointFieldSamplingResult result;
    result.inputCount = source.schema.pointCount;
    result.fieldGridName = field.grid->getName();
    result.fieldValueType = field.grid->valueType();
    const auto run = [&]<class G>() {
      auto g = openvdb::gridPtrCast<G>(field.grid);
      validateField(*g, remaining, work, field.mac);
      result.selectedCount =
          dispatch(*g, *target, source, o, pointMap, fieldMap, field.mac, work);
    };
    if (field.grid->isType<openvdb::BoolGrid>())
      run.operator()<openvdb::BoolGrid>();
    else if (field.grid->isType<openvdb::Int32Grid>())
      run.operator()<openvdb::Int32Grid>();
    else if (field.grid->isType<openvdb::Int64Grid>())
      run.operator()<openvdb::Int64Grid>();
    else if (field.grid->isType<openvdb::FloatGrid>())
      run.operator()<openvdb::FloatGrid>();
    else if (field.grid->isType<openvdb::DoubleGrid>())
      run.operator()<openvdb::DoubleGrid>();
    else if (field.grid->isType<openvdb::Vec3SGrid>())
      run.operator()<openvdb::Vec3SGrid>();
    else
      run.operator()<openvdb::Vec3DGrid>();
    if (field.grid->isType<openvdb::Vec3SGrid>() ||
        field.grid->isType<openvdb::Vec3DGrid>())
      result.vectorSemantics =
          field.mac
              ? "world components per second (native negative-face MAC J*u)"
              : "world components (source vector tag " +
                    std::to_string(int(field.grid->getVectorType())) + ")";
    result.writtenCount = result.selectedCount;
    target->setName(p.gridName);
    target->removeMeta("container:point_count");
    target->insertMeta("container:point_count",
                       openvdb::Int64Metadata(int64_t(result.inputCount)));
    long double actual = target->memUsage() + metadataBytes(*target);
    std::vector<const Leaf *> leaves;
    leaves.reserve(source.leaves.size());
    for (const auto *original : source.leaves) {
      const auto *leaf = target->tree().probeConstLeaf(original->origin());
      if (!leaf || leaf->pointCount() != original->pointCount())
        throw std::runtime_error("point sampling changed native offsets");
      leaf->validateOffsets();
      leaves.push_back(leaf);
      actual += metadataBytes(leaf->attributeSet().descriptor().getMetadata());
      for (size_t i = 0; i < leaf->attributeSet().size(); ++i)
        actual += leaf->constAttributeArray(i).memUsageIfLoaded();
    }
    if (target->transform() != source.grid->transform() ||
        !target->tree().hasSameTopology(source.grid->tree()))
      throw std::runtime_error(
          "point sampling changed native transform/topology");
    budget(outer + source.snapshot->encoded + source.decoded +
               4 * source.metadata + field.resident + 3 * actual + optionBytes +
               Fixed,
           p, "sampled point output/writer exceeds joint memory budget");
    result.schema = describe(*target, leaves, result.inputCount);
    writeSampledFile(output, source.snapshot->directory / "sampled-output.vdb",
                     target, *catalog.fileMetadata, p.maxInputBytes);
    return result;
  });
#endif
}
} // namespace container::geometry::vdb
