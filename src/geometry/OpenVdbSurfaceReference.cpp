#include "Container/geometry/OpenVdbSurfaceReference.h"

#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/tools/RayTracer.h>
#include <tinyexr.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <numbers>
#include <type_traits>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB surface reference images are unavailable; "
                           "configure ENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using V = std::array<double, 3>;
using NativeV = openvdb::Vec3d;
using Film = openvdb::tools::Film;
constexpr uint64_t Fixed = 1024 * 1024;
constexpr uint64_t MaxBytes = 16ull << 30;
uint64_t add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX - a)
    throw std::invalid_argument("surface image byte/work integer overflow");
  return a + b;
}
uint64_t mul(uint64_t a, uint64_t b) {
  if (a && b > UINT64_MAX / a)
    throw std::invalid_argument("surface image byte/work integer overflow");
  return a * b;
}
void budget(uint64_t amount, uint64_t limit, const char *why) {
  if (amount > limit)
    throw std::invalid_argument(why);
}
bool finite(const V &v) {
  return std::all_of(v.begin(), v.end(),
                     [](double x) { return std::isfinite(x); });
}
V cross(const V &a, const V &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}
V unit(V v) {
  if (!finite(v))
    throw std::invalid_argument("surface camera vector is nonfinite");
  const double peak =
      std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])});
  if (!peak)
    throw std::invalid_argument("surface camera basis is degenerate");
  const V original = v;
  for (size_t k = 0; k < 3; ++k) {
    v[k] /= peak;
    if (original[k] != 0 && v[k] == 0)
      throw std::invalid_argument(
          "surface camera vector loses nonzero dynamic range");
  }
  const double length = std::hypot(v[0], v[1], v[2]);
  for (size_t k = 0; k < 3; ++k) {
    v[k] /= length;
    if (original[k] != 0 && v[k] == 0)
      throw std::invalid_argument("surface camera unit vector underflows");
  }
  return v;
}
NativeV native(const V &v) { return {v[0], v[1], v[2]}; }
V array(const NativeV &v) { return {v[0], v[1], v[2]}; }
float checkedFloat(double value) {
  if (!std::isfinite(value) ||
      std::abs(value) > std::numeric_limits<float>::max())
    throw std::invalid_argument(
        "surface reference Float conversion overflow/nonfinite value");
  const float f = static_cast<float>(value);
  if (value != 0 && f == 0)
    throw std::invalid_argument(
        "surface reference nonzero Float conversion underflow");
  return f;
}
Film::RGBA color(const V &v) {
  for (const double x : v)
    if (!std::isfinite(x) || x < 0 || x > 1)
      throw std::invalid_argument(
          "surface reference colors must be finite within[0,1]");
  return {checkedFloat(v[0]), checkedFloat(v[1]), checkedFloat(v[2])};
}
struct Basis {
  V forward, right, up;
  double aspect{}, scale{};
  Basis(const SurfaceReferenceCamera &c, uint32_t width, uint32_t height) {
    if (!finite(c.eye) || !finite(c.target) || !finite(c.up) ||
        !std::isfinite(c.minimumDistance) ||
        !std::isfinite(c.maximumDistance) || c.minimumDistance < 0 ||
        c.maximumDistance <= c.minimumDistance)
      throw std::invalid_argument(
          "surface reference camera pose/radial clipping is invalid");
    forward = unit({c.target[0] - c.eye[0], c.target[1] - c.eye[1],
                    c.target[2] - c.eye[2]});
    const auto upHint = unit(c.up);
    const auto rawRight = cross(forward, upHint);
    if (std::hypot(rawRight[0], rawRight[1], rawRight[2]) < 1e-8)
      throw std::invalid_argument(
          "surface reference up is parallel to view direction");
    right = unit(rawRight);
    up = unit(cross(right, forward));
    aspect = double(width) / height;
    if (c.projection == SurfaceProjection::Perspective) {
      if (!std::isfinite(c.verticalFovDegrees) || c.verticalFovDegrees < .1 ||
          c.verticalFovDegrees > 150)
        throw std::invalid_argument(
            "surface reference perspective FOV must be within[0.1,150]");
      scale = std::tan(c.verticalFovDegrees * std::numbers::pi / 360);
    } else if (c.projection == SurfaceProjection::Orthographic) {
      if (!std::isfinite(c.orthographicHeight) || c.orthographicHeight <= 0)
        throw std::invalid_argument(
            "surface reference orthographic height must be finite positive");
      scale = .5 * c.orthographicHeight;
      if (scale == 0)
        throw std::invalid_argument(
            "surface orthographic half height underflows");
    } else
      throw std::invalid_argument("unknown surface reference projection");
    if (!std::isfinite(2 * (scale * aspect)) || scale * aspect == 0)
      throw std::invalid_argument(
          "surface camera aspect/scale overflows or underflows");
  }
};
uint64_t dimensions(const SurfaceReferenceOptions &o) {
  const auto count = mul(o.width, o.height);
  if (!o.width || !o.height || o.width > 2048 || o.height > 2048 ||
      !o.maxPixels || o.maxPixels > 262144 || count > o.maxPixels ||
      !o.rays.limits.maxRays || o.rays.limits.maxRays > 1'000'000 ||
      count > o.rays.limits.maxRays || !o.rays.limits.maxWorkItems ||
      o.rays.limits.maxWorkItems > 10'000'000'000ull ||
      (o.shading != SurfaceShading::Matte &&
       o.shading != SurfaceShading::WorldNormal &&
       o.shading != SurfaceShading::TwoSidedHeadlight))
    throw std::invalid_argument(
        "surface reference dimensions/shading/pixel/ray/work limits are "
        "invalid");
  return count;
}
// Knuth TwoSum records the lost WORLD translation rather than silently treating
// the rounded origin as the requested orthographic sampling position.
double translated(double eye, double offset, double &error) {
  const double sum = eye + offset;
  if (!std::isfinite(sum))
    throw std::invalid_argument("surface camera WORLD origin overflow");
  const double z = sum - eye;
  const double lost = (eye - (sum - z)) + (offset - z);
  if (!std::isfinite(lost))
    throw std::invalid_argument(
        "surface camera WORLD origin arithmetic is unsafe");
  error = std::max(error, std::abs(lost));
  return sum;
}
void closeVector(const V &actual, const V &expected) {
  for (size_t k = 0; k < 3; ++k)
    if (!std::isfinite(actual[k]) ||
        std::abs(actual[k] - expected[k]) >
            2e-12 * std::max(1., std::abs(expected[k])))
      throw std::runtime_error(
          "native camera failed the intended independent pose/ray check");
}
uint64_t metadataBytes(const RayQuerySource &s, uint64_t limit) {
  uint64_t bytes = sizeof(s);
  for (const auto *v : {&s.descriptorName, &s.gridName, &s.valueType})
    bytes = add(bytes, add(v->capacity(), 1));
  for (const auto *items : {&s.metadata, &s.fileMetadata}) {
    bytes = add(bytes, mul(items->capacity(), sizeof(RayQueryMetadata)));
    budget(bytes, limit, "surface EXR metadata catalog exceeds byte cap");
    for (const auto &m : *items)
      for (const auto *v : {&m.name, &m.type, &m.value})
        bytes = add(bytes, add(v->capacity(), 1));
  }
  budget(bytes, limit, "surface EXR metadata values exceed byte cap");
  return bytes;
}
uint64_t retainedBytes(const SurfaceReferenceImage &image, uint64_t limit) {
  uint64_t result =
      add(sizeof(image), metadataBytes(image.geometry.source, limit));
  result = add(result, mul(image.geometry.rays.capacity(), sizeof(SdfRayHit)));
  result =
      add(result, mul(image.rgba.capacity(), sizeof(std::array<double, 4>)));
  result = add(result,
               mul(image.geometry.diagnostics.capacity(), sizeof(std::string)));
  for (const auto &d : image.geometry.diagnostics)
    result = add(result, add(d.capacity(), 1));
  return result;
}
double product(double a, double b) {
  const double v = a * b;
  if (!std::isfinite(v) || (a != 0 && b != 0 && v == 0))
    throw std::invalid_argument(
        "surface shader meaningful Double product range loss");
  return v;
}
double shaderCosine(const V &a, const V &b) {
  // Native dot arithmetic is part of the reference shader definition. Check
  // each nonzero term before allowing Float shading to hide range loss.
  const double x = product(a[0], b[0]), y = product(a[1], b[1]),
               z = product(a[2], b[2]);
  return std::abs(x + y + z);
}
void auditColor(SurfaceReferenceImage &out, const Film::RGBA &value,
                const V &expected, uint64_t index, bool hit) {
  const V actual{value.r, value.g, value.b};
  for (size_t k = 0; k < 3; ++k) {
    if (!std::isfinite(expected[k]) || expected[k] < 0 ||
        !std::isfinite(actual[k]) || actual[k] < 0 ||
        (expected[k] != 0 && actual[k] == 0))
      throw std::invalid_argument(
          "surface native Float shader overflow/nonzero underflow");
    if (actual[k] != expected[k]) {
      ++out.roundedColorValues;
      out.maximumColorAbsoluteError = std::max(
          out.maximumColorAbsoluteError, std::abs(actual[k] - expected[k]));
    }
    out.rgba[size_t(index)][k] = actual[k];
  }
  out.rgba[size_t(index)][3] = hit ? 1 : 0;
}
struct Text {
  std::string value;
  Text() { value.reserve(16384); }
  void append(std::string_view s) {
    if (s.size() > value.capacity() - value.size())
      throw std::invalid_argument(
          "surface EXR metadata exceeds bounded capacity");
    value.append(s);
  }
  template <class T> void number(T v) {
    char b[128];
    std::to_chars_result r;
    if constexpr (std::is_floating_point_v<T>)
      r = std::to_chars(b, b + sizeof(b), v, std::chars_format::general,
                        std::numeric_limits<T>::max_digits10);
    else
      r = std::to_chars(b, b + sizeof(b), v);
    if (r.ec != std::errc{})
      throw std::runtime_error(
          "surface EXR numeric metadata formatting failed");
    append({b, size_t(r.ptr - b)});
  }
  void vector(const V &v) {
    for (size_t k = 0; k < 3; ++k) {
      if (k)
        append(",");
      number(v[k]);
    }
  }
  void bytes(std::string_view v) {
    constexpr char hex[] = "0123456789abcdef";
    append("hex-prefix:");
    for (unsigned char c : v.substr(0, 256)) {
      char b[]{hex[c >> 4], hex[c & 15]};
      append({b, 2});
    }
    append("; byte_count=");
    number(v.size());
  }
};
#endif
} // namespace

