// Adapted from selfshadow/ltc_code fit/fitLTC.cpp, revision
// 31e5e96b54f98f33098f8503003119ba2231a1c6.
// Copyright (c) 2017, Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt.
// License and required paper reference: reference/LICENSE and README.md.
#include <glm/glm.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include "reference/LTC.h"
#include "reference/nelder_mead.h"

namespace {
constexpr float pi = 3.14159265358979323846f;
constexpr uint32_t tableExtent = 64;
struct EngineBrdf {
  float roughness, alphaSquared, projectedMass, transition, transitionMass;
  explicit EngineBrdf(float r) : roughness(std::clamp(r, .045f, 1.f)) {
    alphaSquared = std::pow(roughness, 4.f);
    const float floor = 1.e-4f / pi;
    const float b = 1.f - alphaSquared;
    transition = b > 1.e-7f ? std::min(1.f, (1.f - std::sqrt(floor)) / b) : 1.f;
    transitionMass = b > 1.e-7f
        ? alphaSquared / b * (1.f / (1.f - b * transition) - 1.f)
        : transition;
    projectedMass = transitionMass + (1.f - transition) * alphaSquared / floor;
  }
  float distribution(float nh) const {
    const float d = nh * nh * (alphaSquared - 1.f) + 1.f;
    return alphaSquared / std::max(pi * d * d, 1.e-4f);
  }
  float geometry(float n) const {
    const float k = (roughness + 1.f) * (roughness + 1.f) / 8.f;
    return n / std::max(n * (1.f - k) + k, 1.e-4f);
  }
  float eval(const glm::vec3 &v, const glm::vec3 &l, float &pdf) const {
    const glm::vec3 sum = v + l;
    if (glm::dot(sum, sum) < 1.e-15f) { pdf = 0.f; return 0.f; }
    const glm::vec3 h = glm::normalize(sum);
    const float hv = glm::dot(v, h), nh = std::max(h.z, 0.f);
    const float d = distribution(nh);
    pdf = hv > 1.e-7f && h.z > 0.f ? d * h.z / (4.f * hv * projectedMass) : 0.f;
    if (v.z <= 0.f || l.z <= 0.f) return 0.f;
    return d * geometry(v.z) * geometry(l.z) * l.z /
           std::max(4.f * v.z * l.z, 1.e-4f);
  }
  glm::vec3 sample(const glm::vec3 &v, float u1, float u2) const {
    // Invert the exact projected CDF of the denominator-clamped engine NDF.
    // Stock GGX sampling misses most of this lobe at very small roughness.
    const float b = 1.f - alphaSquared, mass = u2 * projectedMass;
    float cosSquared;
    if (b < 1.e-7f) cosSquared = u2;
    else if (mass <= transitionMass)
      cosSquared = (1.f - 1.f / (1.f + mass * b / alphaSquared)) / b;
    else
      cosSquared = transition + (mass - transitionMass) * (1.e-4f / pi) / alphaSquared;
    cosSquared = std::clamp(cosSquared, 0.f, 1.f);
    const float phi = 2.f * pi * u1, radius = std::sqrt(1.f - cosSquared);
    const glm::vec3 h(radius * std::cos(phi), radius * std::sin(phi), std::sqrt(cosSquared));
    return -v + 2.f * h * glm::dot(h, v);
  }
};

struct Sample { glm::vec3 cosine, brdf; float value, pdf; };
struct Fit {
  LTC &ltc;
  const EngineBrdf &brdf;
  glm::vec3 view;
  const std::vector<Sample> &samples;
  bool isotropic;
  float targetMagnitude;
  void update(const float *p) {
    ltc.m11 = std::max(p[0], 1.e-5f);
    ltc.m22 = isotropic ? ltc.m11 : std::max(p[1], 1.e-5f);
    ltc.m13 = isotropic ? 0.f : p[2];
    ltc.update();
    const glm::vec3 physicalHorizon(ltc.M[0][2], ltc.M[1][2], ltc.M[2][2]);
    const float physicalMass = .5f * (1.f + physicalHorizon.z / glm::length(physicalHorizon));
    ltc.magnitude = targetMagnitude / std::max(physicalMass, 1.e-6f);
  }
  float operator()(const float *p) {
    update(p);
    if (!std::isfinite(ltc.detM) || ltc.detM <= 1.e-12f) return 1.e30f;
    double error = 0.;
    for (const Sample &s : samples) {
      const glm::vec3 l = glm::normalize(ltc.M * s.cosine);
      float pdf;
      const float actual = brdf.eval(view, l, pdf), estimate = l.z > 0.f ? ltc.eval(l) : 0.f;
      double delta = std::abs(actual - estimate);
      error += delta * delta * delta / std::max(pdf + estimate / ltc.magnitude, 1.e-20f);
      const float otherEstimate = s.brdf.z > 0.f ? ltc.eval(s.brdf) : 0.f;
      delta = std::abs(s.value - otherEstimate);
      error += delta * delta * delta /
               std::max(s.pdf + otherEstimate / ltc.magnitude, 1.e-20f);
    }
    return float(error / samples.size());
  }
};

void writeTable(const std::filesystem::path &path, const std::vector<glm::vec4> &data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("Cannot write " + path.string());
  out.write("CLTC", 4);
  const uint32_t header[] = {1, tableExtent, tableExtent};
  out.write(reinterpret_cast<const char *>(header), sizeof(header));
  for (const auto &value : data)
    for (uint32_t c = 0; c < 4; ++c) {
      if (!std::isfinite(value[c])) throw std::runtime_error("Nonfinite fit");
      const float scalar = value[c];
      out.write(reinterpret_cast<const char *>(&scalar), sizeof(scalar));
    }
  if (!out) throw std::runtime_error("Incomplete table write");
}
} // namespace

