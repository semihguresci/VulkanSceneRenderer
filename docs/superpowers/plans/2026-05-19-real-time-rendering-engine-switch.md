# Forward Rendering Engine Switch Implementation Plan

## Completion Status

Implementation is complete as of 2026-05-20. The forward raster technique is
registered as `Forward rendering`, can be selected from the editor render-engine
combo, can be requested from the CLI with `--render-technique forward-rendering`,
builds a non-G-buffer forward render graph, renders through the direct forward
opaque path, reuses shared shadows/OIT/bloom/exposure/post-process infrastructure,
and supports the editor Overview display mode.

Final verification covered the focused forward/registry/debug targets, affected
renderer validation tests, the full CTest suite, and headless Sponza captures for
both deferred and forward rendering. The interactive editor checklist remains a
human-operated smoke path because this environment cannot click the native ImGui
window, but the selector and switch plumbing are covered by guardrail tests and
runtime headless captures.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a selectable forward raster rendering engine next to the existing deferred renderer and expose the switch in the editor.

**Architecture:** Treat "Forward rendering" as the existing `RenderTechniqueId::ForwardRaster` slot, implemented as a forward raster technique. The editor requests technique changes, while `RendererFrontend` applies them at a safe frame boundary by rebuilding the technique contracts and render graph. The first forward path reuses shared depth, shadow, OIT, bloom, exposure, post-process, and gizmo infrastructure, but replaces G-buffer plus deferred lighting with direct forward opaque lighting into `scene-color`.

**Tech Stack:** C++23, Vulkan, Slang shaders, ImGui, GTest, CMake/Ninja.

---

## Scope

This plan implements a forward raster renderer. It does not implement real-time ray tracing, path tracing, virtual shadow maps, or a full clustered Forward+ light list rewrite. The forward technique should still use the current tiled/local-shadow data when it is already produced by shared managers, but it must not run G-buffer-only passes such as `GBuffer`, `BimGBuffer`, `TileCull`, or `GTAO`.

## File Structure

- Create `include/Container/renderer/forward/ForwardRasterTechnique.h`
  - Owns the public technique class for `RenderTechniqueId::ForwardRaster`.
- Create `src/renderer/forward/ForwardRasterTechnique.cpp`
  - Registers forward technique resource and pipeline contracts.
  - Builds the forward render graph with custom dependencies.
- Create `include/Container/renderer/forward/ForwardRasterResourceBridge.h`
  - Provides ForwardRaster-scoped resource lookup helpers for scene color, depth, framebuffers, descriptors, and OIT resources.
- Create `include/Container/renderer/forward/ForwardRasterPipelineBridge.h`
  - Provides ForwardRaster-scoped pipeline lookup helpers for forward opaque, transparent, shadow, post-process, and gizmo pipelines.
- Create `include/Container/renderer/forward/ForwardRasterLightingPassRecorder.h`
  - Declares the direct forward lighting render-pass recorder.
- Create `src/renderer/forward/ForwardRasterLightingPassRecorder.cpp`
  - Records the lighting render pass: forward opaque scene and BIM draws, transparent OIT draws, and debug overlays.
- Create `shaders/forward_opaque.slang`
  - Direct-lit opaque material shader based on `forward_transparent.slang`, returning `SV_Target0` color instead of writing OIT nodes.
- Modify `include/Container/renderer/core/RenderTechnique.h`
  - Add a user-facing alias helper only if needed; keep `RenderTechniqueId::ForwardRaster`.
- Modify `src/renderer/core/RenderTechnique.cpp`
  - Mark ForwardRaster implemented, label it "Forward rendering", parse aliases, and register `ForwardRasterTechnique`.
- Modify `include/Container/renderer/core/FrameRecorder.h`
  - Add `RenderTechniqueId activeTechnique` to `FrameRuntimeResources`.
- Modify `include/Container/renderer/core/RendererFrontend.h`
  - Add pending render-technique switch state and helper declarations.
- Modify `src/renderer/core/RendererFrontend.cpp`
  - Factor technique context creation, initialization, GUI option sync, pending switch application, and runtime resource mirroring for active technique.
- Modify `include/Container/utility/GuiManager.h`
  - Add render engine option state, active engine state, and pending engine-change consumption.
- Modify `src/utility/GuiManager.cpp`
  - Add a "Rendering Engine" combo to Scene Controls.
- Modify `include/Container/renderer/pipeline/PipelineTypes.h`
  - Add neutral vectors for extra technique pipeline handles, not ForwardRaster-specific fields.
- Modify `src/renderer/pipeline/GraphicsPipelineBuilder.cpp`
  - Load `forward_opaque` shader and create a registry-owned forward opaque pipeline handle.
- Modify `src/renderer/pipeline/PipelineRegistry.cpp`
  - Register neutral extra handles/layouts in the built registry.
- Modify `src/CMakeLists.txt`
  - Add the new forward renderer source files.
- Modify `tests/CMakeLists.tests.cmake`
  - Add forward technique tests.
- Modify `tests/renderer/core/render_technique_registry_tests.cpp`
  - Update registry expectations and alias parsing.
- Modify `tests/renderer/core/resource_pipeline_registry_tests.cpp`
  - Add forward resource/pipeline contract tests and update old deferred-only expectations.
- Modify `tests/renderer/core/technique_debug_model_tests.cpp`
  - Update ForwardRaster display name expectations and add GUI/frontend guardrails.
- Create `tests/renderer/forward/forward_raster_technique_tests.cpp`
  - Unit-test the forward graph shape.

---

### Task 1: Baseline And Registry Tests

**Files:**
- Modify: `tests/renderer/core/render_technique_registry_tests.cpp`
- Modify: `tests/renderer/core/technique_debug_model_tests.cpp`

- [ ] **Step 1: Run the current focused tests before changes**

Run:

```powershell
cmake --build out/build/windows-debug --target render_technique_registry_tests technique_debug_model_tests
ctest --test-dir out/build/windows-debug -R "render_technique_registry_tests|technique_debug_model_tests" --output-on-failure
```

Expected: existing tests pass before changing expectations.

- [ ] **Step 2: Write failing registry expectations**

In `tests/renderer/core/render_technique_registry_tests.cpp`, update `ParsesStableTechniqueNames`:

```cpp
  EXPECT_EQ(
      container::renderer::renderTechniqueIdFromName("real-time-rendering"),
      RenderTechniqueId::ForwardRaster);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("realtime-rendering"),
            RenderTechniqueId::ForwardRaster);
```

In `KnownTechniqueDescriptorsExposeFutureAlgorithmsWithoutRegisteringThem`, add the ForwardRaster assertions after the deferred assertions:

```cpp
  const auto forward = findDescriptor(RenderTechniqueId::ForwardRaster);
  ASSERT_NE(forward, descriptors.end());
  EXPECT_TRUE(forward->implemented);
  EXPECT_EQ(forward->displayName, std::string_view{"Forward rendering"});
```

Replace the old default-registry size expectation in that test:

```cpp
  EXPECT_EQ(registry.techniques().size(), 2u);
```

Replace `DefaultRegistryRegistersOnlyDeferredRaster` with:

```cpp
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
```

- [ ] **Step 3: Write failing display-name expectation**

In `tests/renderer/core/technique_debug_model_tests.cpp`, change:

```cpp
  EXPECT_EQ(model.displayName, "Forward raster");
```

to:

```cpp
  EXPECT_EQ(model.displayName, "Forward rendering");
```

- [ ] **Step 4: Verify these tests fail**

Run:

```powershell
cmake --build out/build/windows-debug --target render_technique_registry_tests technique_debug_model_tests
ctest --test-dir out/build/windows-debug -R "render_technique_registry_tests|technique_debug_model_tests" --output-on-failure
```

Expected: failures mention missing alias, ForwardRaster not implemented, default registry size still `1`, and display name still "Forward raster".

---

### Task 2: Implement ForwardRaster Technique Identity

**Files:**
- Create: `include/Container/renderer/forward/ForwardRasterTechnique.h`
- Create: `src/renderer/forward/ForwardRasterTechnique.cpp`
- Modify: `src/renderer/core/RenderTechnique.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add the ForwardRasterTechnique header**

Create `include/Container/renderer/forward/ForwardRasterTechnique.h`:

```cpp
#pragma once

#include "Container/renderer/core/RenderTechnique.h"

namespace container::renderer {

class ForwardRasterTechnique final : public RenderTechnique {
 public:
  [[nodiscard]] RenderTechniqueId id() const override {
    return RenderTechniqueId::ForwardRaster;
  }
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view displayName() const override;
  [[nodiscard]] RenderTechniqueAvailability availability(
      const RenderSystemContext& context) const override;
  [[nodiscard]] TechniqueDebugModel debugModel() const override;

  void registerTechniqueContracts(RenderSystemContext& context) override;
  void buildFrameGraph(RenderSystemContext& context) override;
};

}  // namespace container::renderer
```

- [ ] **Step 2: Add a minimal source file**

Create `src/renderer/forward/ForwardRasterTechnique.cpp`:

```cpp
#include "Container/renderer/forward/ForwardRasterTechnique.h"

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/deferred/DeferredRasterFrameGraphContext.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"

#include <utility>