SurfaceReferenceImage RenderSurfaceReference(
    const std::filesystem::path &input, const SurfaceReferenceCamera &camera,
    const SurfaceReferenceOptions &options, const std::string &grid,
    const ProcessingOptions &processing) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  const uint64_t count = dimensions(options);
  const Basis basis(camera, options.width, options.height);
  const auto material = color(options.color),
             background = color(options.background),
             invalid = color(options.invalidNormalColor);
  const uint64_t caller =
      add(mul(input.native().size(), sizeof(std::filesystem::path::value_type)),
          grid.size());
  if (caller > Fixed || !processing.maxInputBytes ||
      processing.maxInputBytes > MaxBytes)
    throw std::invalid_argument(
        "surface reference input/selector/shared byte limits are invalid");
  const uint64_t fixed = add(Fixed, mul(caller, 16));
  uint64_t wrapperBytes =
      add(fixed, mul(count, sizeof(Film::RGBA) + sizeof(WorldRay) +
                                sizeof(std::array<double, 4>)));
  const uint64_t prep = add(add(4096, mul(caller, 16)), mul(count, 1024));
  if (wrapperBytes >= processing.maxInputBytes ||
      prep >= options.rays.limits.maxWorkItems)
    throw std::invalid_argument("surface image/camera/shader reservation "
                                "exceeds shared byte/work caps");
  std::vector<WorldRay> rays;
  rays.reserve(size_t(count));
  SurfaceReferenceImage out;
  out.width = options.width;
  out.height = options.height;
  out.camera = camera;
  out.options = options;
  out.rgba.resize(size_t(count));
  wrapperBytes = add(
      fixed, add(mul(count, sizeof(Film::RGBA)),
                 add(mul(rays.capacity(), sizeof(WorldRay)),
                     mul(out.rgba.capacity(), sizeof(std::array<double, 4>)))));
  if (wrapperBytes >= processing.maxInputBytes)
    throw std::invalid_argument(
        "surface image actual capacities exceed shared byte cap");
  Film film(options.width, options.height, background);
  std::unique_ptr<openvdb::tools::BaseCamera> nativeCamera;
  if (camera.projection == SurfaceProjection::Perspective)
    nativeCamera = std::make_unique<openvdb::tools::PerspectiveCamera>(
        film, NativeV(0), NativeV(0), 1., 2 * basis.aspect * basis.scale, 1.,
        2.);
  else
    nativeCamera = std::make_unique<openvdb::tools::OrthographicCamera>(
        film, NativeV(0), NativeV(0), 2 * basis.aspect * basis.scale, 1., 2.);
  nativeCamera->lookAt(native(basis.forward), native(unit(camera.up)));
  double originError = 0;
  for (uint32_t j = 0; j < options.height; ++j)
    for (uint32_t i = 0; i < options.width; ++i) {
      const auto nativeRay = nativeCamera->getRay(i, j, .5, .5);
      const double x = (2 * (double(i) + .5) / options.width - 1) *
                       basis.aspect * basis.scale;
      const double y =
          (1 - 2 * (double(j) + .5) / options.height) * basis.scale;
      V offset{}, expectedDirection = basis.forward;
      if (camera.projection == SurfaceProjection::Perspective)
        expectedDirection =
            unit({basis.forward[0] + x * basis.right[0] + y * basis.up[0],
                  basis.forward[1] + x * basis.right[1] + y * basis.up[1],
                  basis.forward[2] + x * basis.right[2] + y * basis.up[2]});
      else
        for (size_t k = 0; k < 3; ++k)
          offset[k] = x * basis.right[k] + y * basis.up[k];
      closeVector(array(nativeRay.eye()), offset);
      closeVector(array(nativeRay.dir()), expectedDirection);
      WorldRay ray;
      ray.direction = array(nativeRay.dir());
      ray.minimumDistance = camera.minimumDistance;
      ray.maximumDistance = camera.maximumDistance;
      for (size_t k = 0; k < 3; ++k)
        ray.origin[k] =
            translated(camera.eye[k], nativeRay.eye()[k], originError);
      rays.push_back(ray);
    }
  // Do not copy a possibly huge irrelevant generation gridName or revive
  // invalid hidden voxelSize/halfWidth/triangle settings in this read-only
  // route.
  ProcessingOptions localProcessing;
  localProcessing.maxInputBytes = processing.maxInputBytes - wrapperBytes;
  localProcessing.maxVoxelCount = processing.maxVoxelCount;
  auto localRays = options.rays;
  localRays.limits.maxWorkItems -= prep;
  out.geometry =
      QueryLevelSetRays(input, rays, localRays, grid, localProcessing);
  if (out.geometry.rays.size() != count)
    throw std::runtime_error(
        "surface native query returned an inconsistent ray count");
  const auto &map = out.geometry.source.indexToWorld;
  const double h = std::hypot(map[0], map[4], map[8]);
  if (!std::isfinite(h) || h <= 0 || originError > h * 1e-4)
    throw std::invalid_argument(
        "surface camera translation loses more than1e-4 source voxel");
  openvdb::tools::MatteShader<> matte(material);
  openvdb::tools::NormalShader<> normal(material);
  openvdb::tools::DiffuseShader<> headlight(material);
  for (uint64_t index = 0; index < count; ++index) {
    const auto &hit = out.geometry.rays[size_t(index)];
    const bool present = hit.kind != SdfRayHitKind::Miss;
    Film::RGBA shaded = background;
    V expected = options.background;
    if (present) {
      ++out.hitCount;
      if (hit.normalStatus != RayNormalStatus::Available)
        ++out.unavailableNormalCount;
      if (options.shading == SurfaceShading::Matte) {
        shaded = matte(native(hit.position), NativeV(0),
                       native(hit.normalizedDirection));
        expected = options.color;
      } else if (hit.normalStatus != RayNormalStatus::Available) {
        shaded = invalid;
        expected = options.invalidNormalColor;
      } else if (options.shading == SurfaceShading::WorldNormal) {
        shaded = normal(native(hit.position), native(hit.normal),
                        native(hit.normalizedDirection));
        for (size_t k = 0; k < 3; ++k)
          expected[k] = product(options.color[k], .5 * (hit.normal[k] + 1));
      } else {
        const double cosine = shaderCosine(hit.normal, hit.normalizedDirection);
        for (size_t k = 0; k < 3; ++k)
          expected[k] = product(options.color[k], cosine);
        shaded = headlight(native(hit.position), native(hit.normal),
                           native(hit.normalizedDirection));
      }
    }
    auditColor(out, shaded, expected, index, present);
  }
  out.preparationWork = prep;
  out.work = add(prep, out.geometry.source.workItems);
  out.maxImageBytes = processing.maxInputBytes;
  static_assert(std::is_nothrow_move_constructible_v<SurfaceReferenceImage>);
  return out;
