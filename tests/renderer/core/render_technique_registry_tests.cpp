#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/core/RendererDeviceCapabilities.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestTechnique final : public container::renderer::RenderTechnique {
public:
  TestTechnique(container::renderer::RenderTechniqueId id,
                bool available = true, std::string reason = {})
      : id_(id), available_(available), reason_(std::move(reason)) {}

  [[nodiscard]] container::renderer::RenderTechniqueId id() const override {
    return id_;
  }

  [[nodiscard]] std::string_view name() const override {
    return container::renderer::renderTechniqueName(id_);
  }

  [[nodiscard]] std::string_view displayName() const override {
    return container::renderer::renderTechniqueDisplayName(id_);
  }

  [[nodiscard]] container::renderer::RenderTechniqueAvailability availability(
      const container::renderer::RenderSystemContext &) const override {
    if (!available_) {
      return container::renderer::RenderTechniqueAvailability::unavailable(
          reason_);
    }
    return container::renderer::RenderTechniqueAvailability::availableNow();
  }

  void buildFrameGraph(container::renderer::RenderSystemContext &) override {
    ++buildFrameGraphCallCount;
  }

  int buildFrameGraphCallCount{0};

private:
  container::renderer::RenderTechniqueId id_{
      container::renderer::RenderTechniqueId::DeferredRaster};
  bool available_{true};
  std::string reason_{};
};

class RayTracingRequiredTechnique final
    : public container::renderer::RenderTechnique {
public:
  [[nodiscard]] container::renderer::RenderTechniqueId id() const override {
    return container::renderer::RenderTechniqueId::RayTracing;
  }

  [[nodiscard]] std::string_view name() const override {
    return container::renderer::renderTechniqueName(id());
  }

  [[nodiscard]] std::string_view displayName() const override {
    return container::renderer::renderTechniqueDisplayName(id());
  }

  [[nodiscard]] container::renderer::RenderTechniqueAvailability availability(
      const container::renderer::RenderSystemContext &context) const override {
    if (context.deviceCapabilities == nullptr ||
        !context.deviceCapabilities->supportsRayTracing()) {
      return container::renderer::RenderTechniqueAvailability::unavailable(
          "ray tracing device capabilities are unavailable");
    }
    return container::renderer::RenderTechniqueAvailability::availableNow();
  }

  void buildFrameGraph(container::renderer::RenderSystemContext &) override {}
};

[[nodiscard]] bool contains(std::string_view haystack,
                            std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

[[nodiscard]] std::string readRepoTextFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open " + path.string());
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

} // namespace

TEST(RenderTechniqueRegistry, ParsesStableTechniqueNames) {
  using container::renderer::RenderTechniqueId;

  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("deferred-raster"),
            RenderTechniqueId::DeferredRaster);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("forward-raster"),
            RenderTechniqueId::ForwardRaster);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("forward-rendering"),
            RenderTechniqueId::ForwardRaster);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("real-time-rendering"),
            RenderTechniqueId::ForwardRaster);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("realtime-rendering"),
            RenderTechniqueId::ForwardRaster);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("ray-tracing"),
            RenderTechniqueId::RayTracing);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("path-tracing"),
            RenderTechniqueId::PathTracing);
  EXPECT_EQ(
      container::renderer::renderTechniqueIdFromName("gaussian-splatting"),
      RenderTechniqueId::GaussianSplatting);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("radiance-field"),
            RenderTechniqueId::RadianceField);
  EXPECT_FALSE(
      container::renderer::renderTechniqueIdFromName("unknown").has_value());
}

TEST(RenderTechniqueRegistry,
     KnownTechniqueDescriptorsExposeFutureAlgorithmsWithoutRegisteringThem) {
  using container::renderer::RenderTechniqueId;

  const auto descriptors =
      container::renderer::knownRenderTechniqueDescriptors();
  ASSERT_EQ(descriptors.size(), 6u);

  const auto findDescriptor = [&](RenderTechniqueId id) {
    return std::ranges::find_if(descriptors, [id](const auto &descriptor) {
      return descriptor.id == id;
    });
  };

  const auto deferred = findDescriptor(RenderTechniqueId::DeferredRaster);
  ASSERT_NE(deferred, descriptors.end());
  EXPECT_TRUE(deferred->implemented);

  const auto forwardRaster = findDescriptor(RenderTechniqueId::ForwardRaster);
  ASSERT_NE(forwardRaster, descriptors.end());
  EXPECT_TRUE(forwardRaster->implemented);
  EXPECT_EQ(forwardRaster->displayName,
            std::string_view{"Forward rendering"});

  const auto rayTracing = findDescriptor(RenderTechniqueId::RayTracing);
  ASSERT_NE(rayTracing, descriptors.end());
  EXPECT_FALSE(rayTracing->implemented);
  EXPECT_TRUE(rayTracing->requiresRayTracing);

  const auto pathTracing = findDescriptor(RenderTechniqueId::PathTracing);
  ASSERT_NE(pathTracing, descriptors.end());
  EXPECT_FALSE(pathTracing->implemented);
  EXPECT_TRUE(pathTracing->requiresPathTracing);

  container::renderer::RenderTechniqueRegistry registry =
      container::renderer::createDefaultRenderTechniqueRegistry();
  EXPECT_EQ(registry.techniques().size(), 2u);
  EXPECT_EQ(registry.find(RenderTechniqueId::RayTracing), nullptr);
  EXPECT_EQ(registry.find(RenderTechniqueId::PathTracing), nullptr);
}