namespace container::renderer {

namespace {

constexpr RenderTechniqueId kForwardRasterTechnique =
    RenderTechniqueId::ForwardRaster;

void registerForwardRasterFrameResources(FrameResourceRegistry& registry) {
  registry.clearTechnique(kForwardRasterTechnique);

  registry.registerExternal(kForwardRasterTechnique, "swapchain");
  registry.registerExternal(kForwardRasterTechnique, "scene-geometry");
  registry.registerExternal(kForwardRasterTechnique, "bim-geometry");
  registry.registerExternal(kForwardRasterTechnique, "shadow-atlas");
  registry.registerExternal(kForwardRasterTechnique, "local-shadow-atlas");

  registry.registerBuffer(
      kForwardRasterTechnique, "camera-buffer",
      FrameBufferDesc{.size = sizeof(container::gpu::CameraData),
                      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT},
      FrameResourceLifetime::Imported);
  registry.registerBuffer(
      kForwardRasterTechnique, "scene-object-buffer",
      FrameBufferDesc{.size = 0, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},
      FrameResourceLifetime::Imported);
  registry.registerDescriptorSet(kForwardRasterTechnique,
                                 "scene-descriptor-set",
                                 FrameResourceLifetime::Imported);
  registry.registerDescriptorSet(kForwardRasterTechnique,
                                 "bim-scene-descriptor-set",
                                 FrameResourceLifetime::Imported);
  registry.registerDescriptorSet(kForwardRasterTechnique,
                                 "light-descriptor-set",
                                 FrameResourceLifetime::Imported);
  registry.registerDescriptorSet(kForwardRasterTechnique,
                                 "shadow-descriptor-set",
                                 FrameResourceLifetime::Imported);
  registry.registerDescriptorSet(kForwardRasterTechnique,
                                 "local-shadow-descriptor-set",
                                 FrameResourceLifetime::Imported);
  registry.registerSampler(kForwardRasterTechnique, "g-buffer-sampler",
                           FrameSamplerDesc{}, FrameResourceLifetime::Imported);

  registry.registerImage(
      kForwardRasterTechnique, "depth-stencil",
      FrameImageDesc{.format = VK_FORMAT_UNDEFINED,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kForwardRasterTechnique, "depth-sampling-view",
      FrameImageDesc{.format = VK_FORMAT_UNDEFINED,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kForwardRasterTechnique, "scene-color",
      FrameImageDesc{.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kForwardRasterTechnique, "pick-id",
      FrameImageDesc{.format = VK_FORMAT_R32_UINT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kForwardRasterTechnique, "pick-depth",
      FrameImageDesc{.format = VK_FORMAT_UNDEFINED,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kForwardRasterTechnique, "oit-head-pointers",
      FrameImageDesc{.format = VK_FORMAT_R32_UINT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT});

  registry.registerBuffer(
      kForwardRasterTechnique, "oit-node-buffer",
      FrameBufferDesc{.size = 0, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},
      FrameResourceLifetime::PerFrame);
  registry.registerBuffer(
      kForwardRasterTechnique, "oit-counter-buffer",
      FrameBufferDesc{.size = sizeof(uint32_t),
                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT},
      FrameResourceLifetime::PerFrame);
  registry.registerBuffer(
      kForwardRasterTechnique, "oit-metadata-buffer",
      FrameBufferDesc{.size = sizeof(OitMetadata),
                      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT},
      FrameResourceLifetime::PerFrame);
  registry.registerDescriptorSet(kForwardRasterTechnique,
                                 "frame-lighting-descriptor-set");
  registry.registerDescriptorSet(kForwardRasterTechnique,
                                 "post-process-descriptor-set");
  registry.registerDescriptorSet(kForwardRasterTechnique, "oit-descriptor-set");

  registry.registerFramebuffer(kForwardRasterTechnique,
                               "depth-prepass-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 1u});
  registry.registerFramebuffer(kForwardRasterTechnique,
                               "bim-depth-prepass-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 1u});
  registry.registerFramebuffer(kForwardRasterTechnique,
                               "transparent-pick-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 2u});
  registry.registerFramebuffer(kForwardRasterTechnique, "lighting-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 2u});
  registry.registerFramebuffer(kForwardRasterTechnique,
                               "transform-gizmo-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 1u});
}

void registerForwardRasterPipelineRecipes(PipelineRegistry& registry) {
  registry.clearTechnique(kForwardRasterTechnique);
  registry.registerRecipe(PipelineRecipe{
      .key = {kForwardRasterTechnique, "forward-opaque"},
      .kind = PipelineRecipeKind::Graphics,
      .shaderStages = {"spv_shaders/forward_opaque.vert.spv",
                       "spv_shaders/forward_opaque.frag.spv"},
      .layoutName = "transparent"});
  registry.registerRecipe(PipelineRecipe{
      .key = {kForwardRasterTechnique, "transparent"},
      .kind = PipelineRecipeKind::Graphics,
      .shaderStages = {"spv_shaders/forward_transparent.vert.spv",
                       "spv_shaders/forward_transparent.frag.spv"},
      .layoutName = "transparent"});
  registry.registerRecipe(PipelineRecipe{
      .key = {kForwardRasterTechnique, "post-process"},
      .kind = PipelineRecipeKind::Graphics,
      .shaderStages = {"spv_shaders/post_process.vert.spv",
                       "spv_shaders/post_process.frag.spv"},
      .layoutName = "post-process"});
}

}  // namespace

std::string_view ForwardRasterTechnique::name() const {
  return renderTechniqueName(id());
}

std::string_view ForwardRasterTechnique::displayName() const {
  return renderTechniqueDisplayName(id());
}

RenderTechniqueAvailability ForwardRasterTechnique::availability(
    const RenderSystemContext&) const {
  return RenderTechniqueAvailability::availableNow();
}

TechniqueDebugModel ForwardRasterTechnique::debugModel() const {
  return {.techniqueName = std::string(name()),
          .displayName = std::string(displayName()),
          .panels = {{.id = "forward-frame",
                      .title = "Forward Frame",
                      .controls = {{"render-graph", "Render graph"},
                                   {"forward-opaque", "Forward opaque"},
                                   {"transparent-oit", "Transparent OIT"}}}}};
}

void ForwardRasterTechnique::registerTechniqueContracts(
    RenderSystemContext& context) {
  if (context.frameResources != nullptr) {
    registerForwardRasterFrameResources(*context.frameResources);
  }
  if (context.pipelines != nullptr) {
    registerForwardRasterPipelineRecipes(*context.pipelines);
  }
}

void ForwardRasterTechnique::buildFrameGraph(RenderSystemContext& context) {
  registerTechniqueContracts(context);
  if (context.frameRecorder == nullptr || context.deferredRaster == nullptr) {
    return;
  }
  RenderGraphBuilder graph = context.frameRecorder->graphBuilder();
  graph.clear();
  graph.compile();
}

}  // namespace container::renderer
```

This compiles the class first. Later tasks fill the graph and recorder.

- [ ] **Step 3: Register the source file**

In `src/CMakeLists.txt`, add:

```cmake
    renderer/forward/ForwardRasterTechnique.cpp
```

near the deferred renderer source entries.

- [ ] **Step 4: Wire the technique into the registry**

In `src/renderer/core/RenderTechnique.cpp`, add:

```cpp
#include "Container/renderer/forward/ForwardRasterTechnique.h"
```

Change the ForwardRaster display name:

```cpp
    std::string_view{"Forward rendering"},
```

Change the descriptor:

```cpp
        {.id = RenderTechniqueId::ForwardRaster,
         .name = "forward-raster",
         .displayName = "Forward rendering",
         .implemented = true},
```

Update `renderTechniqueIdFromName` after the stable-name search:

```cpp
  if (name == "real-time-rendering" || name == "realtime-rendering" ||
      name == "real-time") {
    return RenderTechniqueId::ForwardRaster;
  }
```

Update `createDefaultRenderTechniqueRegistry()`:

```cpp
  registry.registerTechnique(std::make_unique<DeferredRasterTechnique>());
  registry.registerTechnique(std::make_unique<ForwardRasterTechnique>());
```

- [ ] **Step 5: Verify registry tests pass**

Run:

```powershell
cmake --build out/build/windows-debug --target render_technique_registry_tests technique_debug_model_tests
ctest --test-dir out/build/windows-debug -R "render_technique_registry_tests|technique_debug_model_tests" --output-on-failure
```

Expected: registry and display-name tests pass.

---

### Task 3: Forward Technique Contract And Graph Tests

**Files:**
- Create: `tests/renderer/forward/forward_raster_technique_tests.cpp`
- Modify: `tests/CMakeLists.tests.cmake`
- Modify: `tests/renderer/core/resource_pipeline_registry_tests.cpp`

- [ ] **Step 1: Add the forward test target**

In `tests/CMakeLists.tests.cmake`, define:

```cmake
set(TEST_RENDERER_FORWARD_DIR "${TEST_RENDERER_DIR}/forward")
```

near the other renderer test directory variables.

Add a test target:

```cmake
add_custom_test(forward_raster_technique_tests
    ${TEST_RENDERER_FORWARD_DIR}/forward_raster_technique_tests.cpp  ""  ${TEST_RESULTS_DIR}
    VulkanSceneRenderer_renderer
)
target_compile_definitions(forward_raster_technique_tests PRIVATE
    CONTAINER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
```

- [ ] **Step 2: Write failing forward contract tests**

Create `tests/renderer/forward/forward_raster_technique_tests.cpp`:

```cpp
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/deferred/DeferredRasterFrameGraphContext.h"
#include "Container/renderer/forward/ForwardRasterTechnique.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

namespace {

bool hasPass(const container::renderer::RenderGraphDebugModel& model,
             std::string_view name) {
  return std::ranges::any_of(model.passes, [name](const auto& pass) {
    return pass.passName == name;
  });
}

}  // namespace

TEST(ForwardRasterTechniqueTests, PublishesForwardContracts) {
  container::renderer::FrameResourceRegistry resources;
  container::renderer::PipelineRegistry pipelines;
  container::renderer::ForwardRasterTechnique technique;
  container::renderer::RenderSystemContext context{
      .frameResources = &resources,
      .pipelines = &pipelines,
  };

  technique.registerTechniqueContracts(context);

  using container::renderer::RenderTechniqueId;
  EXPECT_TRUE(resources.contains(
      {RenderTechniqueId::ForwardRaster, "scene-color"}));
  EXPECT_TRUE(resources.contains(
      {RenderTechniqueId::ForwardRaster, "depth-stencil"}));
  EXPECT_TRUE(resources.contains(
      {RenderTechniqueId::ForwardRaster, "lighting-framebuffer"}));
  EXPECT_TRUE(pipelines.contains(
      {RenderTechniqueId::ForwardRaster, "forward-opaque"}));
  EXPECT_TRUE(pipelines.contains(
      {RenderTechniqueId::ForwardRaster, "transparent"}));
  EXPECT_FALSE(resources.contains(
      {RenderTechniqueId::ForwardRaster, "gbuffer-framebuffer"}));
  EXPECT_FALSE(pipelines.contains(
      {RenderTechniqueId::ForwardRaster, "gbuffer"}));
}

TEST(ForwardRasterTechniqueTests, BuildsForwardGraphWithoutGBufferOnlyPasses) {
  container::renderer::FrameRecorder recorder;
  container::renderer::DeferredRasterFrameGraphContext deferredContext(
      container::renderer::DeferredRasterFrameGraphServices{
          .graph = &recorder.graph()});
  container::renderer::FrameResourceRegistry resources;
  container::renderer::PipelineRegistry pipelines;
  container::renderer::ForwardRasterTechnique technique;
  container::renderer::RenderSystemContext context{
      .frameRecorder = &recorder,
      .deferredRaster = &deferredContext,
      .frameResources = &resources,
      .pipelines = &pipelines,
  };

  technique.buildFrameGraph(context);

  const auto model = recorder.graph().debugModel();
  EXPECT_TRUE(hasPass(model, "DepthPrepass"));
  EXPECT_TRUE(hasPass(model, "Lighting"));
  EXPECT_TRUE(hasPass(model, "PostProcess"));
  EXPECT_TRUE(hasPass(model, "ShadowCascade0"));
  EXPECT_FALSE(hasPass(model, "GBuffer"));
  EXPECT_FALSE(hasPass(model, "BimGBuffer"));
  EXPECT_FALSE(hasPass(model, "TileCull"));
  EXPECT_FALSE(hasPass(model, "GTAO"));
}
```

- [ ] **Step 3: Update existing resource registry expectations**

In `tests/renderer/core/resource_pipeline_registry_tests.cpp`, replace the two old false expectations in `DefaultRegistrySelectionPublishesDeferredContracts`:

```cpp
  EXPECT_FALSE(resources.contains(
      TechniqueResourceKey{RenderTechniqueId::ForwardRaster, "scene-color"}));
  EXPECT_FALSE(pipelines.contains(
      TechniquePipelineKey{RenderTechniqueId::ForwardRaster, "forward-opaque"}));
```

with:

```cpp
  const auto forwardSelection =
      registry.select(RenderTechniqueId::ForwardRaster, context);
  ASSERT_NE(forwardSelection.technique, nullptr);
  EXPECT_EQ(forwardSelection.selected, RenderTechniqueId::ForwardRaster);

  forwardSelection.technique->registerTechniqueContracts(context);

  EXPECT_TRUE(resources.contains(
      TechniqueResourceKey{RenderTechniqueId::ForwardRaster, "scene-color"}));
  EXPECT_TRUE(pipelines.contains(
      TechniquePipelineKey{RenderTechniqueId::ForwardRaster, "forward-opaque"}));
  EXPECT_FALSE(resources.contains(
      TechniqueResourceKey{RenderTechniqueId::ForwardRaster,
                           "gbuffer-framebuffer"}));
```

- [ ] **Step 4: Verify forward tests fail until graph is implemented**

Run:

```powershell
cmake --build out/build/windows-debug --target forward_raster_technique_tests resource_pipeline_registry_tests
ctest --test-dir out/build/windows-debug -R "forward_raster_technique_tests|resource_pipeline_registry_tests" --output-on-failure
```

Expected: `BuildsForwardGraphWithoutGBufferOnlyPasses` fails because the graph still compiles empty.

---

### Task 4: Pipeline And Shader Support For Forward Opaque

**Files:**
- Create: `shaders/forward_opaque.slang`
- Modify: `include/Container/renderer/pipeline/PipelineTypes.h`
- Modify: `src/renderer/pipeline/GraphicsPipelineBuilder.cpp`
- Modify: `src/renderer/pipeline/PipelineRegistry.cpp`
- Modify: `tests/renderer/core/resource_pipeline_registry_tests.cpp`

- [ ] **Step 1: Write failing registry-handle test**

In `tests/renderer/core/resource_pipeline_registry_tests.cpp`, add to the pipeline registry tests:

```cpp
TEST(PipelineRegistryTests, RegistersExtraTechniquePipelineHandles) {
  GraphicsPipelines pipelines;
  const VkPipeline forwardOpaque = fakeHandle<VkPipeline>(0x501);
  pipelines.extraHandles.push_back(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "forward-opaque"},
      .pipeline = forwardOpaque});

  const auto registry = buildGraphicsPipelineHandleRegistry(pipelines);

  EXPECT_EQ(registry->pipelineHandle(
                {RenderTechniqueId::ForwardRaster, "forward-opaque"}),
            forwardOpaque);
}
```

Run:

```powershell
cmake --build out/build/windows-debug --target resource_pipeline_registry_tests
ctest --test-dir out/build/windows-debug -R resource_pipeline_registry_tests --output-on-failure
```

Expected: compile failure because `GraphicsPipelines::extraHandles` does not exist.

- [ ] **Step 2: Add neutral extra pipeline storage**

In `include/Container/renderer/pipeline/PipelineTypes.h`, add includes:

```cpp
#include <vector>
```

Add to `struct GraphicsPipelines`:

```cpp
  std::vector<RegisteredPipelineHandle> extraHandles{};
```

Add to `struct PipelineLayouts` if a future task needs extra layouts:

```cpp
  std::vector<RegisteredPipelineLayout> extraLayouts{};
```

- [ ] **Step 3: Register extra handles and layouts**

In `src/renderer/pipeline/PipelineRegistry.cpp`, after the built-in handle registrations in `buildGraphicsPipelineHandleRegistry`, add:

```cpp
  for (const RegisteredPipelineHandle& handle : pipelines.extraHandles) {
    registry->registerHandle(handle);
  }
```

In `buildGraphicsPipelineLayoutRegistry`, after built-in layout registrations, add:

```cpp
  for (const RegisteredPipelineLayout& layout : layouts.extraLayouts) {
    registry->registerLayout(layout);
  }
```

- [ ] **Step 4: Create the forward opaque shader**

Create `shaders/forward_opaque.slang` by copying `shaders/forward_transparent.slang`, then make these exact edits:

Remove:

```hlsl
#include "oit_common.slang"
```

Remove OIT bindings:

```hlsl
[[vk::binding(0, 2)]] RWTexture2D<uint> headPointerImage;
[[vk::binding(1, 2)]] RWStructuredBuffer<TransparentNode> transparentNodes;
[[vk::binding(2, 2)]] RWStructuredBuffer<uint> nodeCounter;
[[vk::binding(3, 2)]] ConstantBuffer<OitMetadataBuffer> uOit;
```

Rename helper prefixes from `TransparentFinite` to `ForwardFinite`, and keep behavior identical:

```hlsl
float ForwardFiniteOr(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float2 ForwardFinite2Or(float2 value, float2 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}

float3 ForwardFinite3Or(float3 value, float3 fallback)
{
    return all(isfinite(value)) ? value : fallback;
}
```

Replace the OIT tail at the end of `fragMain`:

```hlsl
    uint2 pixelCoord = uint2(vertIn.pos.xy);
    if (pixelCoord.x >= uOit.viewportWidth || pixelCoord.y >= uOit.viewportHeight)
    {
        return 0.0.xxxx;
    }

    uint nodeIndex = 0u;
    InterlockedAdd(nodeCounter[0], 1u, nodeIndex);
    if (nodeIndex >= uOit.nodeCapacity)
    {
        return 0.0.xxxx;
    }

    TransparentNode node;
    node.color = float4(lighting, saturate(baseAlpha));
    node.depth = saturate(vertIn.pos.z);
    node.next = INVALID_NODE_INDEX;
    node.padding0 = 0.0;
    node.padding1 = 0.0;

    uint previousHead = INVALID_NODE_INDEX;
    InterlockedExchange(headPointerImage[pixelCoord], nodeIndex, previousHead);
    node.next = previousHead;
    transparentNodes[nodeIndex] = node;
    return 0.0.xxxx;
```

with:

```hlsl
    return float4(lighting, 1.0);
```

- [ ] **Step 5: Create the forward opaque pipeline**

In `src/renderer/pipeline/GraphicsPipelineBuilder.cpp`, load shader modules near the existing transparent modules:

```cpp
  VkShaderModule forwardOpaqueVert =
      loadModule("spv_shaders/forward_opaque.vert.spv");
  VkShaderModule forwardOpaqueFrag =
      loadModule("spv_shaders/forward_opaque.frag.spv");
```

Create stages:

```cpp
  std::array<VkPipelineShaderStageCreateInfo, 2> forwardOpaqueStages = {
      makeStage(forwardOpaqueVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(forwardOpaqueFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
```

Before transparent pipeline creation, add:

```cpp
  VkGraphicsPipelineCreateInfo forwardOpaquePCI = meshPCI;
  forwardOpaquePCI.stageCount =
      static_cast<uint32_t>(forwardOpaqueStages.size());
  forwardOpaquePCI.pStages = forwardOpaqueStages.data();
  forwardOpaquePCI.pColorBlendState = &opaqueBlend;
  VkPipeline forwardOpaquePipeline = pipelineManager_.createGraphicsPipeline(
      forwardOpaquePCI, "forward_opaque_pipeline");
  pipelines.extraHandles.push_back(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "forward-opaque"},
      .pipeline = forwardOpaquePipeline});
```

Destroy shader modules where the existing shader modules are destroyed:

```cpp
  vkDestroyShaderModule(device, forwardOpaqueVert, nullptr);
  vkDestroyShaderModule(device, forwardOpaqueFrag, nullptr);
```

- [ ] **Step 6: Destroy extra pipelines**

In `RendererFrontend::destroyGraphicsPipelines()` in `src/renderer/core/RendererFrontend.cpp`, before clearing `resources_.builtPipelines`, add:

```cpp
  for (const RegisteredPipelineHandle& handle :
       pipelines.extraHandles) {
    destroyPipeline(handle.pipeline);
  }
  pipelines.extraHandles.clear();
```

- [ ] **Step 7: Verify shader and pipeline tests**

Run:

```powershell
cmake --build out/build/windows-debug --target shaders resource_pipeline_registry_tests
ctest --test-dir out/build/windows-debug -R resource_pipeline_registry_tests --output-on-failure
```

Expected: shader compilation succeeds and the extra handle registry test passes.

---

### Task 5: Forward Resource And Pipeline Bridges

**Files:**
- Create: `include/Container/renderer/forward/ForwardRasterResourceBridge.h`
- Create: `include/Container/renderer/forward/ForwardRasterPipelineBridge.h`
- Modify: `src/renderer/forward/ForwardRasterTechnique.cpp`

- [ ] **Step 1: Add ForwardRaster resource bridge**

Create `include/Container/renderer/forward/ForwardRasterResourceBridge.h`:

```cpp
#pragma once

#include "Container/renderer/core/FrameRecorder.h"

#include <string_view>

namespace container::renderer {

enum class ForwardRasterFramebufferId {
  DepthPrepass,
  BimDepthPrepass,
  TransparentPick,
  Lighting,
  TransformGizmo,
};

enum class ForwardRasterImageId {
  DepthStencil,
  DepthSamplingView,
  SceneColor,
  PickDepth,
  PickId,
  OitHeadPointers,
};

enum class ForwardRasterBufferId {
  Camera,
  SceneObject,
  OitNode,
  OitCounter,
  OitMetadata,
};

enum class ForwardRasterDescriptorSetId {
  Scene,
  BimScene,
  Light,
  Shadow,
  LocalShadow,
  FrameLighting,
  PostProcess,
  Oit,
};

enum class ForwardRasterSamplerId {
  GBuffer,
};

[[nodiscard]] inline std::string_view forwardRasterFramebufferKey(
    ForwardRasterFramebufferId id) {
  switch (id) {
  case ForwardRasterFramebufferId::DepthPrepass:
    return "depth-prepass-framebuffer";
  case ForwardRasterFramebufferId::BimDepthPrepass:
    return "bim-depth-prepass-framebuffer";
  case ForwardRasterFramebufferId::TransparentPick:
    return "transparent-pick-framebuffer";
  case ForwardRasterFramebufferId::Lighting:
    return "lighting-framebuffer";
  case ForwardRasterFramebufferId::TransformGizmo:
    return "transform-gizmo-framebuffer";
  }
  return {};
}

[[nodiscard]] inline std::string_view forwardRasterImageKey(
    ForwardRasterImageId id) {
  switch (id) {
  case ForwardRasterImageId::DepthStencil:
    return "depth-stencil";
  case ForwardRasterImageId::DepthSamplingView:
    return "depth-sampling-view";
  case ForwardRasterImageId::SceneColor:
    return "scene-color";
  case ForwardRasterImageId::PickDepth:
    return "pick-depth";
  case ForwardRasterImageId::PickId:
    return "pick-id";
  case ForwardRasterImageId::OitHeadPointers:
    return "oit-head-pointers";
  }
  return {};
}

[[nodiscard]] inline std::string_view forwardRasterBufferKey(
    ForwardRasterBufferId id) {
  switch (id) {
  case ForwardRasterBufferId::Camera:
    return "camera-buffer";
  case ForwardRasterBufferId::SceneObject:
    return "scene-object-buffer";
  case ForwardRasterBufferId::OitNode:
    return "oit-node-buffer";
  case ForwardRasterBufferId::OitCounter:
    return "oit-counter-buffer";
  case ForwardRasterBufferId::OitMetadata:
    return "oit-metadata-buffer";
  }
  return {};
}

[[nodiscard]] inline std::string_view forwardRasterDescriptorSetKey(
    ForwardRasterDescriptorSetId id) {
  switch (id) {
  case ForwardRasterDescriptorSetId::Scene:
    return "scene-descriptor-set";
  case ForwardRasterDescriptorSetId::BimScene:
    return "bim-scene-descriptor-set";
  case ForwardRasterDescriptorSetId::Light:
    return "light-descriptor-set";
  case ForwardRasterDescriptorSetId::Shadow:
    return "shadow-descriptor-set";
  case ForwardRasterDescriptorSetId::LocalShadow:
    return "local-shadow-descriptor-set";
  case ForwardRasterDescriptorSetId::FrameLighting:
    return "frame-lighting-descriptor-set";
  case ForwardRasterDescriptorSetId::PostProcess:
    return "post-process-descriptor-set";
  case ForwardRasterDescriptorSetId::Oit:
    return "oit-descriptor-set";
  }
  return {};
}

[[nodiscard]] inline VkImage forwardRasterImage(
    const FrameRecordParams& p, ForwardRasterImageId id) {
  return p.imageBinding(RenderTechniqueId::ForwardRaster,
                        forwardRasterImageKey(id))
             ? p.imageBinding(RenderTechniqueId::ForwardRaster,
                              forwardRasterImageKey(id))
                   ->image
             : VK_NULL_HANDLE;
}

[[nodiscard]] inline VkImageView forwardRasterImageView(
    const FrameRecordParams& p, ForwardRasterImageId id) {
  const FrameImageBinding* binding =
      p.imageBinding(RenderTechniqueId::ForwardRaster,
                     forwardRasterImageKey(id));
  return binding != nullptr ? binding->view : VK_NULL_HANDLE;
}

[[nodiscard]] inline VkBuffer forwardRasterBuffer(
    const FrameRecordParams& p, ForwardRasterBufferId id) {
  const FrameBufferBinding* binding =
      p.bufferBinding(RenderTechniqueId::ForwardRaster,
                      forwardRasterBufferKey(id));
  return binding != nullptr ? binding->buffer : VK_NULL_HANDLE;
}

[[nodiscard]] inline VkDeviceSize forwardRasterBufferSize(
    const FrameRecordParams& p, ForwardRasterBufferId id) {
  const FrameBufferBinding* binding =
      p.bufferBinding(RenderTechniqueId::ForwardRaster,
                      forwardRasterBufferKey(id));
  return binding != nullptr ? binding->size : 0;
}

[[nodiscard]] inline VkDescriptorSet forwardRasterDescriptorSet(
    const FrameRecordParams& p, ForwardRasterDescriptorSetId id) {
  return p.descriptorSet(RenderTechniqueId::ForwardRaster,
                         forwardRasterDescriptorSetKey(id));
}

[[nodiscard]] inline VkSampler forwardRasterSampler(
    const FrameRecordParams& p, ForwardRasterSamplerId id) {
  const std::string_view key =
      id == ForwardRasterSamplerId::GBuffer ? "g-buffer-sampler" : "";
  return p.sampler(RenderTechniqueId::ForwardRaster, key);
}

[[nodiscard]] inline VkFramebuffer forwardRasterFramebuffer(
    const FrameRecordParams& p, ForwardRasterFramebufferId id) {
  return p.framebuffer(RenderTechniqueId::ForwardRaster,
                       forwardRasterFramebufferKey(id));
}

}  // namespace container::renderer
```

- [ ] **Step 2: Add ForwardRaster pipeline bridge**

Create `include/Container/renderer/forward/ForwardRasterPipelineBridge.h`:

```cpp
#pragma once

#include "Container/renderer/core/FrameRecorder.h"

#include <string_view>

namespace container::renderer {

enum class ForwardRasterPipelineId {
  ForwardOpaque,
  ForwardOpaqueFrontCull,
  ForwardOpaqueNoCull,
  Transparent,
  TransparentFrontCull,
  TransparentNoCull,
  PostProcess,
  TransformGizmo,
  TransformGizmoSolid,
  TransformGizmoOverlay,
  TransformGizmoSolidOverlay,
};

enum class ForwardRasterPipelineLayoutId {
  Scene,
  Transparent,
  PostProcess,
  TransformGizmo,
};

[[nodiscard]] inline std::string_view forwardRasterPipelineName(
    ForwardRasterPipelineId id) {
  switch (id) {
  case ForwardRasterPipelineId::ForwardOpaque:
    return "forward-opaque";
  case ForwardRasterPipelineId::ForwardOpaqueFrontCull:
    return "forward-opaque-front-cull";
  case ForwardRasterPipelineId::ForwardOpaqueNoCull:
    return "forward-opaque-no-cull";
  case ForwardRasterPipelineId::Transparent:
    return "transparent";
  case ForwardRasterPipelineId::TransparentFrontCull:
    return "transparent-front-cull";
  case ForwardRasterPipelineId::TransparentNoCull:
    return "transparent-no-cull";
  case ForwardRasterPipelineId::PostProcess:
    return "post-process";
  case ForwardRasterPipelineId::TransformGizmo:
    return "transform-gizmo";
  case ForwardRasterPipelineId::TransformGizmoSolid:
    return "transform-gizmo-solid";
  case ForwardRasterPipelineId::TransformGizmoOverlay:
    return "transform-gizmo-overlay";
  case ForwardRasterPipelineId::TransformGizmoSolidOverlay:
    return "transform-gizmo-solid-overlay";
  }
  return {};
}

[[nodiscard]] inline std::string_view forwardRasterPipelineLayoutName(
    ForwardRasterPipelineLayoutId id) {
  switch (id) {
  case ForwardRasterPipelineLayoutId::Scene:
    return "scene";
  case ForwardRasterPipelineLayoutId::Transparent:
    return "transparent";
  case ForwardRasterPipelineLayoutId::PostProcess:
    return "post-process";
  case ForwardRasterPipelineLayoutId::TransformGizmo:
    return "transform-gizmo";
  }
  return {};
}

[[nodiscard]] inline VkPipeline forwardRasterPipelineHandle(
    const FrameRecordParams& p, ForwardRasterPipelineId id) {
  return p.pipelineHandle(RenderTechniqueId::ForwardRaster,
                          forwardRasterPipelineName(id));
}

[[nodiscard]] inline VkPipelineLayout forwardRasterPipelineLayout(
    const FrameRecordParams& p, ForwardRasterPipelineLayoutId id) {
  return p.pipelineLayout(RenderTechniqueId::ForwardRaster,
                          forwardRasterPipelineLayoutName(id));
}

}  // namespace container::renderer
```

- [ ] **Step 3: Mirror shared handles under ForwardRaster**

In `src/renderer/pipeline/PipelineRegistry.cpp`, extend `buildGraphicsPipelineHandleRegistry` after existing DeferredRaster registrations:

```cpp
  auto registerForwardAlias = [&registry](const char* name, VkPipeline pipeline) {
    if (pipeline == VK_NULL_HANDLE) {
      return;
    }
    registry->registerHandle(RegisteredPipelineHandle{
        .key = {RenderTechniqueId::ForwardRaster, name},
        .pipeline = pipeline});
  };

  registerForwardAlias("transparent", pipelines.transparent);
  registerForwardAlias("transparent-front-cull", pipelines.transparentFrontCull);
  registerForwardAlias("transparent-no-cull", pipelines.transparentNoCull);
  registerForwardAlias("transparent-pick", pipelines.transparentPick);
  registerForwardAlias("transparent-pick-front-cull",
                       pipelines.transparentPickFrontCull);
  registerForwardAlias("transparent-pick-no-cull",
                       pipelines.transparentPickNoCull);
  registerForwardAlias("post-process", pipelines.postProcess);
  registerForwardAlias("transform-gizmo", pipelines.transformGizmo);
  registerForwardAlias("transform-gizmo-solid", pipelines.transformGizmoSolid);
  registerForwardAlias("transform-gizmo-overlay",
                       pipelines.transformGizmoOverlay);
  registerForwardAlias("transform-gizmo-solid-overlay",
                       pipelines.transformGizmoSolidOverlay);
```

In `buildGraphicsPipelineLayoutRegistry`, mirror layouts:

```cpp
  auto registerForwardLayoutAlias = [&registry](const char* name,
                                                VkPipelineLayout layout) {
    if (layout == VK_NULL_HANDLE) {
      return;
    }
    registry->registerLayout(RegisteredPipelineLayout{
        .key = {RenderTechniqueId::ForwardRaster, name},
        .layout = layout});
  };

  registerForwardLayoutAlias("scene", layouts.scene);
  registerForwardLayoutAlias("transparent", layouts.transparent);
  registerForwardLayoutAlias("post-process", layouts.postProcess);
  registerForwardLayoutAlias("transform-gizmo", layouts.transformGizmo);
```

- [ ] **Step 4: Verify bridge compiles**

Run:

```powershell
cmake --build out/build/windows-debug --target VulkanSceneRenderer_renderer resource_pipeline_registry_tests
ctest --test-dir out/build/windows-debug -R resource_pipeline_registry_tests --output-on-failure
```

Expected: compilation succeeds and pipeline registry tests pass.

---

### Task 6: Direct Forward Lighting Recorder

**Files:**
- Create: `include/Container/renderer/forward/ForwardRasterLightingPassRecorder.h`
- Create: `src/renderer/forward/ForwardRasterLightingPassRecorder.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add recorder header**

Create `include/Container/renderer/forward/ForwardRasterLightingPassRecorder.h`:

```cpp
#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/core/FrameRecorder.h"

namespace container::renderer {

struct ForwardRasterLightingPassServices {
  VkExtent2D framebufferExtent{};
  const DebugOverlayRenderer* debugOverlay{nullptr};
};

[[nodiscard]] RenderPassReadiness forwardRasterLightingReadiness(
    const FrameRecordParams& p, VkExtent2D extent);

[[nodiscard]] bool recordForwardRasterLightingPassCommands(
    VkCommandBuffer cmd,
    const FrameRecordParams& p,
    const ForwardRasterLightingPassServices& services);

}  // namespace container::renderer
```

- [ ] **Step 2: Add recorder implementation**

Create `src/renderer/forward/ForwardRasterLightingPassRecorder.cpp`:

```cpp
#include "Container/renderer/forward/ForwardRasterLightingPassRecorder.h"

#include "Container/renderer/deferred/DeferredTransparentOitRecorder.h"
#include "Container/renderer/forward/ForwardRasterPipelineBridge.h"
#include "Container/renderer/forward/ForwardRasterResourceBridge.h"
#include "Container/renderer/scene/SceneTransparentDrawPlanner.h"
#include "Container/renderer/scene/SceneTransparentDrawRecorder.h"
#include "Container/renderer/scene/SceneViewport.h"

#include <array>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand>* commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] SceneTransparentDrawLists forwardOpaqueDrawLists(
    const FrameDrawLists& draws) {
  return {.aggregate = draws.opaqueDrawCommands,
          .singleSided = draws.opaqueSingleSidedDrawCommands,
          .windingFlipped = draws.opaqueWindingFlippedDrawCommands,
          .doubleSided = draws.opaqueDoubleSidedDrawCommands};
}

[[nodiscard]] SceneTransparentDrawLists forwardTransparentDrawLists(
    const FrameDrawLists& draws) {
  return {.aggregate = draws.transparentDrawCommands,
          .singleSided = draws.transparentSingleSidedDrawCommands,
          .windingFlipped = draws.transparentWindingFlippedDrawCommands,
          .doubleSided = draws.transparentDoubleSidedDrawCommands};
}

[[nodiscard]] SceneTransparentDrawGeometryBinding forwardSceneGeometry(
    const FrameRecordParams& p,
    std::span<const VkDescriptorSet> descriptorSets) {
  return {.descriptorSets = descriptorSets,
          .vertexSlice = p.scene.vertexSlice,
          .indexSlice = p.scene.indexSlice,
          .indexType = p.scene.indexType};
}

[[nodiscard]] bool forwardSceneReady(const FrameRecordParams& p) {
  return forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::Scene) !=
             VK_NULL_HANDLE &&
         p.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.scene.indexSlice.buffer != VK_NULL_HANDLE &&
         forwardRasterPipelineLayout(
             p, ForwardRasterPipelineLayoutId::Transparent) != VK_NULL_HANDLE;
}

}  // namespace

RenderPassReadiness forwardRasterLightingReadiness(
    const FrameRecordParams& p, VkExtent2D extent) {
  if (extent.width == 0u || extent.height == 0u ||
      forwardRasterFramebuffer(p, ForwardRasterFramebufferId::Lighting) ==
          VK_NULL_HANDLE ||
      forwardRasterPipelineLayout(p,
                                  ForwardRasterPipelineLayoutId::Transparent) ==
          VK_NULL_HANDLE ||
      forwardRasterPipelineHandle(p,
                                  ForwardRasterPipelineId::ForwardOpaque) ==
          VK_NULL_HANDLE) {
    return renderPassMissingResource(RenderResourceId::SceneColor);
  }
  if (!forwardSceneReady(p)) {
    return renderPassMissingResource(RenderResourceId::SceneGeometry);
  }
  if (!hasDrawCommands(p.draws.opaqueDrawCommands) &&
      !hasDrawCommands(p.draws.transparentDrawCommands)) {
    return renderPassNotNeeded();
  }
  return renderPassReady();
}

bool recordForwardRasterLightingPassCommands(
    VkCommandBuffer cmd,
    const FrameRecordParams& p,
    const ForwardRasterLightingPassServices& services) {
  if (cmd == VK_NULL_HANDLE ||
      forwardRasterLightingReadiness(p, services.framebufferExtent).ready ==
          false) {
    return false;
  }

  std::array<VkClearValue, 2> clearValues{};
  clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
  clearValues[1].depthStencil = {0.0f, 0u};

  VkRenderPassBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = p.postProcess.renderPass;
  info.framebuffer =
      forwardRasterFramebuffer(p, ForwardRasterFramebufferId::Lighting);
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = services.framebufferExtent;
  info.clearValueCount = static_cast<uint32_t>(clearValues.size());
  info.pClearValues = clearValues.data();

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  recordSceneViewportAndScissor(cmd, services.framebufferExtent);

  const std::array<VkDescriptorSet, 4> descriptorSets = {
      forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::Scene),
      forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::Light),
      forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::Oit),
      forwardRasterDescriptorSet(p,
                                 ForwardRasterDescriptorSetId::FrameLighting)};

  const SceneTransparentDrawPlan opaquePlan =
      buildSceneTransparentDrawPlan(forwardOpaqueDrawLists(p.draws));
  (void)recordSceneTransparentDrawCommands(
      cmd, {.plan = &opaquePlan,
            .geometry = forwardSceneGeometry(p, descriptorSets),
            .pipelines = {.primary = forwardRasterPipelineHandle(
                              p, ForwardRasterPipelineId::ForwardOpaque),
                          .frontCull = forwardRasterPipelineHandle(
                              p,
                              ForwardRasterPipelineId::ForwardOpaqueFrontCull),
                          .noCull = forwardRasterPipelineHandle(
                              p, ForwardRasterPipelineId::ForwardOpaqueNoCull)},
            .pipelineLayout = forwardRasterPipelineLayout(
                p, ForwardRasterPipelineLayoutId::Transparent),
            .pushConstants = p.pushConstants.bindless != nullptr
                                 ? *p.pushConstants.bindless
                                 : container::gpu::BindlessPushConstants{},
            .debugOverlay = services.debugOverlay});

  const SceneTransparentDrawPlan transparentPlan =
      buildSceneTransparentDrawPlan(forwardTransparentDrawLists(p.draws));
  (void)recordSceneTransparentDrawCommands(
      cmd, {.plan = &transparentPlan,
            .geometry = forwardSceneGeometry(p, descriptorSets),
            .pipelines = {.primary = forwardRasterPipelineHandle(
                              p, ForwardRasterPipelineId::Transparent),
                          .frontCull = forwardRasterPipelineHandle(
                              p, ForwardRasterPipelineId::TransparentFrontCull),
                          .noCull = forwardRasterPipelineHandle(
                              p, ForwardRasterPipelineId::TransparentNoCull)},
            .pipelineLayout = forwardRasterPipelineLayout(
                p, ForwardRasterPipelineLayoutId::Transparent),
            .pushConstants = p.pushConstants.bindless != nullptr
                                 ? *p.pushConstants.bindless
                                 : container::gpu::BindlessPushConstants{},
            .debugOverlay = services.debugOverlay});

  vkCmdEndRenderPass(cmd);
  return true;
}

}  // namespace container::renderer
```

During implementation, verify the `info.renderPass` assignment. If `FrameRecordParams` does not expose the lighting render pass directly, add `VkRenderPass lightingRenderPass{VK_NULL_HANDLE};` to `FramePostProcessState` or pass it through the services struct and fill it from `resources_.renderPasses.lighting`.

- [ ] **Step 3: Register the recorder source**

In `src/CMakeLists.txt`, add:

```cmake
    renderer/forward/ForwardRasterLightingPassRecorder.cpp
```

- [ ] **Step 4: Build**

Run:

```powershell
cmake --build out/build/windows-debug --target VulkanSceneRenderer_renderer
```

Expected: compile succeeds. If `RenderPassReadiness` uses a field other than `.ready`, adjust the boolean check to match the existing type in `RenderGraph.h`.

---

### Task 7: Build The Forward Render Graph

**Files:**
- Modify: `src/renderer/forward/ForwardRasterTechnique.cpp`
- Modify: `tests/renderer/forward/forward_raster_technique_tests.cpp`

- [ ] **Step 1: Implement graph passes with custom dependencies**

In `src/renderer/forward/ForwardRasterTechnique.cpp`, include the forward recorder and shared deferred/shadow recorders:

```cpp
#include "Container/renderer/forward/ForwardRasterLightingPassRecorder.h"
#include "Container/renderer/deferred/DeferredRasterDepthReadOnlyTransitionRecorder.h"
#include "Container/renderer/deferred/DeferredRasterPostProcess.h"
#include "Container/renderer/deferred/DeferredRasterSceneColorReadBarrierRecorder.h"
#include "Container/renderer/deferred/DeferredRasterScenePassRecorder.h"
#include "Container/renderer/deferred/DeferredTransparentOitFramePassRecorder.h"
#include "Container/renderer/effects/BloomManager.h"
#include "Container/renderer/effects/ExposureManager.h"
#include "Container/renderer/picking/TransparentPickRasterPassRecorder.h"
#include "Container/renderer/shadow/ShadowCullPassPlanner.h"
#include "Container/renderer/shadow/ShadowCullPassRecorder.h"
```

Replace the empty graph body in `ForwardRasterTechnique::buildFrameGraph` with passes in this order:

```cpp
  DeferredRasterFrameGraphContext* deferred = context.deferredRaster;
  const DeferredTransparentOitFramePassRecorder transparentOit =
      deferred->transparentOitFramePassRecorder();
  RenderGraphBuilder graph = context.frameRecorder->graphBuilder();
  graph.clear();

  graph.addPass(RenderPassId::FrustumCull,
                [deferred](VkCommandBuffer cmd, const FrameRecordParams& p) {
                  // Use the existing deferred frustum-cull recorder until the
                  // cull domain is made technique-neutral.
                  (void)cmd;
                  (void)p;
                });

  graph.addPass(RenderPassId::DepthPrepass,
                [deferred](VkCommandBuffer cmd, const FrameRecordParams& p) {
                  static_cast<void>(recordDeferredRasterScenePassCommands(
                      cmd, deferredRasterScenePassInputs(
                               p, SceneRasterPassKind::DepthPrepass,
                               *deferred)));
                });

  graph.addPass(RenderPassId::BimDepthPrepass,
                {RenderPassId::DepthPrepass},
                [deferred](VkCommandBuffer cmd, const FrameRecordParams& p) {
                  const std::array<VkDescriptorSet, 1> bimDescriptorSets = {
                      deferredRasterBimSceneDescriptorSet(p)};
                  static_cast<void>(recordDeferredRasterBimSurfacePassCommands(
                      cmd, deferredRasterBimSurfacePassInputs(
                               p, BimSurfacePassKind::DepthPrepass, *deferred,
                               bimDescriptorSets)));
                });

  const auto shadowPassIds = shadowCascadePassIds();
  for (uint32_t i = 0; i < shadowPassIds.size(); ++i) {
    graph.addPass(shadowPassIds[i], {RenderPassId::BimDepthPrepass},
                  [deferred, i](VkCommandBuffer cmd,
                                const FrameRecordParams& p) {
                    deferred->recordShadowPass(cmd, p, i);
                  });
  }

  graph.addPass(RenderPassId::LocalShadowDepth,
                [deferred](VkCommandBuffer cmd, const FrameRecordParams& p) {
                  recordDeferredRasterLocalShadowPass(cmd, p, false);
                });

  graph.addPass(
      RenderPassId::DepthToReadOnly,
      {RenderPassId::BimDepthPrepass, RenderPassId::ShadowCascade0,
       RenderPassId::ShadowCascade1, RenderPassId::ShadowCascade2,
       RenderPassId::ShadowCascade3, RenderPassId::LocalShadowDepth},
      [deferred](VkCommandBuffer cmd, const FrameRecordParams& p) {
        const DeferredRasterDepthReadOnlyTransitionPlan transitionPlan =
            buildDeferredRasterDepthReadOnlyTransitionPlan(
                {.depthStencilImage =
                     deferredRasterImage(p, DeferredRasterImageId::DepthStencil),
                 .shadowAtlasImage =
                     p.shadows.shadowManager != nullptr
                         ? p.shadows.shadowManager->shadowAtlasImage()
                         : VK_NULL_HANDLE,
                 .shadowAtlasVisible = false,
                 .shadowCascadeCount = container::gpu::kShadowCascadeCount,
                 .localShadowAtlasImage =
                     p.shadows.shadowManager != nullptr
                         ? p.shadows.shadowManager->localShadowAtlasImage()
                         : VK_NULL_HANDLE,
                 .localShadowAtlasVisible = false,
                 .localShadowLayerCount = p.shadows.localShadowLayerCount});
        static_cast<void>(recordDeferredRasterDepthReadOnlyTransitionCommands(
            cmd, transitionPlan));
      });

  graph.addPass(RenderPassId::OitClear, {RenderPassId::DepthToReadOnly},
                [transparentOit](VkCommandBuffer cmd,
                                 const FrameRecordParams& p) {
                  static_cast<void>(transparentOit.recordClear(cmd, p));
                });

  graph.addPass(RenderPassId::Lighting, {RenderPassId::OitClear},
                [deferred](VkCommandBuffer cmd, const FrameRecordParams& p) {
                  static_cast<void>(recordForwardRasterLightingPassCommands(
                      cmd, p,
                      ForwardRasterLightingPassServices{
                          .framebufferExtent = deferred->swapchainExtent(),
                          .debugOverlay = deferred->debugOverlay()}));
                });

  graph.addPass(RenderPassId::ExposureAdaptation, {RenderPassId::Lighting},
                [deferred](VkCommandBuffer cmd, const FrameRecordParams& p) {
                  (void)cmd;
                  (void)p;
                  (void)deferred;
                });

  graph.addPass(RenderPassId::OitResolve, {RenderPassId::ExposureAdaptation},
                [transparentOit](VkCommandBuffer cmd,
                                 const FrameRecordParams& p) {
                  static_cast<void>(
                      transparentOit.recordResolvePreparation(cmd, p));
                });

  graph.addPass(RenderPassId::Bloom, {RenderPassId::OitResolve},
                [deferred](VkCommandBuffer cmd, const FrameRecordParams& p) {
                  if (!deferred->bloomManager() ||
                      !deferred->bloomManager()->isReady() ||
                      !deferred->bloomManager()->enabled()) {
                    return;
                  }
                  const VkImage sceneColorImage =
                      deferredRasterImage(p, DeferredRasterImageId::SceneColor);
                  const VkImageView sceneColorView = deferredRasterImageView(
                      p, DeferredRasterImageId::SceneColor);
                  if (sceneColorView == VK_NULL_HANDLE ||
                      sceneColorImage == VK_NULL_HANDLE) {
                    return;
                  }
                  const DeferredRasterSceneColorReadBarrierPlan readPlan =
                      buildDeferredRasterSceneColorReadBarrierPlan(
                          {.sceneColorImage = sceneColorImage});
                  static_cast<void>(
                      recordDeferredRasterSceneColorReadBarrierCommands(
                          cmd, readPlan));
                  const auto extent = deferred->swapchainExtent();
                  deferred->bloomManager()->dispatch(cmd, sceneColorView,
                                                     extent.width,
                                                     extent.height);
                });

  graph.addPass(RenderPassId::PostProcess, {RenderPassId::Bloom},
                [deferred](VkCommandBuffer cmd, const FrameRecordParams& p) {
                  static_cast<void>(recordDeferredPostProcessPassCommands(
                      cmd, deferredRasterPostProcessInputs(
                               p, *deferred,
                               {.transparentOitActive =
                                    deferred->isPassActive(
                                        RenderPassId::OitResolve)})));
                });
```

After adding passes, set resource access overrides:

```cpp
  graph.setPassResourceAccess(RenderPassId::Lighting,
                              {RenderResourceId::SceneGeometry,
                               RenderResourceId::CameraBuffer,
                               RenderResourceId::ObjectBuffer,
                               RenderResourceId::LightingData,
                               RenderResourceId::ShadowData,
                               RenderResourceId::LocalShadowData,
                               RenderResourceId::EnvironmentMaps},
                              {RenderResourceId::BimGeometry,
                               RenderResourceId::OitStorage},
                              {RenderResourceId::SceneColor});
```

Set readiness:

```cpp
  graph.setPassReadiness(
      RenderPassId::Lighting, [deferred](const FrameRecordParams& p) {
        return forwardRasterLightingReadiness(p, deferred->swapchainExtent());
      });
  graph.setPassReadiness(RenderPassId::OitClear,
                         [transparentOit](const FrameRecordParams& p) {
                           return transparentOit.readiness(p);
                         });
  graph.setPassReadiness(RenderPassId::OitResolve,
                         [transparentOit](const FrameRecordParams& p) {
                           return transparentOit.readiness(p);
                         });
```

End with:

```cpp
  graph.compile();
```

- [ ] **Step 2: Keep test expectations focused on graph shape**

The test must only assert that forward excludes G-buffer-only passes and includes the required high-level passes. Do not assert exact pass count.

- [ ] **Step 3: Verify graph tests pass**

Run:

```powershell
cmake --build out/build/windows-debug --target forward_raster_technique_tests
ctest --test-dir out/build/windows-debug -R forward_raster_technique_tests --output-on-failure
```

Expected: forward graph tests pass.

---

### Task 8: Editor Render Engine Selector

**Files:**
- Modify: `include/Container/utility/GuiManager.h`
- Modify: `src/utility/GuiManager.cpp`
- Modify: `tests/renderer/core/technique_debug_model_tests.cpp`

- [ ] **Step 1: Add failing GUI guardrail test**

In `tests/renderer/core/technique_debug_model_tests.cpp`, add:

```cpp
TEST(TechniqueDebugModelGuardrails, GuiExposesRenderEngineSelector) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiSource =
      readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(guiHeader, "struct RenderEngineOption"));
  EXPECT_TRUE(contains(guiHeader, "consumeRenderEngineChange"));
  EXPECT_TRUE(contains(guiHeader, "setRenderEngineOptions"));
  EXPECT_TRUE(contains(guiSource, "\"Rendering Engine\""));
}
```

Run:

```powershell
cmake --build out/build/windows-debug --target technique_debug_model_tests
ctest --test-dir out/build/windows-debug -R technique_debug_model_tests --output-on-failure
```

Expected: new guardrail fails.

- [ ] **Step 2: Add GUI state types and methods**

In `include/Container/utility/GuiManager.h`, include:

```cpp
#include "Container/renderer/core/RenderTechnique.h"
```

Add in `namespace container::ui`:

```cpp
struct RenderEngineOption {
  container::renderer::RenderTechniqueId id{
      container::renderer::RenderTechniqueId::DeferredRaster};
  std::string label{};
  bool available{true};
  std::string unavailableReason{};
};
```

Add public methods to `GuiManager`:

```cpp
  void setRenderEngineOptions(std::vector<RenderEngineOption> options,
                              container::renderer::RenderTechniqueId active);
  [[nodiscard]] std::optional<container::renderer::RenderTechniqueId>
  consumeRenderEngineChange();
  [[nodiscard]] container::renderer::RenderTechniqueId activeRenderEngine()
      const {
    return activeRenderEngine_;
  }
```

Add private members:

```cpp
  std::vector<RenderEngineOption> renderEngineOptions_{};
  container::renderer::RenderTechniqueId activeRenderEngine_{
      container::renderer::RenderTechniqueId::DeferredRaster};
  std::optional<container::renderer::RenderTechniqueId>
      pendingRenderEngineChange_{};
```

- [ ] **Step 3: Implement GUI methods**

In `src/utility/GuiManager.cpp`, add:

```cpp
void GuiManager::setRenderEngineOptions(
    std::vector<RenderEngineOption> options,
    container::renderer::RenderTechniqueId active) {
  renderEngineOptions_ = std::move(options);
  activeRenderEngine_ = active;
}

std::optional<container::renderer::RenderTechniqueId>
GuiManager::consumeRenderEngineChange() {
  auto pending = pendingRenderEngineChange_;
  pendingRenderEngineChange_.reset();
  return pending;
}
```

- [ ] **Step 4: Draw the combo**

In `GuiManager::drawSceneControls`, after the `ImGui::Separator();` before `Display`, add:

```cpp
  if (!renderEngineOptions_.empty()) {
    int activeEngineIndex = 0;
    for (int i = 0; i < static_cast<int>(renderEngineOptions_.size()); ++i) {
      if (renderEngineOptions_[static_cast<size_t>(i)].id ==
          activeRenderEngine_) {
        activeEngineIndex = i;
        break;
      }
    }
    const char* preview =
        renderEngineOptions_[static_cast<size_t>(activeEngineIndex)]
            .label.c_str();
    if (ImGui::BeginCombo("Rendering Engine", preview)) {
      for (int i = 0; i < static_cast<int>(renderEngineOptions_.size()); ++i) {
        const RenderEngineOption& option =
            renderEngineOptions_[static_cast<size_t>(i)];
        const bool selected = option.id == activeRenderEngine_;
        ImGui::BeginDisabled(!option.available);
        if (ImGui::Selectable(option.label.c_str(), selected) &&
            option.available && option.id != activeRenderEngine_) {
          pendingRenderEngineChange_ = option.id;
          statusMessage_ = "Render engine change pending: " + option.label;
        }
        ImGui::EndDisabled();
        if (!option.available && ImGui::IsItemHovered() &&
            !option.unavailableReason.empty()) {
          ImGui::SetTooltip("%s", option.unavailableReason.c_str());
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
  }
```

- [ ] **Step 5: Verify GUI guardrail passes**

Run:

```powershell
cmake --build out/build/windows-debug --target technique_debug_model_tests
ctest --test-dir out/build/windows-debug -R technique_debug_model_tests --output-on-failure
```

Expected: GUI guardrail passes.

---

### Task 9: RendererFrontend Runtime Technique Switching

**Files:**
- Modify: `include/Container/renderer/core/FrameRecorder.h`
- Modify: `include/Container/renderer/core/RendererFrontend.h`
- Modify: `src/renderer/core/RendererFrontend.cpp`
- Modify: `tests/renderer/core/technique_debug_model_tests.cpp`

- [ ] **Step 1: Add failing frontend guardrail test**

In `tests/renderer/core/technique_debug_model_tests.cpp`, add:

```cpp
TEST(TechniqueDebugModelGuardrails, RendererFrontendOwnsRuntimeTechniqueSwitch) {
  const std::string rendererFrontendHeader =
      readRepoTextFile("include/Container/renderer/core/RendererFrontend.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");

  EXPECT_TRUE(contains(frameRecorderHeader, "activeTechnique"));
  EXPECT_TRUE(contains(rendererFrontendHeader,
                       "pendingRenderTechniqueChange_"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "applyPendingRenderTechniqueChange"));
  EXPECT_TRUE(contains(rendererFrontend, "initializeRenderTechnique"));
  EXPECT_TRUE(contains(rendererFrontend, "syncGuiRenderEngineOptions"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "consumeRenderEngineChange"));
}
```

Run:

```powershell
cmake --build out/build/windows-debug --target technique_debug_model_tests
ctest --test-dir out/build/windows-debug -R technique_debug_model_tests --output-on-failure
```

Expected: new guardrail fails.

- [ ] **Step 2: Add active technique to frame params**

In `include/Container/renderer/core/FrameRecorder.h`, change `FrameRuntimeResources`:

```cpp
struct FrameRuntimeResources {
  uint32_t frameSlot{0};
  uint32_t imageIndex{0};
  RenderTechniqueId activeTechnique{RenderTechniqueId::DeferredRaster};
};
```

In `RendererFrontend::buildFrameRecordParams`, after assigning `frameSlot` and `imageIndex`, add:

```cpp
  p.runtime.activeTechnique =
      subs_.activeTechnique != nullptr ? subs_.activeTechnique->id()
                                       : RenderTechniqueId::DeferredRaster;
```

- [ ] **Step 3: Add frontend helper declarations**

In `include/Container/renderer/core/RendererFrontend.h`, add private members:

```cpp
  std::optional<RenderTechniqueId> pendingRenderTechniqueChange_{};
```

Add private helper declarations:

```cpp
  [[nodiscard]] RenderSystemContext renderSystemContext();
  void initializeRenderTechnique(RenderTechniqueId requested,
                                 std::string_view requestLabel);
  void requestRenderTechnique(RenderTechniqueId requested);
  void applyPendingRenderTechniqueChange();
  void syncGuiRenderEngineOptions();
```

- [ ] **Step 4: Implement technique context helper**

In `src/renderer/core/RendererFrontend.cpp`, add:

```cpp
RenderSystemContext RendererFrontend::renderSystemContext() {
  return RenderSystemContext{
      .frameRecorder = subs_.frameRecorder.get(),
      .deferredRaster = subs_.deferredRasterFrameGraphContext.get(),
      .deviceCapabilities = &subs_.deviceCapabilities,
      .sceneProviders = subs_.sceneProviderRegistry.get(),
      .frameResources = subs_.frameResourceRegistry.get(),
      .pipelines = subs_.pipelineRegistry.get(),
  };
}
```

- [ ] **Step 5: Factor technique initialization**

Add:

```cpp
void RendererFrontend::initializeRenderTechnique(
    RenderTechniqueId requested, std::string_view requestLabel) {
  if (!subs_.techniqueRegistry) {
    throw std::runtime_error("render technique registry is not initialized");
  }

  RenderSystemContext context = renderSystemContext();
  const RenderTechniqueSelection selection =
      subs_.techniqueRegistry->select(requested, context);
  subs_.activeTechnique = selection.technique;
  if (subs_.activeTechnique == nullptr) {
    throw std::runtime_error(
        "No available render technique; deferred raster fallback unavailable");
  }

  subs_.activeTechnique->registerTechniqueContracts(context);
  subs_.activeTechnique->buildFrameGraph(context);

  if (subs_.guiManager && subs_.frameRecorder) {
    DebugUiPresenter::publishRenderPasses(*subs_.guiManager,
                                          subs_.frameRecorder->graph());
  }

  if (selection.usedFallback) {
    container::log::ContainerLogger::instance().renderer()->warn(
        "Requested render technique '{}' unavailable ({}); using '{}'.",
        requestLabel, selection.unavailableReason,
        subs_.activeTechnique->name());
  } else {
    container::log::ContainerLogger::instance().renderer()->info(
        "Active render technique: {}", subs_.activeTechnique->name());
  }
}
```

Replace the existing inline selection block in `initialize()` with:

```cpp
  const RenderTechniqueId requestedTechnique =
      renderTechniqueIdFromName(svc_.config.renderTechnique)
          .value_or(RenderTechniqueId::DeferredRaster);
  initializeRenderTechnique(requestedTechnique, svc_.config.renderTechnique);
```

- [ ] **Step 6: Implement GUI option sync**

Add:

```cpp
void RendererFrontend::syncGuiRenderEngineOptions() {
  if (!subs_.guiManager || !subs_.techniqueRegistry) {
    return;
  }

  std::vector<container::ui::RenderEngineOption> options;
  RenderSystemContext context = renderSystemContext();
  for (const RenderTechniqueDescriptor& descriptor :
       knownRenderTechniqueDescriptors()) {
    RenderTechnique* technique = subs_.techniqueRegistry->find(descriptor.id);
    if (technique == nullptr && !descriptor.implemented) {
      continue;
    }

    container::ui::RenderEngineOption option{};
    option.id = descriptor.id;
    option.label = std::string(descriptor.displayName);
    if (technique == nullptr) {
      option.available = false;
      option.unavailableReason = "render technique is not registered";
    } else {
      const RenderTechniqueAvailability availability =
          technique->availability(context);
      option.available = availability.available;
      option.unavailableReason = availability.reason;
    }
    options.push_back(std::move(option));
  }

  subs_.guiManager->setRenderEngineOptions(
      std::move(options),
      subs_.activeTechnique != nullptr ? subs_.activeTechnique->id()
                                       : RenderTechniqueId::DeferredRaster);
}
```

Call `syncGuiRenderEngineOptions();` in `presentSceneControls()` before `subs_.guiManager->startFrame();`.

- [ ] **Step 7: Implement pending switch application**

Add:

```cpp
void RendererFrontend::requestRenderTechnique(RenderTechniqueId requested) {
  if (subs_.activeTechnique != nullptr &&
      subs_.activeTechnique->id() == requested) {
    return;
  }
  pendingRenderTechniqueChange_ = requested;
}

void RendererFrontend::applyPendingRenderTechniqueChange() {
  if (!pendingRenderTechniqueChange_) {
    return;
  }
  const RenderTechniqueId requested = *pendingRenderTechniqueChange_;
  pendingRenderTechniqueChange_.reset();
  initializeRenderTechnique(requested, renderTechniqueName(requested));
  if (subs_.guiManager) {
    subs_.guiManager->setStatusMessage(
        "Render engine active: " +
        std::string(renderTechniqueDisplayName(requested)));
  }
}
```

In `drawFrame`, after `concurrencyPolicy.waitBeforeAcquire(...)` and before MSAA recreation, add:

```cpp
  applyPendingRenderTechniqueChange();
```

In `presentSceneControls()`, after `drawSceneControls(...)` returns, add:

```cpp
  if (auto requested = subs_.guiManager->consumeRenderEngineChange()) {
    requestRenderTechnique(*requested);
  }
```

- [ ] **Step 8: Mirror runtime resources for active technique**

In `RendererFrontend::publishFrameRuntimeResourceBindings`, define:

```cpp
  const RenderTechniqueId activeTechnique =
      subs_.activeTechnique != nullptr ? subs_.activeTechnique->id()
                                       : RenderTechniqueId::DeferredRaster;
```

Update the manager binding copy so deferred resources are also published under active ForwardRaster when needed:

```cpp
  auto copyBindingAsTechnique = [runtime](const FrameResourceBinding& binding,
                                          RenderTechniqueId technique) {
    switch (binding.kind) {
    case FrameResourceKind::Image:
      runtime->bindImage(technique, binding.key.name, binding.frameIndex,
                         binding.image);
      break;
    case FrameResourceKind::Buffer:
      runtime->bindBuffer(technique, binding.key.name, binding.frameIndex,
                          binding.buffer);
      break;
    case FrameResourceKind::Framebuffer:
      runtime->bindFramebuffer(technique, binding.key.name,
                               binding.frameIndex, binding.framebuffer);
      break;
    case FrameResourceKind::DescriptorSet:
      runtime->bindDescriptorSet(technique, binding.key.name,
                                 binding.frameIndex, binding.descriptor);
      break;
    case FrameResourceKind::Sampler:
      runtime->bindSampler(technique, binding.key.name, binding.frameIndex,
                           binding.sampler);
      break;
    case FrameResourceKind::External:
      break;
    }
  };
```

Then in the `forEachBindingForFrame` callback:

```cpp
          copyBinding(binding);
          if (activeTechnique != RenderTechniqueId::DeferredRaster) {
            copyBindingAsTechnique(binding, activeTechnique);
          }
```

Update local descriptor/sampler/buffer bind lambdas so they bind to both DeferredRaster and active technique:

```cpp
    runtime->bindDescriptorSet(RenderTechniqueId::DeferredRaster,
                               std::move(name), imageIndex,
                               FrameDescriptorBinding{.descriptorSet = set});
```

becomes:

```cpp
    runtime->bindDescriptorSet(RenderTechniqueId::DeferredRaster, name,
                               imageIndex,
                               FrameDescriptorBinding{.descriptorSet = set});
    if (activeTechnique != RenderTechniqueId::DeferredRaster) {
      runtime->bindDescriptorSet(activeTechnique, std::move(name), imageIndex,
                                 FrameDescriptorBinding{.descriptorSet = set});
    }
```

Repeat the same dual-bind rule for sampler, camera buffer, and object buffer.

- [ ] **Step 9: Verify frontend guardrail passes**

Run:

```powershell
cmake --build out/build/windows-debug --target technique_debug_model_tests
ctest --test-dir out/build/windows-debug -R technique_debug_model_tests --output-on-failure
```

Expected: frontend guardrail passes.

---

### Task 10: End-To-End Verification

**Files:**
- No planned source edits.

- [ ] **Step 1: Build focused targets**

Run:

```powershell
cmake --build out/build/windows-debug --target shaders VulkanSceneRenderer_renderer render_technique_registry_tests resource_pipeline_registry_tests technique_debug_model_tests forward_raster_technique_tests
```

Expected: build succeeds.

- [ ] **Step 2: Run focused tests**

Run:

```powershell
ctest --test-dir out/build/windows-debug -R "render_technique_registry_tests|resource_pipeline_registry_tests|technique_debug_model_tests|forward_raster_technique_tests" --output-on-failure
```

Expected: all focused tests pass.

- [ ] **Step 3: Run full CPU validation suite likely affected by renderer contracts**

Run:

```powershell
ctest --test-dir out/build/windows-debug -R "render|renderer|validation" --output-on-failure
```

Expected: renderer and validation tests pass. If GPU visual-regression tests require unavailable hardware/runtime, record the exact skipped or failing test name and reason.

- [ ] **Step 4: Manual editor smoke test**

Run the app from the debug build with Sponza:

```powershell
.\out\build\windows-debug\Container.exe --model .\models\sponza\Sponza.gltf
```

Expected:
- Scene Controls shows `Rendering Engine`.
- Default engine is `Deferred raster` or the configured technique.
- Selecting `Forward rendering` updates the status text to pending, then active on the next frame.
- Render graph panel no longer lists `GBuffer`, `BimGBuffer`, `TileCull`, or `GTAO` while forward rendering is active.
- Shadows do not flash when a directional light is present.
- Switching back to deferred restores the deferred graph and G-buffer debug display.

- [ ] **Step 5: Performance smoke check**

Use the existing renderer telemetry panel:
- Compare GPU frame time in deferred vs forward rendering on the same Sponza camera.
- Confirm forward rendering omits G-buffer, tile cull, and GTAO timings.
- Confirm no repeated graph rebuild occurs while staying on one selected engine.

---

## Self-Review Notes

- Spec coverage: The plan adds a forward raster technique, editor switching, frontend runtime application, forward graph, direct forward opaque shader, and verification.
- Main architecture risk: shared shadow and post-process helpers still use deferred bridge names. The plan mitigates this by publishing shared runtime bindings under both DeferredRaster and ForwardRaster while the forward graph controls which passes execute.
- Performance risk: the first forward pass is not clustered Forward+. It should still be faster than deferred for many editor scenes because it skips G-buffer, tile cull, and GTAO. Clustered Forward+ can be a later plan built on this technique switch.
- Test coverage: registry, contract, graph-shape, UI guardrail, frontend guardrail, shader build, and manual editor smoke test are covered.
