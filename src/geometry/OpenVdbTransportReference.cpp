#include "Container/geometry/OpenVdbTransportReference.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace container::geometry::vdb {
namespace {
using V = TransportVector;
constexpr double pi = std::numbers::pi_v<double>;
constexpr double infinity = std::numeric_limits<double>::infinity();

V add(V a, const V &b) {
  for (int k = 0; k < 3; ++k)
    a[k] += b[k];
  return a;
}
V sub(V a, const V &b) {
  for (int k = 0; k < 3; ++k)
    a[k] -= b[k];
  return a;
}
V mul(V a, double b) {
  for (double &x : a)
    x *= b;
  return a;
}
V product(V a, const V &b) {
  for (int k = 0; k < 3; ++k) {
    const double value = a[k] * b[k];
    if (a[k] != 0 && b[k] != 0 && value == 0)
      throw std::runtime_error("transport radiometric dynamic range underflows; rescale the scene");
    a[k] = value;
  }
  return a;
}
double dot(const V &a, const V &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V &a, const V &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const V &a) { return std::hypot(a[0], a[1], a[2]); }
double peak(const V &a) { return std::max({a[0], a[1], a[2]}); }
double radiometricProduct(double a, double b) {
  const double value = a * b;
  if (a != 0 && b != 0 && value == 0)
    throw std::runtime_error("transport radiometric dynamic range underflows; rescale the scene");
  if (!std::isfinite(value))
    throw std::runtime_error("transport radiometric dynamic range overflows; rescale the scene");
  return value;
}
bool finite(const V &a) {
  return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]);
}
V unit(const V &a) {
  const double n = norm(a);
  if (!finite(a) || !std::isfinite(n) || n == 0)
    throw std::invalid_argument("transport ray/light direction must be finite and nonzero");
  V result = a;
  for (double &value : result)
    value /= n;
  return result;
}
void position(const V &v) {
  for (double x : v)
    if (!std::isfinite(x) || std::abs(x) > 1e12)
      throw std::invalid_argument(
          "transport coordinates must be finite with magnitude at most 1e12");
}
void color(const V &v, double maximum = 1e6) {
  for (double x : v)
    if (!std::isfinite(x) || x < 0 || x > maximum)
      throw std::invalid_argument(
          "transport colors/coefficients exceed their finite nonnegative range");
}
double spacing(double x) {
  return std::max(std::nextafter(x, infinity) - x, x - std::nextafter(x, -infinity));
}
void checkedAdd(V &sum, const V &contribution) {
  if (!finite(contribution))
    throw std::runtime_error("transport radiance overflow; rescale the scene");
  sum = add(sum, contribution);
  if (!finite(sum))
    throw std::runtime_error("transport radiance overflow; rescale the scene");
}

struct Random {
  uint64_t state;
  uint64_t bits() {
    uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }
  double uniform() { return (double(bits() >> 12) + .5) * 0x1p-52; }
};

struct Moment {
  double scale = 0, mean = 0, m2 = 0;
  void add(double value, uint32_t count) {
    if (value > scale) {
      const double ratio = scale / value;
      mean *= ratio;
      m2 *= ratio * ratio;
      scale = value;
    }
    const double normalized = scale == 0 ? 0 : value / scale;
    const double delta = normalized - mean;
    mean += delta / count;
    m2 += delta * (normalized - mean);
    if (!std::isfinite(mean) || !std::isfinite(m2))
      throw std::runtime_error("transport normalized estimator moments are not finite");
  }
  double radiance() const { return radiometricProduct(mean, scale); }
  double error(uint32_t count) const {
    if (count < 2)
      return 0;
    return radiometricProduct(std::sqrt(std::max(0.0, m2) / double(count - 1) / count), scale);
  }
};