TEST(RenderTechniqueRegistry, RejectsNullAndDuplicateRegistrations) {
  using container::renderer::RenderTechniqueId;

  container::renderer::RenderTechniqueRegistry registry;
  EXPECT_THROW(registry.registerTechnique(nullptr), std::invalid_argument);

  registry.registerTechnique(
      std::make_unique<TestTechnique>(RenderTechniqueId::DeferredRaster));
  EXPECT_THROW(registry.registerTechnique(std::make_unique<TestTechnique>(
                   RenderTechniqueId::DeferredRaster)),
               std::invalid_argument);
}

TEST(RenderTechniqueRegistry, FallsBackToDeferredWhenRequestedUnavailable) {
  using container::renderer::RenderTechniqueId;

  container::renderer::RenderTechniqueRegistry registry;
  registry.registerTechnique(
      std::make_unique<TestTechnique>(RenderTechniqueId::DeferredRaster));
  registry.registerTechnique(std::make_unique<TestTechnique>(
      RenderTechniqueId::RayTracing, false,
      "ray tracing device extensions are unavailable"));

  container::renderer::RenderSystemContext context{};
  const container::renderer::RenderTechniqueSelection selection =
      registry.select(RenderTechniqueId::RayTracing, context);

  ASSERT_NE(selection.technique, nullptr);
  EXPECT_TRUE(selection.usedFallback);
  EXPECT_EQ(selection.requested, RenderTechniqueId::RayTracing);
  EXPECT_EQ(selection.selected, RenderTechniqueId::DeferredRaster);
  EXPECT_EQ(selection.technique->id(), RenderTechniqueId::DeferredRaster);
  EXPECT_TRUE(contains(selection.unavailableReason,
                       "device extensions are unavailable"));
}

TEST(RenderTechniqueRegistry, UsesDeviceCapabilitiesForTechniqueAvailability) {
  using container::renderer::RenderTechniqueId;

  container::renderer::RenderTechniqueRegistry registry;
  registry.registerTechnique(
      std::make_unique<TestTechnique>(RenderTechniqueId::DeferredRaster));
  registry.registerTechnique(std::make_unique<RayTracingRequiredTechnique>());

  const container::renderer::RendererDeviceCapabilities rasterOnly =
      container::renderer::RendererDeviceCapabilities::rasterOnly();
  container::renderer::RenderSystemContext unavailableContext{
      .deviceCapabilities = &rasterOnly};

  const container::renderer::RenderTechniqueSelection fallback =
      registry.select(RenderTechniqueId::RayTracing, unavailableContext);
  EXPECT_TRUE(fallback.usedFallback);
  EXPECT_EQ(fallback.selected, RenderTechniqueId::DeferredRaster);
  EXPECT_TRUE(
      contains(fallback.unavailableReason, "ray tracing device capabilities"));

  container::renderer::RendererDeviceCapabilities rayTracing{};
  rayTracing.descriptorIndexing = true;
  rayTracing.bufferDeviceAddress = true;
  rayTracing.accelerationStructure = true;
  rayTracing.rayTracingPipeline = true;
  container::renderer::RenderSystemContext availableContext{
      .deviceCapabilities = &rayTracing};

  const container::renderer::RenderTechniqueSelection selection =
      registry.select(RenderTechniqueId::RayTracing, availableContext);
  EXPECT_FALSE(selection.usedFallback);
  EXPECT_EQ(selection.selected, RenderTechniqueId::RayTracing);
  ASSERT_NE(selection.technique, nullptr);
  EXPECT_EQ(selection.technique->id(), RenderTechniqueId::RayTracing);
}