int main(int argc, char **argv) try {
  if (argc < 2 || argc > 5)
    throw std::runtime_error("Usage: fit_renderer_ltc OUTPUT_DIR [SAMPLES_PER_AXIS=32] [ITERATIONS=100] [THREADS=8]");
  const int count = argc >= 3 ? std::stoi(argv[2]) : 32;
  const int iterations = argc >= 4 ? std::stoi(argv[3]) : 100;
  const unsigned threads = argc >= 5 ? unsigned(std::stoi(argv[4])) : 8u;
  if (count < 8 || count > 256 || iterations < 20 || iterations > 1000 || !threads || threads > 64)
    throw std::runtime_error("Invalid fit settings");
  const std::filesystem::path output(argv[1]);
  std::filesystem::create_directories(output);
  std::vector<glm::vec4> matrices(tableExtent * tableExtent), amplitudes(tableExtent * tableExtent);
  std::atomic<uint32_t> next{0}, done{0};
  auto worker = [&] {
    for (;;) {
      const uint32_t roughnessIndex = next.fetch_add(1);
      if (roughnessIndex >= tableExtent) break;
      const EngineBrdf brdf(float(roughnessIndex) / float(tableExtent - 1));
      LTC ltc;
      for (uint32_t viewIndex = 0; viewIndex < tableExtent; ++viewIndex) {
        const float vCoordinate = float(viewIndex) / float(tableExtent - 1);
        const float theta = std::min(1.57f, std::acos(1.f - vCoordinate * vCoordinate));
        const glm::vec3 view(std::sin(theta), 0.f, std::cos(theta));
        std::vector<Sample> samples;
        samples.reserve(size_t(count) * count);
        double magnitude = 0., fresnel = 0., diffuseFresnel = 0.;
        glm::dvec3 average(0.);
        for (int j = 0; j < count; ++j) for (int i = 0; i < count; ++i) {
          const float u1 = (i + .5f) / count, u2 = (j + .5f) / count;
          const float phi = 2.f * pi * u1;
          const glm::vec3 cosine(std::sqrt(1.f - u2) * std::cos(phi),
                                 std::sqrt(1.f - u2) * std::sin(phi), std::sqrt(u2));
          const glm::vec3 direction = brdf.sample(view, u1, u2);
          float pdf;
          const float value = brdf.eval(view, direction, pdf);
          samples.push_back({cosine, direction, value, pdf});
          if (pdf > 0.f) {
            const double weight = value / pdf;
            const glm::vec3 h = glm::normalize(view + direction);
            magnitude += weight;
            fresnel += weight * std::pow(1.f - std::max(glm::dot(view, h), 0.f), 5.f);
            average += weight * glm::dvec3(direction);
          }
          const glm::vec3 diffuseH = glm::normalize(view + cosine);
          diffuseFresnel += std::pow(1.f - std::max(glm::dot(view, diffuseH), 0.f), 5.f);
        }
        ltc.magnitude = float(magnitude / samples.size());
        ltc.fresnel = float(fresnel / samples.size());
        if (ltc.magnitude < 1.e-12f) throw std::runtime_error("Zero BRDF magnitude");
        average.y = 0.;
        const glm::vec3 averageDir = glm::normalize(glm::vec3(average));
        ltc.X = glm::vec3(averageDir.z, 0.f, -averageDir.x);
        ltc.Y = glm::vec3(0.f, 1.f, 0.f);
        ltc.Z = averageDir;
        if (viewIndex == 0) { ltc.X = {1, 0, 0}; ltc.Z = {0, 0, 1}; ltc.m13 = 0.f; }
        ltc.update();
        const float originalMagnitude = ltc.magnitude;
        Fit fit{ltc, brdf, view, samples, viewIndex == 0, originalMagnitude};
        float start[3] = {ltc.m11, ltc.m22, ltc.m13}, result[3];
        NelderMead<3>(result, start, .05f, 1.e-5f, iterations, fit);
        fit.update(result);
        glm::mat3 inverse = ltc.invM / ltc.invM[1][1];
        const size_t index = roughnessIndex + viewIndex * tableExtent;
        matrices[index] = {inverse[0][0], inverse[0][2], inverse[2][0], inverse[2][2]};
        amplitudes[index] = {ltc.magnitude, ltc.fresnel * ltc.magnitude / originalMagnitude,
                             float(diffuseFresnel / samples.size()), 0.f};
      }
      const auto completed = done.fetch_add(1) + 1;
      std::cout << "fitted roughness rows " << completed << "/" << tableExtent << std::endl;
    }
  };
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < threads; ++i) workers.emplace_back(worker);
  for (auto &thread : workers) thread.join();
  writeTable(output / "matrix.bin", matrices);
  writeTable(output / "amplitude.bin", amplitudes);
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << std::endl;
  return 1;
}
