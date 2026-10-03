#include <gtest/gtest.h>
#include "Container/common/CommonMath.h"
#include "Container/renderer/lighting/AreaLightShadowSampling.h"
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::string shaderSource(const char* name) {
  std::ifstream file(std::string(CONTAINER_SOURCE_DIR) + "/shaders/" + name);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

std::vector<double> shaderArray(const std::string& source, const char* declaration) {
  const auto start = source.find(declaration);
  if (start == std::string::npos) return {};
  const auto brace = source.find('{', start);
  const auto end = source.find('}', brace);
  const std::string body = source.substr(brace + 1, end - brace - 1);
  const std::regex number(R"(-?\d+(?:\.\d+)?)");
  std::vector<double> values;
  for (std::sregex_iterator it(body.begin(), body.end(), number), last; it != last; ++it)
    values.push_back(std::stod(it->str()));
  return values;
}
}

TEST(LightingShadowSampling, DiskIrradianceMatchesAnalyticParallelReceiver) {
  // Evaluate the shader's actual radial nodes/weights against the analytic
  // cosine-weighted irradiance of a unit disk, without a range cutoff.
  const auto source = shaderSource("area_light_common.slang");
  const auto nodes = shaderArray(source, "static const float nodes[5]");
  const auto weights = shaderArray(source, "static const float weights[5]");
  ASSERT_EQ(nodes.size(), 5u);
  ASSERT_EQ(weights.size(), 5u);
  EXPECT_NE(source.find("sqrt(0.5 * (AreaLightGauss5Node(index / 5u) + 1.0))"), std::string::npos);
  EXPECT_NE(source.find("return 0.1 * AreaLightGauss5Weight"), std::string::npos);
  for (const double distance : {0.5, 1.0, 2.0}) {
    double estimate = 0.0;
    for (size_t i = 0; i < nodes.size(); ++i) {
      const double radiusSq = (nodes[i] + 1.0) * 0.5;
      const double distanceSq = distance * distance;
      estimate += weights[i] * 0.5 * distanceSq /
                  std::pow(distanceSq + radiusSq, 2);
    }
    const double exact = 1.0 / (1.0 + distance * distance);
    EXPECT_NEAR(estimate / exact, 1.0, 0.004) << "distance=" << distance;
  }
  double totalWeight = 0.0, radialMoment = 0.0;
  for (size_t i = 0; i < nodes.size(); ++i) {
    totalWeight += weights[i] * 0.5;
    radialMoment += weights[i] * 0.5 * (nodes[i] + 1.0) * 0.5;
  }
  EXPECT_NEAR(totalWeight, 1.0, 1e-9);
  EXPECT_NEAR(radialMoment, 0.5, 1e-9);
}

TEST(LightingShadowSampling, EmitterSamplesRespectBudgetAndStayOnEmitter) {
  using namespace container::renderer;
  EXPECT_EQ(areaShadowSampleCount(5u), 0u);
  EXPECT_EQ(areaShadowSampleCount(8u), 1u);
  EXPECT_EQ(areaShadowSampleCount(18u), 3u);
  EXPECT_EQ(areaShadowSampleCount(24u), 4u);
  EXPECT_EQ(areaShadowSampleCount(24u, 4u), 1u);
  EXPECT_EQ(areaShadowSampleCount(24u, 2u), 2u);
  for (const bool disk : {false, true}) {
    container::gpu::AreaLightData light{};
    light.positionRange = {2, 3, 4, 10};
    light.directionType = {0, 0, -1, disk ? 1.0f : 0.0f};
    light.tangentHalfSize = {1, 0, 0, 2};
    light.bitangentHalfSize = {0, 1, 0, 1};
    for (uint32_t count = 1u; count <= 4u; ++count) {
      glm::vec3 average(0);
      for (uint32_t sample = 0; sample < count; ++sample) {
        const glm::vec3 position = areaShadowSamplePosition(light, sample, count);
        average += position;
        EXPECT_FLOAT_EQ(position.z, 4.0f);
        EXPECT_LE(std::abs(position.x - 2.0f), 2.0f);
        EXPECT_LE(std::abs(position.y - 3.0f), disk ? 2.0f : 1.0f);
        if (disk) EXPECT_LE(glm::length(glm::vec2(position) - glm::vec2(2, 3)), 2.0f);
      }
      EXPECT_NEAR(glm::length(average / float(count) - glm::vec3(light.positionRange)), 0, 1e-5);
    }
  }
}