TEST(RendererDeviceCapabilities, ReportsMissingRayAndPathRequirements) {
  const container::renderer::RendererDeviceCapabilities rasterOnly =
      container::renderer::RendererDeviceCapabilities::rasterOnly();

  const std::vector<std::string> missingRayTracing =
      rasterOnly.missingRayTracingRequirements();
  EXPECT_FALSE(missingRayTracing.empty());
  EXPECT_NE(std::ranges::find(missingRayTracing, "buffer device address"),
            missingRayTracing.end());
  EXPECT_NE(std::ranges::find(missingRayTracing, "acceleration structure"),
            missingRayTracing.end());
  EXPECT_NE(std::ranges::find(missingRayTracing, "ray tracing pipeline"),
            missingRayTracing.end());

  const std::vector<std::string> missingPathTracing =
      rasterOnly.missingPathTracingRequirements();
  EXPECT_NE(std::ranges::find(missingPathTracing, "storage image atomics"),
            missingPathTracing.end());

  container::renderer::RendererDeviceCapabilities full{};
  full.descriptorIndexing = true;
  full.bufferDeviceAddress = true;
  full.accelerationStructure = true;
  full.rayTracingPipeline = true;
  full.storageImageAtomics = true;
  EXPECT_TRUE(full.supportsPathTracing());
  EXPECT_TRUE(full.missingPathTracingRequirements().empty());
}

TEST(RenderTechniqueRegistry, DefaultRegistryRegistersDeferredAndForwardRaster) {
  container::renderer::RenderTechniqueRegistry registry =
      container::renderer::createDefaultRenderTechniqueRegistry();

  ASSERT_NE(
      registry.find(container::renderer::RenderTechniqueId::DeferredRaster),
      nullptr);
  ASSERT_NE(registry.find(container::renderer::RenderTechniqueId::ForwardRaster),
            nullptr);
  EXPECT_EQ(registry.techniques().size(), 2u);
}

TEST(RenderTechniqueGuardrails, FrameRecorderDoesNotOwnGraphRegistration) {
  const std::filesystem::path frameRecorderHeaderPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "include" / "Container" /
      "renderer" / "core" / "FrameRecorder.h";
  const std::filesystem::path rendererFrontendHeaderPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "include" / "Container" /
      "renderer" / "core" / "RendererFrontend.h";
  const std::filesystem::path renderTechniqueHeaderPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "include" / "Container" /
      "renderer" / "core" / "RenderTechnique.h";
  const std::filesystem::path frameRecorderPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "src" / "renderer" /
      "core" / "FrameRecorder.cpp";
  const std::filesystem::path postProcessHeaderPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "include" / "Container" /
      "renderer" / "deferred" / "DeferredRasterPostProcess.h";
  const std::filesystem::path postProcessPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "src" / "renderer" /
      "deferred" / "DeferredRasterPostProcess.cpp";
  const std::filesystem::path deferredRasterTechniquePath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "src" / "renderer" /
      "deferred" / "DeferredRasterTechnique.cpp";
  const std::filesystem::path deferredFrameGraphContextHeaderPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "include" / "Container" /
      "renderer" / "deferred" / "DeferredRasterFrameGraphContext.h";
  const std::filesystem::path deferredFrameGraphContextPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "src" / "renderer" /
      "deferred" / "DeferredRasterFrameGraphContext.cpp";
  const std::filesystem::path shadowFramePassRecorderPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "src" / "renderer" /
      "shadow" / "ShadowCascadeFramePassRecorder.cpp";
  const std::string header = readRepoTextFile(frameRecorderHeaderPath);
  const std::string rendererFrontendHeader =
      readRepoTextFile(rendererFrontendHeaderPath);
  const std::string renderTechniqueHeader =
      readRepoTextFile(renderTechniqueHeaderPath);
  const std::string source = readRepoTextFile(frameRecorderPath);
  const std::string postProcessHeader = readRepoTextFile(postProcessHeaderPath);
  const std::string postProcessSource = readRepoTextFile(postProcessPath);
  const std::string deferredRasterTechnique =
      readRepoTextFile(deferredRasterTechniquePath);
  const std::string deferredFrameGraphContextHeader =
      readRepoTextFile(deferredFrameGraphContextHeaderPath);
  const std::string deferredFrameGraphContext =
      readRepoTextFile(deferredFrameGraphContextPath);
  const std::string shadowFramePassRecorder =
      readRepoTextFile(shadowFramePassRecorderPath);

  EXPECT_FALSE(contains(header, "DeferredRasterFrameGraphContext"));
  EXPECT_TRUE(
      contains(rendererFrontendHeader, "DeferredRasterFrameGraphContext"));
  EXPECT_TRUE(
      contains(rendererFrontendHeader, "deferredRasterFrameGraphContext"));
  EXPECT_TRUE(contains(renderTechniqueHeader,
                       "DeferredRasterFrameGraphContext* deferredRaster"));
  EXPECT_FALSE(contains(header, "friend class DeferredRasterTechnique"));
  EXPECT_FALSE(contains(header, "recordPostProcessPass"));
  EXPECT_FALSE(contains(source, "FrameRecorder::recordPostProcessPass"));
  EXPECT_FALSE(contains(source, "FrameRecorder::recordShadowPassBody"));
  EXPECT_FALSE(contains(source, "recordShadowCascadePassCommands"));
  EXPECT_TRUE(contains(deferredFrameGraphContextHeader,
                       "ShadowCascadeFramePassRecorder"));
  EXPECT_TRUE(contains(deferredFrameGraphContext,
                       "shadowCascadeFramePassRecorder_"));
  EXPECT_TRUE(
      contains(shadowFramePassRecorder, "recordShadowCascadePassCommands"));
  EXPECT_FALSE(contains(header, "recordPostProcessOverlays"));
  EXPECT_FALSE(contains(source, "recordTransformGizmoOverlay"));
  EXPECT_TRUE(contains(deferredRasterTechnique,
                       "recordDeferredRasterTransformGizmoOverlay"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "deferred->renderGui(passCmd)"));
  EXPECT_TRUE(contains(deferredFrameGraphContext,
                       "recordDeferredRasterGuiPass"));
  EXPECT_TRUE(contains(deferredFrameGraphContext,
                       "recordScreenshotCaptureCopy"));
  EXPECT_TRUE(contains(deferredFrameGraphContext,
                       "prepareBimFrameGpuVisibility"));
  EXPECT_TRUE(contains(deferredFrameGraphContext,
                       "recordBimFrameGpuVisibilityCommands"));
  EXPECT_FALSE(contains(header, "ShadowCascadeFramePassRecorder.h"));
  EXPECT_TRUE(contains(postProcessHeader, "DeferredPostProcessPassRecordInputs"));
  EXPECT_TRUE(
      contains(postProcessSource, "recordDeferredPostProcessPassCommands"));
  EXPECT_FALSE(contains(source, "void FrameRecorder::buildGraph()"));
  EXPECT_FALSE(contains(source, "graph_.addPass("));
}