double phase(double cosine, double g) {
  const double d = 1 + g * g - 2 * g * std::clamp(cosine, -1.0, 1.0);
  return (1 - g * g) / (4 * pi * d * std::sqrt(d));
}
V samplePhase(const V &current, double g, Random &random) {
  const double u = random.uniform();
  // Algebraically factor the usual HG inverse numerator by 2g. This exact
  // form has a stable g=0 limit and needs neither a small-g approximation nor
  // a PDF correction for an approximate proposal.
  const double t = 2 * u - 1, denominator = 1 + g * t;
  double cosine = (t + .5 * g * (t * t + 3) + g * g * t + .5 * g * g * g * (t * t - 1)) /
                  (denominator * denominator);
  cosine = std::clamp(cosine, -1.0, 1.0);
  const double sine = std::sqrt(std::max(0.0, 1 - cosine * cosine));
  const double phi = 2 * pi * random.uniform();
  const V tangent = unit(cross(std::abs(current[2]) < .9 ? V{0, 0, 1} : V{0, 1, 0}, current));
  const V bitangent = cross(current, tangent);
  return unit(add(mul(current, cosine),
                  add(mul(tangent, sine * std::cos(phi)), mul(bitangent, sine * std::sin(phi)))));
}

struct Prepared {
  const TransportScene &scene;
  const TransportReferenceOptions &options;
  TransportReferenceResult result;
  V minimum{infinity, infinity, infinity}, maximum{-infinity, -infinity, -infinity};
  double majorant = 0, minSpacing = infinity;
  bool hasMedium = false;