TEST(LightingShadowSampling, PerspectiveEmitterRaysProjectOffAxisBlockersOntoReceiver) {
  const auto directions = container::renderer::localShadowCubeDirections();
  const auto ups = container::renderer::localShadowCubeUps();
  const glm::mat4 projection = container::math::perspectiveRH_ReverseZ(
      glm::radians(90.0f), 1.0f, 0.05f, 10.0f);
  // This blocker was missed by the old orthographic area-light map: its X
  // coordinate differs from the receiver's, but it lies directly on the ray.
  const glm::vec3 receiver(4, 0, -4), blocker(2, 0, -2);
  const glm::mat4 view = container::math::lookAt(glm::vec3(0), directions[0], ups[0]);
  const glm::vec4 r = projection * view * glm::vec4(receiver, 1);
  const glm::vec4 b = projection * view * glm::vec4(blocker, 1);
  EXPECT_NEAR(r.x / r.w, b.x / b.w, 1e-6);
  EXPECT_NEAR(r.y / r.w, b.y / b.w, 1e-6);
  EXPECT_GT(b.z / b.w, r.z / r.w);
}

TEST(LightingShadowSampling, CubeEdgeFilterDirectionsProjectInsideAdjacentFaces) {
  const auto directions = container::renderer::localShadowCubeDirections();
  const auto ups = container::renderer::localShadowCubeUps();
  const glm::mat4 projection = container::math::perspectiveRH_ReverseZ(
      glm::radians(90.0f), 1.0f, 0.05f, 10.0f);
  for (uint32_t face = 0; face < 6u; ++face) {
    const glm::vec3 right = glm::cross(directions[face], ups[face]);
    for (const glm::vec2 ndc : {glm::vec2(1.02f, 0), glm::vec2(-1.02f, 0),
                                glm::vec2(0, 1.02f), glm::vec2(0, -1.02f)}) {
      const glm::vec3 ray = glm::normalize(directions[face] + right * ndc.x + ups[face] * ndc.y);
      const glm::vec3 axes = glm::abs(ray);
      const uint32_t adjacent = axes.x >= axes.y && axes.x >= axes.z ? (ray.x >= 0 ? 0 : 1)
                               : axes.y >= axes.z ? (ray.y >= 0 ? 2 : 3) : (ray.z >= 0 ? 4 : 5);
      EXPECT_NE(adjacent, face);
      const auto vp = projection * container::math::lookAt(glm::vec3(0), directions[adjacent], ups[adjacent]);
      const glm::vec4 clip = vp * glm::vec4(ray * 4.0f, 1);
      EXPECT_LE(std::abs(clip.x / clip.w), 1.0f);
      EXPECT_LE(std::abs(clip.y / clip.w), 1.0f);
      const glm::vec4 blocker = vp * glm::vec4(ray * 2.0f, 1);
      EXPECT_GT(blocker.z / blocker.w, clip.z / clip.w);
    }
  }
  const auto source = shaderSource("local_shadow_common.slang");
  EXPECT_NE(source.find("SampleLocalShadowCubeTap(layerIndex, tapUv"), std::string::npos);
  EXPECT_NE(source.find("projected.z + bias"), std::string::npos);
}

TEST(LightingShadowSampling, ForwardPathsKeepLightThirteenAndDeferredNormalsAvoidDerivatives) {
  for (const auto name : {"forward_opaque.slang", "forward_transparent.slang"}) {
    const auto source = shaderSource(name);
    EXPECT_EQ(source.find("i < MAX_POINT_LIGHTS"), std::string::npos);
    EXPECT_NE(source.find("i < MAX_CLUSTERED_LIGHTS"), std::string::npos);
  }
  for (const auto name : {"deferred_directional.slang", "point_light.slang", "tiled_lighting.slang"}) {
    const auto source = shaderSource(name);
    EXPECT_EQ(source.find("ddx(worldPosition)"), std::string::npos);
    EXPECT_EQ(source.find("ddy(worldPosition)"), std::string::npos);
    EXPECT_NE(source.find("ReconstructDepthGeometricNormal("), std::string::npos);
  }
}