#else
  (void)input;
  (void)camera;
  (void)options;
  (void)grid;
  (void)processing;
  unavailable();
#endif
}

SurfaceReferenceExportResult
WriteSurfaceReferenceExr(const std::filesystem::path &output,
                         const SurfaceReferenceImage &image,
                         uint64_t maxBytes) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  const uint64_t count = dimensions(image.options);
  if (output.empty() || image.width != image.options.width ||
      image.height != image.options.height || image.rgba.size() != count ||
      image.geometry.rays.size() != count || !image.maxImageBytes ||
      image.maxImageBytes > MaxBytes || !maxBytes || maxBytes > MaxBytes)
    throw std::invalid_argument(
        "surface EXR dimensions/result/byte limits are invalid");
  const Basis basis(image.camera, image.width, image.height);
  (void)basis;
  (void)color(image.options.color);
  (void)color(image.options.background);
  (void)color(image.options.invalidNormalColor);
  if (!std::isfinite(image.options.rays.isovalue) ||
      image.options.rays.refinementSteps > 8 ||
      image.work !=
          add(image.preparationWork, image.geometry.source.workItems) ||
      image.work > image.options.rays.limits.maxWorkItems ||
      !std::isfinite(image.maximumColorAbsoluteError) ||
      image.maximumColorAbsoluteError < 0)
    throw std::invalid_argument(
        "surface EXR method/work/color diagnostics are invalid");
  const auto &matrix = image.geometry.source.indexToWorld;
  if (!std::all_of(matrix.begin(), matrix.end(),
                   [](double x) { return std::isfinite(x); }) ||
      matrix[12] != 0 || matrix[13] != 0 || matrix[14] != 0 || matrix[15] != 1)
    throw std::invalid_argument("surface EXR source map metadata is invalid");
  const uint64_t pathBytes =
      mul(output.native().size(), sizeof(std::filesystem::path::value_type));
  if (pathBytes > Fixed)
    throw std::invalid_argument(
        "surface EXR output path exceeds bounded workspace");
  const uint64_t limit = std::min(maxBytes, image.maxImageBytes);
  const uint64_t retained =
      add(retainedBytes(image, limit), add(Fixed, mul(pathBytes, 16)));
  const uint64_t reserve = add(retained, mul(count, 11 * 4 * 8));
  budget(reserve, limit,
         "surface EXR result/planar/encoded/workspace exceeds byte cap");
  if (std::filesystem::exists(output))
    throw std::invalid_argument("surface EXR output already exists");
  std::array<std::vector<float>, 11> channels;
  for (auto &channel : channels)
    channel.resize(size_t(count));
  SurfaceReferenceExportResult converted;
  uint64_t hits = 0, missing = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto &h = image.geometry.rays[i];
    const auto kind = static_cast<unsigned>(h.kind),
               status = static_cast<unsigned>(h.normalStatus);
    if (kind > 2 || status > 4)
      throw std::invalid_argument("surface EXR unknown hit/normal status");
    const bool hit = h.kind != SdfRayHitKind::Miss,
               normal = h.normalStatus == RayNormalStatus::Available;
    const bool requested = image.options.rays.computeNormals;
    const bool validStatus =
        !requested ? h.normalStatus == RayNormalStatus::NotRequested
        : !hit
            ? h.normalStatus == RayNormalStatus::NoHit
            : h.normalStatus == RayNormalStatus::Available ||
                  h.normalStatus == RayNormalStatus::MissingActiveFootprint ||
                  h.normalStatus == RayNormalStatus::DegenerateGradient;
    if (!validStatus)
      throw std::invalid_argument(
          "surface EXR normal status contradicts hit kind/requested normals");
    if (hit) {
      ++hits;
      if (!normal)
        ++missing;
    }
    if (image.rgba[i][3] != (hit ? 1. : 0.))
      throw std::invalid_argument(
          "surface EXR coverage disagrees with hit status");
    if (hit && (!std::isfinite(h.distance) ||
                h.distance < image.camera.minimumDistance ||
                h.distance > image.camera.maximumDistance ||
                !finite(h.position) || !std::isfinite(h.sampledResidual)))
      throw std::invalid_argument("surface EXR hit geometry is invalid");
    if (normal && (!finite(h.normal) ||
                   std::abs(std::hypot(h.normal[0], h.normal[1], h.normal[2]) -
                            1) > 1e-6))
      throw std::invalid_argument(
          "surface EXR available normal is not finite/unit");
    // Lexicographically sorted names; all unavailable numeric AOVs are explicit
    // zero placeholders paired with independently retained status channels.
    const std::array<double, 11> values{image.rgba[i][3],
                                        image.rgba[i][2],
                                        double(kind),
                                        normal ? 1. : 0.,
                                        double(status),
                                        normal ? h.normal[0] : 0.,
                                        normal ? h.normal[1] : 0.,
                                        normal ? h.normal[2] : 0.,
                                        image.rgba[i][1],
                                        image.rgba[i][0],
                                        hit ? h.distance : 0.};
    for (size_t c = 0; c < values.size(); ++c) {
      const double v = values[c];
      if ((c == 1 || c == 8 || c == 9) && (v < 0 || v > 1.000001))
        throw std::invalid_argument(
            "surface EXR RGB is outside the reference response range");
      const float f = checkedFloat(v);
      channels[c][i] = f;
      if (double(f) != v) {
        ++converted.roundedValues;
        const double error = std::abs(double(f) - v);
        converted.maximumAbsoluteError =
            std::max(converted.maximumAbsoluteError, error);
        if (v != 0)
          converted.maximumRelativeError =
              std::max(converted.maximumRelativeError, error / std::abs(v));
      }
    }
  }
  if (hits != image.hitCount || missing != image.unavailableNormalCount)
    throw std::invalid_argument(
        "surface EXR hit/normal counters are inconsistent");
  Text text;
  text.append("schema=1; response=native Float surface reference,not physical "
              "radiance; center_pixel_rays=1; row0=top; distances=Euclidean "
              "WORLD; no_AA_or_SE=1; approximate_native_HDDA_chord=1; "
              "tangent_or_multiple_roots_can_be_missed=1; "
              "sampled_zero_includes_plateaus=1; "
              "invalid_AOV_zero_placeholders_require_masks=1; "
              "N.STATUS=0:not-requested,1:no-hit,2:available,3:missing-active-"
              "footprint,4:degenerate-gradient; "
              "HIT.KIND=0:miss,1:sampled-zero,2:bracketed-crossing; width=");
  text.number(image.width);
  text.append("; height=");
  text.number(image.height);
  text.append("; projection=");
  text.append(image.camera.projection == SurfaceProjection::Perspective
                  ? "perspective"
                  : "orthographic");
  text.append("; shading=");
  text.append(image.options.shading == SurfaceShading::Matte ? "matte"
              : image.options.shading == SurfaceShading::WorldNormal
                  ? "WORLD normal"
                  : "two-sided headlight abs(N dot rayD)");
  text.append("; eye=");
  text.vector(image.camera.eye);
  text.append("; target=");
  text.vector(image.camera.target);
  text.append("; up=");
  text.vector(image.camera.up);
  text.append("; vertical_fov=");
  text.number(image.camera.verticalFovDegrees);
  text.append("; orthographic_height=");
  text.number(image.camera.orthographicHeight);
  text.append("; minimum_distance=");
  text.number(image.camera.minimumDistance);
  text.append("; maximum_distance=");
  text.number(image.camera.maximumDistance);
  text.append("; color=");
  text.vector(image.options.color);
  text.append("; background=");
  text.vector(image.options.background);
  text.append("; invalid_normal_color=");
  text.vector(image.options.invalidNormalColor);
  text.append("; hit_count=");
  text.number(hits);
  text.append("; unavailable_normal_count=");
  text.number(missing);
  text.append("; isovalue=");
  text.number(image.options.rays.isovalue);
  text.append("; native_refinement_steps=");
  text.number(image.options.rays.refinementSteps);
  text.append("; source_descriptor=");
  text.bytes(image.geometry.source.descriptorName);
  text.append("; source_name=");
  text.bytes(image.geometry.source.gridName);
  text.append("; source_type=");
  text.bytes(image.geometry.source.valueType);
  text.append("; index_to_world_rows=");
  for (size_t k = 0; k < 16; ++k) {
    if (k)
      text.append(",");
    text.number(image.geometry.source.indexToWorld[k]);
  }
  text.append("; work=");
  text.number(image.work);
  text.append("; rounded_shader_values=");
  text.number(image.roundedColorValues);
  text.append("; maximum_shader_absolute_error=");
  text.number(image.maximumColorAbsoluteError);
  text.append("; rounded_export_values=");
  text.number(converted.roundedValues);
  text.append("; maximum_export_absolute_error=");
  text.number(converted.maximumAbsoluteError);
  EXRImage exr;
  InitEXRImage(&exr);
  exr.num_channels = 11;
  exr.width = int(image.width);
  exr.height = int(image.height);
  std::array<unsigned char *, 11> pointers;
  for (size_t c = 0; c < 11; ++c)
    pointers[c] = reinterpret_cast<unsigned char *>(channels[c].data());
  exr.images = pointers.data();
  EXRHeader header;
  InitEXRHeader(&header);
  header.num_channels = 11;
  header.compression_type = TINYEXR_COMPRESSIONTYPE_NONE;
  constexpr std::array<const char *, 11> names{
      "A",   "B",   "HIT.KIND", "N.VALID", "N.STATUS", "N.X",
      "N.Y", "N.Z", "G",        "R",       "Z"};
  // TinyEXR accepts any order, but EXR convention requires true lexical order.
  constexpr std::array<size_t, 11> order{0, 1, 8, 2, 4, 3, 5, 6, 7, 9, 10};
  std::array<EXRChannelInfo, 11> info{};
  std::array<int, 11> types;
  types.fill(TINYEXR_PIXELTYPE_FLOAT);
  std::array<unsigned char *, 11> sorted;
  for (size_t c = 0; c < 11; ++c) {
    std::strncpy(info[c].name, names[order[c]], sizeof(info[c].name) - 1);
    sorted[c] = pointers[order[c]];
  }
  exr.images = sorted.data();
  header.channels = info.data();
  header.pixel_types = types.data();
  header.requested_pixel_types = types.data();
  EXRAttribute attribute{};
  std::strcpy(attribute.name, "container.surface_reference");
  std::strcpy(attribute.type, "string");
  attribute.value = reinterpret_cast<unsigned char *>(text.value.data());
  attribute.size = int(text.value.size());
  header.num_custom_attributes = 1;
  header.custom_attributes = &attribute;
  unsigned char *encoded = nullptr;
  const char *error = nullptr;
  struct Cleanup {
    unsigned char *&data;
    const char *&error;
    ~Cleanup() {
      std::free(data);
      if (error)
        FreeEXRErrorMessage(error);
    }
  } cleanup{encoded, error};
  const auto size = SaveEXRImageToMemory(&exr, &header, &encoded, &error);
  if (!size || !encoded)
    throw std::runtime_error(std::string("surface EXR serialization failed: ") +
                             (error ? error : "unknown error"));
  budget(add(add(retained, mul(count, 11 * 4)), size), limit,
         "surface encoded EXR exceeds shared byte cap");
  if (size > size_t(std::numeric_limits<std::streamsize>::max()))
    throw std::invalid_argument("surface encoded EXR exceeds stream size");
  std::ofstream file;
  bool owned = false;
  try {
    file.open(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    owned = file.is_open();
    if (!owned)
      throw std::runtime_error(
          "could not exclusively create surface reference EXR");
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.write(reinterpret_cast<const char *>(encoded), std::streamsize(size));
    file.flush();
    file.close();
  } catch (...) {
    file.exceptions(std::ios::goodbit);
    if (file.is_open())
      file.close();
    if (owned) {
      std::error_code ignored;
      std::filesystem::remove(output, ignored);
    }
    throw;
  }
  converted.encodedBytes = size;
  return converted;
#else
  (void)output;
  (void)image;
  (void)maxBytes;
  unavailable();
#endif
}

} // namespace container::geometry::vdb