  void work() {
    if (result.work == options.maxWork)
      throw std::runtime_error("transport work budget exceeded; no partial result returned");
    ++result.work;
  }
  bool box(const V &origin, const V &direction, double limit, double &enter, double &leave) const {
    if (!hasMedium)
      return false;
    enter = 0;
    leave = limit;
    for (int k = 0; k < 3; ++k) {
      if (direction[k] == 0) {
        if (origin[k] < minimum[k] || origin[k] > maximum[k])
          return false;
      } else {
        double a = (minimum[k] - origin[k]) / direction[k],
               b = (maximum[k] - origin[k]) / direction[k];
        if (a > b)
          std::swap(a, b);
        enter = std::max(enter, a);
        leave = std::min(leave, b);
        if (leave <= enter)
          return false;
      }
    }
    return leave > enter;
  }
  double geometry(const V &origin, const V &direction, double limit) {
    double closest = limit;
    for (const auto &triangle : scene.occluders) {
      work();
      const V e1 = sub(triangle.b, triangle.a), e2 = sub(triangle.c, triangle.a);
      const V p = cross(direction, e2);
      const double determinant = dot(e1, p);
      if (determinant == 0)
        continue;
      const double inverse = 1 / determinant;
      if (!std::isfinite(inverse))
        throw std::runtime_error("transport triangle intersection precision is unsupported");
      const V s = sub(origin, triangle.a);
      const double u = dot(s, p) * inverse;
      if (u < 0 || u > 1)
        continue;
      const V q = cross(s, e1);
      const double v = dot(direction, q) * inverse;
      if (v < 0 || u + v > 1)
        continue;
      const double t = dot(e2, q) * inverse;
      if (t >= 0 && t < closest)
        closest = t;
    }
    return closest;
  }
  double density(const TransportMedium &medium, const V &p) {
    work();
    std::array<uint32_t, 3> low{}, high{};
    V fraction{};
    for (int k = 0; k < 3; ++k) {
      if (p[k] < medium.boundsMin[k] || p[k] > medium.boundsMax[k])
        return 0;
      const double index = (p[k] - medium.boundsMin[k]) /
                               (medium.boundsMax[k] - medium.boundsMin[k]) * medium.dimensions[k] -
                           .5;
      const double bounded = std::clamp(index, 0.0, double(medium.dimensions[k] - 1));
      low[k] = static_cast<uint32_t>(bounded);
      high[k] = std::min(low[k] + 1, medium.dimensions[k] - 1);
      fraction[k] = bounded - low[k];
    }
    double sum = 0;
    for (unsigned z = 0; z < 2; ++z)
      for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 2; ++x) {
          const size_t i =
              (z ? high[2] : low[2]) * size_t(medium.dimensions[1]) * medium.dimensions[0] +
              (y ? high[1] : low[1]) * size_t(medium.dimensions[0]) + (x ? high[0] : low[0]);
          sum += double(medium.density[i]) * (x ? fraction[0] : 1 - fraction[0]) *
                 (y ? fraction[1] : 1 - fraction[1]) * (z ? fraction[2] : 1 - fraction[2]);
        }
    return sum;
  }
  struct Coefficients {
    double extinction = 0;
    std::array<double, 8> media{};
    V emission{};
  };
  Coefficients coefficients(const V &p) {
    Coefficients c;
    for (size_t i = 0; i < scene.media.size(); ++i) {
      const auto &medium = scene.media[i];
      const double d = density(medium, p);
      c.media[i] = d * medium.extinction;
      if (d > 0 && medium.extinction > 0 && c.media[i] == 0)
        throw std::runtime_error(
            "transport extinction dynamic range underflows; rescale the scene");
      c.extinction += c.media[i];
      for (double source : medium.emission)
        if (d > 0 && source > 0 && d * source == 0)
          throw std::runtime_error(
              "transport emission dynamic range underflows; rescale the scene");
      c.emission = add(c.emission, mul(medium.emission, d));
    }
    // Majorant is built from the maximum stored density and positive material
    // coefficients; trilinear interpolation is a convex combination. Permit
    // only floating summation roundoff, never an invalid physical majorant.
    if (c.extinction > majorant * (1 + 16 * std::numeric_limits<double>::epsilon()))
      throw std::runtime_error("transport extinction exceeded its validated majorant");
    return c;
  }
  bool proposal(double &t, double end, Random &random) {
    work();
    const double optical = -std::log1p(-random.uniform());
    if (optical >= (end - t) * majorant)
      return false;
    const double next = t + optical / majorant;
    if (!(next > t) || !std::isfinite(next))
      throw std::runtime_error(
          "transport free-flight progress is below double precision; rescale the scene");
    t = next;
    return true;
  }
  double transmittance(const V &origin, const V &direction, double distance, Random &random) {
    if (geometry(origin, direction, distance) < distance)
      return 0;
    double enter, leave;
    if (majorant == 0 || !box(origin, direction, distance, enter, leave))
      return 1;
    double result = 1, t = enter;
    while (proposal(t, leave, random)) {
      ++this->result.shadowProposals;
      const auto c = coefficients(add(origin, mul(direction, t)));
      result = radiometricProduct(result, std::max(0.0, 1 - c.extinction / majorant));
      if (result == 0)
        break;
    }
    return result;
  }
  V direct(const V &p, const V &direction, double g, Random &random) {
    V value{};
    for (const auto &light : scene.lights) {
      work();
      V toward;
      double distance = infinity, falloff = 1;
      if (light.type == TransportLightType::Directional)
        toward = unit(light.vector);
      else {
        toward = sub(light.vector, p);
        distance = norm(toward);
        if (distance == 0)
          throw std::runtime_error("transport point light coincides with a scattering point");
        toward = mul(toward, 1 / distance);
        falloff = 1 / (distance * distance);
      }
      const double t = transmittance(p, toward, distance, random);
      if (t == 0)
        continue;
      const double weight =
          radiometricProduct(radiometricProduct(falloff, phase(dot(direction, toward), g)), t);
      V contribution = light.value;
      for (double &channel : contribution)
        channel = radiometricProduct(channel, weight);
      checkedAdd(value, contribution);
    }
    return value;
  }
  V trace(TransportRay ray, Random &random) {
    V value{}, throughput{1, 1, 1};
    uint32_t order = 0;
    while (true) {
      const double hit = geometry(ray.origin, ray.direction, infinity);
      double enter, leave;
      if (majorant == 0 || !box(ray.origin, ray.direction, hit, enter, leave)) {
        if (!std::isfinite(hit))
          checkedAdd(value, product(throughput, scene.environment));
        return value;
      }
      bool scattered = false;
      double t = enter;
      while (proposal(t, leave, random)) {
        const V p = add(ray.origin, mul(ray.direction, t));
        const auto c = coefficients(p);
        V source = c.emission;
        for (double &value : source) {
          const double divided = value / majorant;
          if (value > 0 && divided == 0)
            throw std::runtime_error("transport emission score dynamic range underflows; rescale "
                                     "the source/proposal rate");
          value = divided;
        }
        checkedAdd(value, product(throughput, source));
        if (random.uniform() * majorant >= c.extinction) {
          ++result.nullCollisions;
          continue;
        }
        double target = random.uniform() * c.extinction;
        size_t selected = scene.media.size() - 1;
        for (size_t i = 0; i < scene.media.size(); ++i) {
          target -= c.media[i];
          if (target < 0) {
            selected = i;
            break;
          }
        }
        const auto &medium = scene.media[selected];
        throughput = product(throughput, medium.albedo);
        if (peak(throughput) == 0)
          return value;
        if (order == options.maxScatteringEvents) {
          // Count only the selected medium's nonzero omitted scattering tail,
          // rather than counting a neighboring black absorber as truncated.
          ++result.truncatedPaths;
          return value;
        }
        ++order;
        ++result.scatteringEvents;
        checkedAdd(value, product(throughput, direct(p, ray.direction, medium.anisotropy, random)));
        if (order >= options.rouletteStart) {
          const double survival = std::clamp(peak(throughput), .05, .95);
          if (random.uniform() >= survival) {
            ++result.rouletteTerminations;
            return value;
          }
          throughput = mul(throughput, 1 / survival);
        }
        const auto nextDirection = samplePhase(ray.direction, medium.anisotropy, random);
        ++result.phaseSamples;
        result.meanScatteringCosine +=
            (dot(ray.direction, nextDirection) - result.meanScatteringCosine) /
            double(result.phaseSamples);
        ray = {p, nextDirection};
        scattered = true;
        break;
      }
      if (!scattered) {
        if (!std::isfinite(hit))
          checkedAdd(value, product(throughput, scene.environment));
        return value;
      }
    }
  }
};