TEST(RenderTechniqueGuardrails, FrameRecorderDoesNotRegrowTechniqueCommandHubs) {
  const std::filesystem::path frameRecorderHeaderPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "include" / "Container" /
      "renderer" / "core" / "FrameRecorder.h";
  const std::filesystem::path frameRecorderPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "src" / "renderer" /
      "core" / "FrameRecorder.cpp";
  const std::string frameRecorder =
      readRepoTextFile(frameRecorderHeaderPath) + "\n" +
      readRepoTextFile(frameRecorderPath);

  const std::vector<std::string_view> forbiddenHubs = {
      "FrameRecorder::recordDeferredRaster",
      "FrameRecorder::recordLightingPass",
      "FrameRecorder::recordDeferredPasses",
      "FrameRecorder::recordDeferredFrame",
      "FrameRecorder::recordDeferredCommands",
      "FrameRecorder::recordBimPasses",
      "FrameRecorder::recordBimFrame",
      "FrameRecorder::recordBimCommands",
      "FrameRecorder::recordShadowPasses",
      "FrameRecorder::recordShadowFrame",
      "FrameRecorder::recordShadowCommands",
      "FrameRecorder::buildDeferred",
      "FrameRecorder::buildBim",
      "FrameRecorder::buildShadow",
      "recordDeferredRasterPasses",
      "recordBimPasses",
      "recordShadowPasses",
      "buildDeferredPasses",
      "buildBimPasses",
      "buildShadowPasses",
  };

  for (std::string_view forbidden : forbiddenHubs) {
    EXPECT_FALSE(contains(frameRecorder, forbidden))
        << "Keep new technique/domain command hubs outside FrameRecorder: "
        << forbidden;
  }
}

TEST(RenderTechniqueGuardrails,
     FutureTechniquePassesStayOutOfDeferredRasterGraph) {
  const std::filesystem::path techniquePath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "src" / "renderer" /
      "deferred" / "DeferredRasterTechnique.cpp";
  const std::string source = readRepoTextFile(techniquePath);

  ASSERT_TRUE(contains(source, "DeferredRasterTechnique::buildFrameGraph"));
  EXPECT_TRUE(contains(source, "DeferredRasterFrameGraphContext *deferred"));
  ASSERT_TRUE(contains(source, "RenderGraphBuilder graph"));
  ASSERT_TRUE(contains(source, "graph.addPass("));
  EXPECT_FALSE(contains(source, "FrameRecorder& recorder"));
  EXPECT_FALSE(contains(source, "recorder."));
  EXPECT_FALSE(contains(source, "recorder.graph_.addPass("));
  EXPECT_TRUE(contains(source, "recordDeferredPostProcessPassCommands"));
  EXPECT_FALSE(contains(source, "deferred.recordPostProcessPass"));
  EXPECT_FALSE(contains(source, "RayTracing"));
  EXPECT_FALSE(contains(source, "PathTracing"));
  EXPECT_FALSE(contains(source, "Splat"));
  EXPECT_FALSE(contains(source, "RadianceField"));
  EXPECT_FALSE(contains(source, "NeRF"));
}
