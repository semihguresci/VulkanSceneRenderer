#include "Container/geometry/OpenVdbSlicePreview.h"
#include "Container/geometry/OpenVdbGridSelection.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Count.h>
#include <openvdb/tools/Interpolation.h>
#include <set>
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using V = openvdb::Vec3d;
constexpr long double Fixed = 1024 * 1024;
double norm(const V &v) { return std::hypot(v.x(), v.y(), v.z()); }
bool finite(const V &v) {
  return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}
double gap(double x) {
  return std::max(std::nextafter(x, INFINITY) - x,
                  x - std::nextafter(x, -INFINITY));
}
std::string text(double x) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << x;
  return out.str();
}
std::string text(const V &v) {
  return text(v.x()) + ", " + text(v.y()) + ", " + text(v.z());
}
void budget(long double bytes, const SlicePreviewOptions &o) {
  if (!std::isfinite(bytes) || bytes > o.maxInputBytes)
    throw std::invalid_argument(
        "slice source/metadata/geometry workspace exceeds memory budget");
}
long double metadata(const openvdb::MetaMap &g) {
  long double n = sizeof(g);
  for (auto it = g.beginMeta(); it != g.endMeta(); ++it)
    n += 4.0L * (it->first.size() + it->second->typeName().size() +
                 it->second->size()) +
         256;
  return n;
}
struct Snapshot {
  std::filesystem::path directory, file;
  uintmax_t encoded{};
  Snapshot(const std::filesystem::path &input, const SlicePreviewOptions &o) {
    if (!std::filesystem::is_regular_file(input))
      throw std::invalid_argument("slice input is not a regular file");
    encoded = std::filesystem::file_size(input);
    budget(static_cast<long double>(encoded) + Fixed, o);
    static std::atomic_uint64_t sequence{};
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      auto candidate =
          std::filesystem::temp_directory_path() /
          ("container-slice-snapshot-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++sequence));
      if (std::filesystem::create_directory(candidate)) {
        directory = std::move(candidate);
        break;
      }
    }
    if (directory.empty())
      throw std::runtime_error("cannot reserve slice snapshot directory");
    file = directory / "source.vdb";
    try {
      std::ifstream source(input, std::ios::binary);
      std::ofstream target(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!source || !target)
        throw std::runtime_error("cannot open slice input/private snapshot");
      source.exceptions(std::ios::badbit);
      target.exceptions(std::ios::badbit | std::ios::failbit);
      std::array<char, 65536> buffer{};
      uintmax_t remaining = encoded;
      while (remaining) {
        const auto n =
            std::streamsize(std::min<uintmax_t>(remaining, buffer.size()));
        source.read(buffer.data(), n);
        if (source.gcount() != n)
          throw std::runtime_error("slice input changed size during snapshot");
        target.write(buffer.data(), n);
        remaining -= uintmax_t(n);
      }
      if (source.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("slice input grew during snapshot");
      target.flush();
      target.close();
    } catch (...) {
      cleanup();
      throw;
    }
  }
  void cleanup() const noexcept {
    std::error_code e;
    std::filesystem::remove(file, e);
    std::filesystem::remove(directory, e);
  }
  ~Snapshot() { cleanup(); }
};
template <class Grid> long double fullMemory(const openvdb::GridBase &grid) {
  return openvdb::tools::memUsageIfLoaded(
      static_cast<const Grid &>(grid).tree(), false);
}
long double fullMemory(const openvdb::GridBase &grid) {
  if (grid.isType<openvdb::FloatGrid>())
    return fullMemory<openvdb::FloatGrid>(grid);
  if (grid.isType<openvdb::DoubleGrid>())
    return fullMemory<openvdb::DoubleGrid>(grid);
  if (grid.isType<openvdb::Vec3SGrid>())
    return fullMemory<openvdb::Vec3SGrid>(grid);
  if (grid.isType<openvdb::Vec3DGrid>())
    return fullMemory<openvdb::Vec3DGrid>(grid);
  if (grid.isType<openvdb::BoolGrid>())
    return fullMemory<openvdb::BoolGrid>(grid);
  if (grid.isType<openvdb::MaskGrid>())
    return fullMemory<openvdb::MaskGrid>(grid);
  if (grid.isType<openvdb::Int32Grid>())
    return fullMemory<openvdb::Int32Grid>(grid);
  if (grid.isType<openvdb::Int64Grid>())
    return fullMemory<openvdb::Int64Grid>(grid);
  if (grid.isType<openvdb::Vec3IGrid>())
    return fullMemory<openvdb::Vec3IGrid>(grid);
  throw std::invalid_argument("unsupported native slice grid type");
}
struct Plane {
  V origin, u, v, normal;
  double feature{};
  uint64_t samples{}, triangles{}, baseWork{};
  Plane(float scale, const SlicePreviewOptions &o) {
    if (!std::isfinite(scale) || scale <= 0 || !o.maxSamples ||
        o.maxSamples > 262144 || !o.maxTriangles || !o.maxActiveVoxels ||
        !o.maxInputBytes || !o.maxMetadataBytes || !o.maxWork ||
        o.maxWork > 1'000'000'000)
      throw std::invalid_argument(
          "slice scale/sample/topology/memory/work limits are invalid");
    if (o.valueMode < SliceValue::Scalar ||
        o.valueMode > SliceValue::VectorMagnitude ||
        o.colorMap < SliceColorMap::Gray ||
        o.colorMap > SliceColorMap::Diverging)
      throw std::invalid_argument("slice value/color mode is invalid");
    if (o.integerRange && (*o.integerRange)[0] >= (*o.integerRange)[1])
      throw std::invalid_argument(
          "slice integer range requires exact minimum < maximum");
    if (!std::isfinite(o.opacity) || o.opacity < 0 || o.opacity > 1 ||
        !std::isfinite(o.colorMinimum) || !std::isfinite(o.colorMaximum) ||
        o.colorMaximum <= o.colorMinimum ||
        !std::isfinite(o.colorMaximum - o.colorMinimum))
      throw std::invalid_argument("slice opacity/color range is invalid");
    origin = V(o.origin.data());
    u = V(o.axisU.data());
    v = V(o.axisV.data());
    if (!finite(origin) || !finite(u) || !finite(v) ||
        !std::isfinite(norm(u)) || !std::isfinite(norm(v)) || norm(u) == 0 ||
        norm(v) == 0)
      throw std::invalid_argument(
          "slice plane origin/basis must be finite and nonzero");
    for (int k = 0; k < 3; ++k)
      if (std::abs(origin[k]) > 1e12)
        throw std::invalid_argument(
            "slice plane origin exceeds1e12 source-world units");
    u /= norm(u);
    v /= norm(v);
    normal = u.cross(v);
    const double sine = norm(normal);
    if (sine < 1e-6)
      throw std::invalid_argument(
          "slice plane basis is collinear/ill-conditioned");
    normal /= sine;
    for (int k = 0; k < 2; ++k)
      if (o.sampleCounts[k] < 2 || o.sampleCounts[k] > 1024 ||
          !std::isfinite(o.worldExtent[k]) || o.worldExtent[k] < 1e-6 ||
          o.worldExtent[k] > 1e6)
        throw std::invalid_argument(
            "slice sample axes/extents exceed supported ranges");
    samples = uint64_t(o.sampleCounts[0]) * o.sampleCounts[1];
    triangles = 2ull * (o.sampleCounts[0] - 1) * (o.sampleCounts[1] - 1);
    if (samples > o.maxSamples || triangles > o.maxTriangles)
      throw std::invalid_argument(
          "slice sample/triangle product exceeds budget");
    feature = std::min(o.worldExtent[0] / (o.sampleCounts[0] - 1),
                       o.worldExtent[1] / (o.sampleCounts[1] - 1)) *
              sine;
    baseWork = samples * 32 + triangles;
    if (baseWork > o.maxWork)
      throw std::invalid_argument(
          "slice sampling/geometry work exceeds budget");
  }
  V point(uint32_t x, uint32_t y, const SlicePreviewOptions &o) const {
    const double a = (double(x) / (o.sampleCounts[0] - 1) - .5) *
                     o.worldExtent[0],
                 b = (double(y) / (o.sampleCounts[1] - 1) - .5) *
                     o.worldExtent[1];
    V p = origin + u * a + v * b;
    for (int k = 0; k < 3; ++k) {
      const double first = u[k] * a, second = v[k] * b;
      const double error = gap(origin[k]) + gap(first) + gap(second) +
                           gap(origin[k] + first) + gap(p[k]);
      if (!std::isfinite(p[k]) || error > feature * 1e-4)
        throw std::invalid_argument(
            "slice plane loses Double world precision; recenter/rescale");
    }
    return p;
  }
};
bool isMac(const openvdb::GridBase &g) {
  const auto a = g.getMetadata<openvdb::StringMetadata>("container:mac_layout"),
             b = g.getMetadata<openvdb::StringMetadata>(
                 "container:velocity_units");
  return g.getGridClass() == openvdb::GRID_STAGGERED && !g.isInWorldSpace() &&
         g.getVectorType() == openvdb::VEC_CONTRAVARIANT_RELATIVE && a && b &&
         a->value() == "negative_faces" && b->value() == "voxels_per_second";
}
struct Map {
  openvdb::math::MapBase::ConstPtr map;
  V origin;
  std::array<V, 3> axes, inverse;
  Map(const openvdb::GridBase &g, bool mac)
      : map(g.transform().baseMap()), origin(map->applyMap(V(0))) {
    if (!g.transform().isLinear() || !finite(origin))
      throw std::invalid_argument("slice requires a finite affine grid map");
    std::array<double, 3> sizes;
    for (int k = 0; k < 3; ++k) {
      V e(0);
      e[k] = 1;
      axes[k] = map->applyJacobian(e);
      inverse[k] = map->applyInverseJacobian(e);
      sizes[k] = norm(axes[k]);
      if (!finite(axes[k]) || !finite(inverse[k]) || sizes[k] < 1e-6 ||
          sizes[k] > 1e6)
        throw std::invalid_argument(
            "slice source voxel axes must span[1e-6,1e6]");
      for (int j = 0; j < 3; ++j)
        if (std::abs(inverse[k][j]) > 1e8)
          throw std::invalid_argument(
              "slice source affine inverse is ill-conditioned");
    }
    const double det = (axes[0] / sizes[0])
                           .dot((axes[1] / sizes[1]).cross(axes[2] / sizes[2]));
    if (!std::isfinite(det) || std::abs(det) < 1e-8)
      throw std::invalid_argument(
          "slice source affine is singular/ill-conditioned");
    if (mac)
      for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b)
          if (std::abs(sizes[a] / sizes[b] - 1) > 1e-9 ||
              std::abs(axes[a].dot(axes[b]) / (sizes[a] * sizes[b])) > 1e-9)
            throw std::invalid_argument(
                "slice native MAC requires uniform orthogonal voxels");
  }
  V index(const V &p, double feature) const {
    const V delta = p - origin, q = map->applyInverseJacobian(delta);
    if (!finite(q))
      throw std::invalid_argument("slice sample inverse is not finite");
    double resolution = 0;
    for (int k = 0; k < 3; ++k) {
      if (q[k] < double(INT32_MIN) + 16 || q[k] > double(INT32_MAX) - 16)
        throw std::invalid_argument(
            "slice samples exceed safe interpolation coordinates");
      double error = gap(q[k]), partial = 0;
      for (int j = 0; j < 3; ++j) {
        const double product = inverse[j][k] * delta[j];
        partial += product;
        error += gap(product) + gap(partial) +
                 std::abs(inverse[j][k]) *
                     (gap(delta[j]) + gap(p[j]) + gap(origin[j]));
      }
      resolution += error * norm(axes[k]);
    }
    if (!std::isfinite(resolution) || resolution > feature * 1e-4)
      throw std::invalid_argument("slice source-index/world precision cannot "
                                  "resolve sampling; recenter source");
    return q;
  }
};
template <class T> void validValue(const T &v, double limit) {
  if constexpr (std::is_integral_v<T>) {
    return;
  } else if constexpr (std::is_arithmetic_v<T>) {
    if (!std::isfinite(double(v)) || std::abs(double(v)) > limit)
      throw std::invalid_argument(
          "slice stored value/background exceeds finite component range");
  } else
    for (int k = 0; k < 3; ++k)
      validValue(v[k], limit);
}
template <class Grid> struct View {
  using Native = typename Grid::ValueType;
  using ValueType = std::conditional_t<std::is_arithmetic_v<Native>, double, V>;
  const Grid &g;
  ValueType getValue(const openvdb::Coord &c) const {
    return ValueType(g.tree().getValue(c));
  }
};
openvdb::Coord nearestCenter(const V &q) {
  openvdb::Coord result;
  for (int axis = 0; axis < 3; ++axis) {
    const double lower = std::floor(q[axis]);
    // Map::index already enforces Int32 margins, so this midpoint is exact.
    // Adding .5 to q first can round nextafter(.5, 0) up to the next cell.
    result[axis] = int32_t(lower + (q[axis] >= lower + .5 ? 1 : 0));
  }
  return result;
}
V worldMac(const Map &map, const V &vector) {
  V out(0);
  for (int r = 0; r < 3; ++r) {
    double sum = 0, correction = 0;
    const auto add = [&](double value) {
      const double next = sum + value;
      correction += std::abs(sum) >= std::abs(value) ? (sum - next) + value
                                                     : (value - next) + sum;
      sum = next;
    };
    for (int c = 0; c < 3; ++c) {
      const double product = map.axes[c][r] * vector[c];
      if (!std::isfinite(product) ||
          (map.axes[c][r] != 0 && vector[c] != 0 && product == 0))
        throw std::invalid_argument("slice MAC world-component product "
                                    "overflows/underflows; rescale source");
      add(product);
      // Recover the product's rounding error as well as addition error. Large
      // opposing MAC components can otherwise produce an inaccurate small
      // world component despite compensated summation of rounded products.
      add(std::fma(map.axes[c][r], vector[c], -product));
    }
    out[r] = sum + correction;
  }
  return out;
}
glm::vec3 unitColor(double t, SliceColorMap map) {
  if (map == SliceColorMap::Gray)
    return glm::vec3(float(t));
  if (map == SliceColorMap::BlueRed)
    return {float(t), 0, float(1 - t)};
  return t <= .5 ? glm::vec3(float(2 * t), float(2 * t), 1)
                 : glm::vec3(1, float(2 - 2 * t), float(2 - 2 * t));
}
glm::vec3 color(double value, const SlicePreviewOptions &o, uint64_t &below,
                uint64_t &above) {
  if (value < o.colorMinimum)
    ++below;
  if (value > o.colorMaximum)
    ++above;
  const double t =
      value <= o.colorMinimum ? 0
      : value >= o.colorMaximum
          ? 1
          : (value - o.colorMinimum) / (o.colorMaximum - o.colorMinimum);
  return unitColor(t, o.colorMap);
}
glm::vec3 position(const V &p, float scale, double feature) {
  glm::vec3 out;
  for (int k = 0; k < 3; ++k) {
    const double exact = p[k] * scale;
    out[k] = float(exact);
    const double step = feature * scale,
                 spacing = std::max(
                     double(std::nextafter(out[k], INFINITY)) - out[k],
                     double(out[k]) - std::nextafter(out[k], -INFINITY));
    if (!std::isfinite(exact) || !std::isfinite(out[k]) ||
        !std::isfinite(step) || step == 0 ||
        std::abs(double(out[k]) - exact) > step * .001 || spacing > step * .001)
      throw std::invalid_argument("slice Float32 vertices lose local sample "
                                  "precision; recenter/rescale");
  }
  return out;
}
template <class Grid>
dotbim::Model build(const Grid &g, const std::string &descriptor, float scale,
                    const SlicePreviewOptions &o, const Plane &plane,
                    long double resident) {
  constexpr bool scalar = std::is_arithmetic_v<typename Grid::ValueType>;
  constexpr bool integer = std::is_integral_v<typename Grid::ValueType>;
  constexpr bool integerVector = std::is_same_v<Grid, openvdb::Vec3IGrid>;
  const bool mac = !scalar && isMac(g);
  if (o.integerRange && !integer)
    throw std::invalid_argument(
        "exact integer color ranges require an integer scalar grid");
  if constexpr (integer)
    if (o.colorMinimum != 0 || o.colorMaximum != 1)
      throw std::invalid_argument(
          "integer scalar slices require exact integerRange or automatic "
          "range, not floating color bounds");
  if constexpr (integer || integerVector)
    if (g.getGridClass() != openvdb::GRID_UNKNOWN ||
        (!scalar && (!g.isInWorldSpace() || mac)))
      throw std::invalid_argument("integer slices require UNKNOWN scalar or "
                                  "collocated world vector grids");
  if constexpr (scalar) {
    if (o.valueMode != SliceValue::Scalar ||
        g.getGridClass() == openvdb::GRID_STAGGERED)
      throw std::invalid_argument(
          "slice scalar mode requires a collocated scalar field");
  } else {
    if (o.valueMode == SliceValue::Scalar ||
        (!mac &&
         (!g.isInWorldSpace() || g.getGridClass() != openvdb::GRID_UNKNOWN)))
      throw std::invalid_argument("slice vectors require world collocated "
                                  "UNKNOWN fields or explicitly tagged MAC");
  }
  const Map map(g, mac);
  const double limit = mac ? 1e24 : 1e12;
  validValue(g.background(), limit);
  uint64_t work = plane.baseWork;
  for (auto it = g.cbeginValueAll(); it; ++it) {
    if (work == o.maxWork)
      throw std::invalid_argument("slice input validation exceeds work budget");
    ++work;
    validValue(*it, limit);
  }
  const long double geometry = plane.samples * sizeof(Vertex) +
                               3.0L * plane.triangles * sizeof(uint32_t);
  budget(resident + 2 * geometry + (integer ? plane.samples * 8.0L : 0) +
             4.0L * o.maxMetadataBytes + Fixed,
         o);
  dotbim::Model model;
  model.vertices.reserve(size_t(plane.samples));
  model.indices.reserve(size_t(3 * plane.triangles));
  V minimum(INFINITY), maximum(-INFINITY);
  double rawMin = INFINITY, rawMax = -INFINITY;
  uint64_t below = 0, above = 0;
  std::vector<int64_t> integers;
  if constexpr (integer)
    integers.reserve(size_t(plane.samples));
  const View<Grid> view{g};
  for (uint32_t y = 0; y < o.sampleCounts[1]; ++y)
    for (uint32_t x = 0; x < o.sampleCounts[0]; ++x) {
      const auto p = plane.point(x, y, o), q = map.index(p, plane.feature);
      double value;
      if constexpr (integer) {
        const int64_t exact =
            g.tree().getValue(nearestCenter(q));
        integers.push_back(exact);
        value = double(exact);
      } else if constexpr (scalar)
        value = openvdb::tools::BoxSampler::sample(view, q);
      else {
        V vector;
        if constexpr (integerVector)
          vector = view.getValue(nearestCenter(q));
        else
          vector = mac ? openvdb::tools::StaggeredBoxSampler::sample(view, q)
                       : openvdb::tools::BoxSampler::sample(view, q);
        if (mac)
          vector = worldMac(map, vector);
        if (!finite(vector))
          throw std::invalid_argument("slice sampled vector is nonfinite");
        value = o.valueMode == SliceValue::VectorMagnitude
                    ? norm(vector)
                    : vector[int(o.valueMode) - int(SliceValue::VectorX)];
      }
      if (!std::isfinite(value))
        throw std::invalid_argument("slice sampled scalar is nonfinite");
      rawMin = std::min(rawMin, value);
      rawMax = std::max(rawMax, value);
      Vertex v;
      v.position = position(p, scale, plane.feature);
      if constexpr (integer)
        v.color = glm::vec3(.5f);
      else
        v.color = color(value, o, below, above);
      v.colorAlpha = 1;
      v.normal = {float(plane.normal.x()), float(plane.normal.y()),
                  float(plane.normal.z())};
      v.tangent = {float(plane.u.x()), float(plane.u.y()), float(plane.u.z()),
                   1};
      v.texCoord = {float(double(x) / (o.sampleCounts[0] - 1)),
                    float(double(y) / (o.sampleCounts[1] - 1))};
      model.vertices.push_back(v);
      for (int k = 0; k < 3; ++k) {
        minimum[k] = std::min(minimum[k], double(v.position[k]));
        maximum[k] = std::max(maximum[k], double(v.position[k]));
      }
    }
  std::string minimumText = text(rawMin), maximumText = text(rawMax);
  std::optional<std::array<int64_t, 2>> usedIntegerRange;
  if constexpr (integer) {
    const auto [minimum, maximum] =
        std::minmax_element(integers.begin(), integers.end());
    minimumText = std::to_string(*minimum);
    maximumText = std::to_string(*maximum);
    auto range =
        o.integerRange.value_or(std::array<int64_t, 2>{*minimum, *maximum});
    if constexpr (std::is_same_v<typename Grid::ValueType, bool>)
      if (!o.integerRange)
        range = {0, 1};
    usedIntegerRange = range;
    const auto lo = range[0], hi = range[1];
    for (size_t n = 0; n < integers.size(); ++n) {
      const auto exact = integers[n];
      below += exact < lo;
      above += exact > hi;
      const double t = lo == hi      ? .5
                       : exact <= lo ? 0
                       : exact >= hi ? 1
                                     : double(uint64_t(exact) - uint64_t(lo)) /
                                           double(uint64_t(hi) - uint64_t(lo));
      model.vertices[n].color = unitColor(t, o.colorMap);
    }
  }
  for (uint32_t y = 0; y + 1 < o.sampleCounts[1]; ++y)
    for (uint32_t x = 0; x + 1 < o.sampleCounts[0]; ++x) {
      const uint32_t a = y * o.sampleCounts[0] + x, b = a + 1,
                     c = a + o.sampleCounts[0], d = c + 1;
      model.indices.insert(model.indices.end(), {a, b, d, a, d, c});
    }
  dotbim::Material material;
  material.pbr.baseColor = {1, 1, 1, o.opacity};
  material.pbr.unlit = true;
  material.pbr.doubleSided = true;
  material.pbr.alphaMode = o.opacity < 1
                               ? container::material::AlphaMode::Blend
                               : container::material::AlphaMode::Opaque;
  model.materials.push_back(material);
  const V center = (minimum + maximum) * .5;
  glm::vec3 mid(float(center.x()), float(center.y()), float(center.z()));
  V extent;
  for (int k = 0; k < 3; ++k)
    extent[k] =
        std::max(std::abs(minimum[k] - mid[k]), std::abs(maximum[k] - mid[k]));
  const double exactRadius = norm(extent);
  float radius = float(exactRadius);
  if (radius < exactRadius)
    radius = std::nextafter(radius, INFINITY);
  if (!std::isfinite(radius) || radius <= 0)
    throw std::invalid_argument(
        "slice display bounds cannot be represented inFloat32");
  model.meshRanges.push_back(
      {0, 0, uint32_t(model.indices.size()), mid, radius});
  dotbim::Element element;
  element.meshId = 0;
  element.geometryKind = dotbim::GeometryKind::Mesh;
  element.color = glm::vec4(1);
  element.materialIndex = 0;
  element.doubleSided = true;
  element.guid = "vdb-slice:" + descriptor;
  element.type = "OpenVDB Field Slice";
  element.displayName = g.getName();
  element.sourceId = descriptor;
  element.properties = {
      {"OpenVDB", "Preview",
       "Explicit source-world sampled plane; display geometry only", ""},
      {"OpenVDB", "Value type", g.valueType(), ""},
      {"OpenVDB", "Grid class",
       openvdb::GridBase::gridClassToString(g.getGridClass()), ""},
      {"OpenVDB", "Frame",
       mac      ? "Negative-face MAC reconstructed in world units"
       : scalar ? "Stored scalar values"
                : "Stored collocated WORLD vector components",
       ""},
      {"OpenVDB", "Plane origin", text(plane.origin), ""},
      {"OpenVDB", "Plane U", text(plane.u), ""},
      {"OpenVDB", "Plane V", text(plane.v), ""},
      {"OpenVDB", "Samples", std::to_string(plane.samples), ""},
      {"OpenVDB", "Triangles", std::to_string(plane.triangles), ""},
      {"OpenVDB", "Value mode", std::to_string(int(o.valueMode)), ""},
      {"OpenVDB", "Sample minimum", minimumText, ""},
      {"OpenVDB", "Sample maximum", maximumText, ""},
      {"OpenVDB", "Sampling",
       integer || integerVector
           ? "Nearest integer center; half ties toward greater index"
           : "Trilinear native field values",
       ""},
      {"OpenVDB", "Clipped below", std::to_string(below), ""},
      {"OpenVDB", "Clipped above", std::to_string(above), ""},
      {"OpenVDB", "Work", std::to_string(work), ""}};
  if constexpr (!scalar) {
    element.properties.push_back(
        {"OpenVDB", "Vector semantics",
         openvdb::GridBase::vecTypeToString(g.getVectorType()), ""});
    if (!mac)
      element.properties.push_back(
          {"OpenVDB", "World vector interpretation",
           "Raw stored WORLD components: semantic tag retained; no Jacobian "
           "multiplication, point translation or normal normalization applied",
           ""});
  }
  if (usedIntegerRange)
    element.properties.push_back({"OpenVDB", "Exact integer color range",
                                  std::to_string((*usedIntegerRange)[0]) +
                                      ", " +
                                      std::to_string((*usedIntegerRange)[1]),
                                  ""});
  long double properties = 0;
  const auto charge = [&] {
    const auto &p = element.properties.back();
    properties += p.set.size() + p.name.size() + p.value.size() +
                  p.category.size() + sizeof(p);
    if (properties > o.maxMetadataBytes)
      throw std::invalid_argument("slice text metadata exceeds budget");
  };
  for (const auto &p : element.properties)
    properties += p.set.size() + p.name.size() + p.value.size() +
                  p.category.size() + sizeof(p);
  if (properties > o.maxMetadataBytes)
    throw std::invalid_argument("slice properties exceed metadata budget");
  std::ostringstream transform;
  transform.imbue(std::locale::classic());
  g.transform().print(transform);
  element.properties.push_back(
      {"OpenVDB", "Source transform", transform.str(), ""});
  charge();
  for (auto it = g.beginMeta(); it != g.endMeta(); ++it) {
    element.properties.push_back({"OpenVDB metadata", it->first,
                                  it->second->str(), it->second->typeName()});
    charge();
  }
  model.elements.push_back(std::move(element));
  model.unitMetadata.hasSourceUnits = true;
  model.unitMetadata.sourceUnits = "OpenVDB world units";
  model.unitMetadata.hasImportScale =
      model.unitMetadata.hasEffectiveImportScale = true;
  model.unitMetadata.importScale = model.unitMetadata.effectiveImportScale =
      scale;
  model.importReport.sourceFormat = "OpenVDB";
  model.importReport.sourceItemName = "slice fields";
  model.importReport.sourceProductCount =
      model.importReport.importedProductCount = 1;
  model.importReport.completeness = ImportCompleteness::Complete;
  if (below || above) {
    model.importReport.representationWarnings["OpenVDB.SliceColorClipped"] =
        size_t(below + above);
    model.importReport.diagnostics.push_back(
        {"OpenVDB.SliceColorClipped", 0, 0, "vdb-slice:" + descriptor,
         "Explicit color range saturated " + std::to_string(below) +
             " samples below and " + std::to_string(above) +
             " above; raw field values are unchanged."});
  }
  return model;
}
#endif
} // namespace
dotbim::Model LoadSlicePreviewFromFile(const std::filesystem::path &input,
                                       const std::string &selection,
                                       float scale,
                                       const SlicePreviewOptions &o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  const Plane plane(scale, o);
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });
  tbb::task_arena arena(1);
  return arena.execute([&]() -> dotbim::Model {
    if (!openvdb::io::Archive::isDelayedLoadingEnabled())
      throw std::invalid_argument(
          "bounded slice preview requires delayed loading; unset "
          "OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
    throw std::invalid_argument(
        "bounded slice preview requires an SDK with delayed-loading support");
#else
    const Snapshot snapshot(input, o);
    detail::RequireSeekableFile(snapshot.file);
    openvdb::io::File file(snapshot.file.string());
    file.setCopyMaxBytes(0);
    file.open(true);
    std::set<std::string> names;
    long double catalog = static_cast<long double>(snapshot.encoded) + Fixed;
    const auto fileMeta = file.getMetadata();
    long double aggregateMetadata = metadata(*fileMeta);
    const auto checkMetadata = [&] {
      if (aggregateMetadata > o.maxMetadataBytes)
        throw std::invalid_argument(
            "aggregate slice file/grid metadata exceeds maxMetadataBytes");
      budget(catalog + 4 * aggregateMetadata + Fixed, o);
    };
    checkMetadata();
    for (auto it = file.beginName(); it != file.endName(); ++it) {
      const auto name = it.gridName();
      catalog += 2.0L * name.size() + 128;
      checkMetadata();
      if (names.size() >= 4096 || !names.insert(name).second)
        throw std::invalid_argument(
            "slice input has too many/colliding descriptors; rename grids");
    }
    std::string descriptor;
    bool found = false;
    long double selectedMetadata = 0;
    for (auto it = file.beginName(); it != file.endName(); ++it) {
      auto g = file.readGridMetadata(it.gridName());
      aggregateMetadata += metadata(*g);
      checkMetadata();
      if (!GridSelectionMatches(selection, it.gridName(), g->getName()))
        continue;
      const bool type =
          g->isType<openvdb::FloatGrid>() || g->isType<openvdb::DoubleGrid>() ||
          g->isType<openvdb::Vec3SGrid>() || g->isType<openvdb::Vec3DGrid>() ||
          g->isType<openvdb::BoolGrid>() || g->isType<openvdb::MaskGrid>() ||
          g->isType<openvdb::Int32Grid>() || g->isType<openvdb::Int64Grid>() ||
          g->isType<openvdb::Vec3IGrid>();
      if (!type) {
        if (!selection.empty())
          throw std::invalid_argument("selected slice grid is not a supported "
                                      "native scalar/vector field");
        continue;
      }
      if (selection.empty() && found)
        continue;
      if (found)
        throw std::invalid_argument(
            "slice grid selector is ambiguous; use descriptor:<exact name>");
      const auto count = g->getMetadata<openvdb::Int64Metadata>(
                     openvdb::GridBase::META_FILE_VOXEL_COUNT),
                 memory = g->getMetadata<openvdb::Int64Metadata>(
                     openvdb::GridBase::META_FILE_MEM_BYTES);
      if (!count || !memory || count->value() < 0 || memory->value() < 0 ||
          uint64_t(count->value()) > o.maxActiveVoxels)
        throw std::invalid_argument(
            "slice requires recorded topology/memory statistics within budget");
      selectedMetadata = metadata(*g);
      budget(catalog + memory->value() + 4 * aggregateMetadata + Fixed, o);
      descriptor = it.gridName();
      found = true;
    }
    if (!found)
      throw std::invalid_argument("no matching scalar/world-vector slice grid");
    auto grid = file.readGrid(descriptor);
    if (grid->activeVoxelCount() > o.maxActiveVoxels ||
        grid->baseTree().leafCount() >
            o.maxActiveVoxels / 512 + (o.maxActiveVoxels % 512 != 0))
      throw std::invalid_argument("slice loaded topology exceeds budget");
    // The descriptor set remains alive through build(), so its strings and
    // nodes share the source/geometry budget rather than disappearing here.
    const auto loadedMetadata = metadata(*grid);
    aggregateMetadata += loadedMetadata;
    checkMetadata();
    const auto full = fullMemory(*grid);
    const long double resident = catalog - Fixed + full + 4 * aggregateMetadata;
    if (!std::isfinite(resident) || resident + Fixed > o.maxInputBytes)
      throw std::invalid_argument(
          "slice full-buffer budget exceeded before leaf materialization");
    grid->readNonresidentBuffers();
    if (grid->memUsage() > full)
      throw std::runtime_error(
          "slice payload exceeded full-buffer memory preflight");
    file.close();
    if (auto g = openvdb::gridPtrCast<openvdb::FloatGrid>(grid))
      return build(*g, descriptor, scale, o, plane, resident);
    if (auto g = openvdb::gridPtrCast<openvdb::DoubleGrid>(grid))
      return build(*g, descriptor, scale, o, plane, resident);
    if (auto g = openvdb::gridPtrCast<openvdb::Vec3SGrid>(grid))
      return build(*g, descriptor, scale, o, plane, resident);
    if (auto g = openvdb::gridPtrCast<openvdb::Vec3DGrid>(grid))
      return build(*g, descriptor, scale, o, plane, resident);
    if (auto g = openvdb::gridPtrCast<openvdb::BoolGrid>(grid))
      return build(*g, descriptor, scale, o, plane, resident);
    if (auto g = openvdb::gridPtrCast<openvdb::MaskGrid>(grid))
      return build(*g, descriptor, scale, o, plane, resident);
    if (auto g = openvdb::gridPtrCast<openvdb::Int32Grid>(grid))
      return build(*g, descriptor, scale, o, plane, resident);
    if (auto g = openvdb::gridPtrCast<openvdb::Int64Grid>(grid))
      return build(*g, descriptor, scale, o, plane, resident);
    return build(*openvdb::gridPtrCast<openvdb::Vec3IGrid>(grid), descriptor,
                 scale, o, plane, resident);
#endif
  });
#else
  (void)input;
  (void)selection;
  (void)scale;
  (void)o;
  throw std::runtime_error("OpenVDB slice preview is unavailable; configure "
                           "with -DENABLE_OPENVDB=ON.");
#endif
}
} // namespace container::geometry::vdb