Prepared prepare(const TransportScene &scene, std::span<const TransportRay> rays,
                 const TransportReferenceOptions &options) {
  if (options.samplesPerRay < 1 || options.samplesPerRay > 1'000'000 ||
      options.maxScatteringEvents > 64 || options.rouletteStart < 1 || options.rouletteStart > 65 ||
      options.maxWork < 1 || options.maxWork > 1'000'000'000 || options.maxDensityValues < 1 ||
      options.maxInputBytes < 1 || !std::isfinite(options.emissionSamplingRate) ||
      options.emissionSamplingRate < 1e-6 || options.emissionSamplingRate > 1e6)
    throw std::invalid_argument("invalid transport sampling/order/memory/work options");
  if (scene.media.size() > 8 || scene.lights.size() > 32 || scene.occluders.size() > 4096 ||
      rays.empty() || rays.size() > 4096)
    throw std::invalid_argument(
        "transport supports 8 media, 32 lights, 4096 triangles and 1..4096 rays");
  const uint64_t fixed = sizeof(TransportScene) + sizeof(Prepared) +
                         scene.media.size() * sizeof(TransportMedium) +
                         scene.lights.size() * sizeof(TransportLight) +
                         scene.occluders.size() * sizeof(TransportTriangle) +
                         rays.size() * (sizeof(TransportRay) + sizeof(TransportRayEstimate));
  if (fixed > options.maxInputBytes)
    throw std::invalid_argument("transport scene exceeds input-memory budget");
  uint64_t values = 0;
  Prepared prepared{scene, options};
  bool emission = false;
  color(scene.environment);
  for (const auto &medium : scene.media) {
    uint64_t count = 1;
    position(medium.boundsMin);
    position(medium.boundsMax);
    color(medium.albedo, 1);
    color(medium.emission);
    if (!std::isfinite(medium.extinction) || medium.extinction < 0 || medium.extinction > 1e6 ||
        !std::isfinite(medium.anisotropy) || std::abs(medium.anisotropy) > .95)
      throw std::invalid_argument("invalid transport extinction or HG anisotropy");
    for (int k = 0; k < 3; ++k) {
      if (medium.dimensions[k] < 1 || medium.dimensions[k] > 256 ||
          medium.boundsMax[k] <= medium.boundsMin[k])
        throw std::invalid_argument("invalid transport density dimensions/bounds");
      count *= medium.dimensions[k];
      const double step = (medium.boundsMax[k] - medium.boundsMin[k]) / medium.dimensions[k];
      if (std::max(spacing(medium.boundsMin[k]), spacing(medium.boundsMax[k])) > step / 1024)
        throw std::invalid_argument("transport density bounds have insufficient world precision");
      prepared.minSpacing = std::min(prepared.minSpacing, step);
    }
    if (count != medium.density.size())
      throw std::invalid_argument("transport density dimensions do not match payload");
    values += count;
    if (values > options.maxDensityValues || fixed + values * sizeof(float) > options.maxInputBytes)
      throw std::invalid_argument("transport density exceeds input-memory/voxel budget");
    double maximum = 0;
    for (float d : medium.density) {
      if (!std::isfinite(d) || d < 0 || d > 1e6f)
        throw std::invalid_argument("transport density must be finite and in [0,1e6]");
      maximum = std::max(maximum, double(d));
    }
    const double extinction = maximum * medium.extinction;
    if (maximum > 0 && medium.extinction > 0 && extinction == 0)
      throw std::invalid_argument("transport extinction dynamic range underflows");
    prepared.majorant += extinction;
    const bool source = maximum > 0 && peak(medium.emission) > 0;
    emission |= source;
    if (extinction > 0 || source) {
      prepared.hasMedium = true;
      for (int k = 0; k < 3; ++k) {
        prepared.minimum[k] = std::min(prepared.minimum[k], medium.boundsMin[k]);
        prepared.maximum[k] = std::max(prepared.maximum[k], medium.boundsMax[k]);
      }
    }
  }
  if (emission)
    prepared.majorant = std::max(prepared.majorant, options.emissionSamplingRate);
  for (const auto &light : scene.lights) {
    color(light.value);
    if (light.type == TransportLightType::Directional)
      (void)unit(light.vector);
    else if (light.type == TransportLightType::Point)
      position(light.vector);
    else
      throw std::invalid_argument("unknown transport light type");
  }
  for (const auto &triangle : scene.occluders) {
    position(triangle.a);
    position(triangle.b);
    position(triangle.c);
    if (norm(cross(sub(triangle.b, triangle.a), sub(triangle.c, triangle.a))) == 0)
      throw std::invalid_argument("degenerate transport occluder triangle");
  }
  for (const auto &ray : rays) {
    position(ray.origin);
    (void)unit(ray.direction);
    for (double x : ray.origin)
      if (prepared.hasMedium && spacing(x) > prepared.minSpacing / 1024)
        throw std::invalid_argument(
            "transport ray origin has insufficient precision for its density lattice");
  }
  return prepared;
}
} // namespace

