#include "Container/common/CommonMath.h"
#include "Container/renderer/lighting/SkySettings.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace container::renderer {
namespace {

using glm::dvec3;
constexpr double pi = std::numbers::pi;

struct Medium {
  double radius, top;
  dvec3 rayleigh, mie, absorption;

  [[nodiscard]] dvec3 density(dvec3 p) const {
    const double h = std::max(glm::length(p) - radius, 0.0);
    return {std::exp(-h / 8), std::exp(-h / 1.2), std::max(1 - std::abs(h - 25) / 15, 0.0)};
  }
  [[nodiscard]] dvec3 extinction(dvec3 d) const {
    return rayleigh * d.x + mie * (0.004440 / 0.003996) * d.y + absorption * d.z;
  }
};

double groundHit(dvec3 p, dvec3 d, double radius) {
  const double b = glm::dot(p, d);
  const double discriminant = b * b + radius * radius - glm::dot(p, p);
  return b < 0 && discriminant >= 0 ? std::max(-b - std::sqrt(discriminant), 0.0) : -1.0;
}

double exitDistance(dvec3 p, dvec3 d, double radius) {
  const double b = glm::dot(p, d);
  return std::max(std::sqrt(std::max(b * b + radius * radius - glm::dot(p, p), 0.0)) - b, 0.0);
}

dvec3 solarTransmission(dvec3 p, dvec3 sun, const Medium& medium) {
  if (groundHit(p, sun, medium.radius) >= 0)
    return dvec3(0);
  const double distance = exitDistance(p, sun, medium.top);
  dvec3 column(0);
  double previous = 0;
  for (int i = 1; i <= 8; ++i) {
    const double u = i / 8.0;
    const double next = distance * u * u;
    column += medium.density(p + sun * ((previous + next) * 0.5)) * (next - previous);
    previous = next;
  }
  return glm::exp(-medium.extinction(column));
}

} // namespace

AtmosphereLut buildAtmosphereLut(const SkySettings& sky) {
  const auto& air = sky.atmosphere;
  const dvec3 groundAlbedo(sky.ground[0], sky.ground[1], sky.ground[2]);
  const Medium medium{air.planetRadiusKm, double(air.planetRadiusKm) + air.atmosphereHeightKm,
                      dvec3(.005802, .013558, .033100) * double(air.rayleighScale),
                      dvec3(.003996 * air.mieScale),
                      dvec3(.000650, .001881, .000085) * double(air.ozoneScale)};
  AtmosphereLut lut{};
  constexpr int directions = 24;
  for (size_t y = 0; y < kAtmosphereLutHeight; ++y) {
    const double h = double(y) / (kAtmosphereLutHeight - 1);
    const dvec3 origin(0, medium.radius + std::max(.002, air.atmosphereHeightKm * h * h), 0);
    for (size_t x = 0; x < kAtmosphereLutWidth; ++x) {
      const double q = 2 * double(x) / (kAtmosphereLutWidth - 1) - 1;
      const double mu = std::copysign(q * q, q);
      const dvec3 sun(std::sqrt(std::max(1 - mu * mu, 0.0)), mu, 0);
      dvec3 radiance(0), feedback(0);
      for (int j = 0; j < directions; ++j) {
        const double vertical = 1 - 2 * (j + .5) / directions;
        const double horizontal = std::sqrt(1 - vertical * vertical);
        const double azimuth = j * 2.399963229728653;
        const dvec3 ray(horizontal * std::cos(azimuth), vertical, horizontal * std::sin(azimuth));
        const double ground = groundHit(origin, ray, medium.radius);
        const double distance = ground >= 0 ? ground : exitDistance(origin, ray, medium.top);
        const double angle = glm::dot(ray, sun);
        const double rayleighPhase = 3 * (1 + angle * angle) / (16 * pi);
        const double miePhase = .36 / (4 * pi * std::pow(1.64 - 1.6 * angle, 1.5));
        dvec3 view(1);
        double previous = 0;
        for (int i = 1; i <= 16; ++i) {
          const double u = i / 16.0;
          const double next = distance * (ground >= 0 ? u * (2 - u) : u * u);
          const dvec3 p = origin + ray * ((next + previous) * .5);
          const auto d = medium.density(p);
          const auto sigma = medium.extinction(d);
          const auto segment = glm::exp(-sigma * (next - previous));
          const auto weight = view * (dvec3(1) - segment) / glm::max(sigma, dvec3(1e-15));
          const auto rayleigh = medium.rayleigh * d.x;
          const auto mie = medium.mie * d.y;
          radiance += weight * solarTransmission(p, sun, medium) *
                      (rayleigh * rayleighPhase + mie * miePhase);
          feedback += weight * (rayleigh + mie);
          view *= segment;
          previous = next;
        }
        if (ground >= 0.0) {
          const auto normal = glm::normalize(origin + ray * ground);
          const auto light = solarTransmission(normal * (medium.radius + 0.002), sun, medium);
          radiance += view * light * groundAlbedo * (std::max(glm::dot(normal, sun), 0.0) / pi);
        }
      }
      // Isotropic higher orders use a bounded local geometric series.
      const auto value =
          (radiance / double(directions)) /
          (dvec3(1) - glm::clamp(feedback / double(directions), dvec3(0), dvec3(.95)));
      lut[y * kAtmosphereLutWidth + x] = {float(value.x), float(value.y), float(value.z), 0.0f};
    }
  }
  return lut;
}

} // namespace container::renderer