TransportMedium MakeTransportMedium(const OpenVdbVolume &volume) {
  if (!volume.sparseDensity.empty() || !volume.sparseTemperature.empty())
    throw std::invalid_argument(
        "CPU transport reference requires an explicitly dense volume import");
  uint64_t count = 1;
  for (uint32_t dimension : volume.dimensions) {
    if (dimension < 1 || dimension > 256)
      throw std::invalid_argument("CPU transport dense adapter requires dimensions in [1,256]");
    count *= dimension;
  }
  if (count != volume.density.size())
    throw std::invalid_argument("CPU transport dense adapter dimensions do not match payload");
  TransportMedium medium;
  medium.dimensions = volume.dimensions;
  for (int k = 0; k < 3; ++k) {
    medium.boundsMin[k] = volume.boundsMin[k];
    medium.boundsMax[k] = volume.boundsMax[k];
  }
  medium.density = volume.density;
  return medium;
}

TransportReferenceResult TraceTransportReference(const TransportScene &scene,
                                                 std::span<const TransportRay> rays,
                                                 const TransportReferenceOptions &options) {
  auto p = prepare(scene, rays, options);
  p.result.rays.reserve(rays.size());
  for (size_t index = 0; index < rays.size(); ++index) {
    TransportRay ray{rays[index].origin, unit(rays[index].direction)};
    std::array<Moment, 3> moments;
    for (uint32_t sample = 0; sample < options.samplesPerRay; ++sample) {
      p.work();
      ++p.result.paths;
      Random random{options.seed ^ (uint64_t(index) * 0x632be59bd9b4e019ull) ^
                    (uint64_t(sample) * 0x8cb92baa3f3d8dd7ull)};
      const auto value = p.trace(ray, random);
      for (int k = 0; k < 3; ++k)
        moments[k].add(value[k], sample + 1);
    }
    V mean{}, error{};
    for (int k = 0; k < 3; ++k) {
      mean[k] = moments[k].radiance();
      error[k] = moments[k].error(options.samplesPerRay);
    }
    p.result.rays.push_back({mean, error});
  }
  return std::move(p.result);
}
} // namespace container::geometry::vdb
