# Architecture

Runtime ownership is intentionally layered:

```text
Application
  -> VulkanContextInitializer / window setup
  -> RendererFrontend
  -> subsystem managers
  -> FrameRecorder render graph
```

`RendererFrontend` owns renderer lifetime and frame submission. `FrameRecorder`
owns command-buffer pass order. Specialized managers own Vulkan resources for
lighting, shadows, frame resources, culling, OIT, bloom, environment maps, and
scene data. `BimManager` owns sidecar geometry and draw data so IFC, IFCX,
dotbim, and USD/USDZ content can be rendered without being folded
into the primary glTF scene buffers.

## Vulkan 1.4 Rendering

The renderer requires Vulkan 1.4, dynamic rendering, synchronization2,
maintenance5/6, bindless descriptor indexing, buffer device addresses,
multi-draw indirect, and indirect draw counts. Startup rejects unsupported
loaders/devices before creating rendering resources. Slang is the shader source
language and emits SPIR-V 1.6, including helper-invocation demotion for discard.

`RenderingPass` and `RenderingTarget` are CPU attachment descriptions. They
replace Vulkan render-pass and framebuffer objects throughout the engine's
pass contracts, resource bindings, pipelines, picking, shadows, and UI.
`DynamicRendering.cpp` records `vkCmdBeginRendering`/`vkCmdEndRendering` and
explicit synchronization2 attachment transitions. Pipelines declare formats
with `VkPipelineRenderingCreateInfo`; secondary command buffers inherit the
same formats and sample count. Color and reverse-Z depth resolves preserve the
existing MSAA behavior. ImGui uses its dynamic-rendering backend.

Instance, device, surface, and debug-messenger lifetime use Vulkan-Hpp RAII.
`VulkanObjects.cpp` owns the remaining engine-created Vulkan objects with
`vk::raii` handles, grouped by device. Managers explicitly release them during
recreation; device teardown also releases surviving objects after waiting idle.
Borrowed C handles preserve interoperability with VMA and command recorders.
Buffers/images allocated by VMA retain their allocator's lifetime management;
descriptor sets and command buffers belong to their RAII-owned pools. The
bundled VMA uses its Vulkan 1.3 dispatch table on the Vulkan 1.4 device.

Forward opaque rendering now schedules frustum culling before its depth
prepass, builds Hi-Z from the resolved depth, and consumes occlusion-culled
indirect commands for lighting. The deferred path keeps the same existing
compute culling pipeline. Single-sided scene draws use GPU indirect counts;
mirrored/double-sided routes, transparency, and debug draws keep their explicit
draw routing. Culling outputs are optional lighting inputs: disabling frustum
or occlusion culling retains CPU or frustum-only draws. Both paths schedule
culling statistics readback, including fallback counts when a culling stage is
disabled. BIM retains its GPU visibility/compaction path.

This is a modern raster architecture, not a claim of complete state-of-the-art
image quality. The existing sampled area lighting, shadow maps, screen-space
occlusion, environment lighting, and bounce approximation remain raster
techniques. Hardware ray tracing, mesh/task shading, virtualized geometry,
temporal upscaling, and production global illumination are further projects,
not implied by an API version upgrade.

## Frame Flow

```text
GPU frustum cull
  -> Depth prepass
  -> BIM depth prepass
  -> Hi-Z / occlusion cull
  -> G-buffer
  -> BIM G-buffer
  -> shadow cascades
  -> tile light cull
  -> GTAO
  -> deferred lighting + transparent OIT
  -> bloom
  -> post-process + debug UI
```

## Ownership Boundaries

- `Application` owns window lifetime and drives the main loop.
- `VulkanContextInitializer` builds the Vulkan instance, device, queues, and
  surface-dependent context.
- `RendererFrontend` wires together managers, frame resources, pipelines, scene
  state, synchronization, and per-frame submission.
- `FrameRecorder` records passes in render-graph order and keeps Vulkan layout
  transitions near the passes that require them.
- `SceneController` owns the shared ECS `World`. Renderable draw extraction,
  active camera data, and generated point lights are mirrored through that
  registry for renderer-side queries.
- `FrameResourceManager` owns per-swapchain-image attachments, descriptor sets,
  samplers, and OIT storage.
- `SceneManager` owns model loading, scene descriptors, materials, texture
  resources, and draw data.
- `BimManager` owns `.bim`, `.ifc`, `.ifcx`, `.usd`, `.usda`, `.usdc`, `.usdz`, and
  fallback glTF sidecar loading, GPU buffers, sidecar object data, and BIM draw
  lists.
- `LightingManager`, `ShadowManager`, `GpuCullManager`, `EnvironmentManager`,
  `OitManager`, and `BloomManager` own their focused rendering resources.

Prefer adding new behavior to the narrowest manager that already owns the data
being mutated. Keep `RendererFrontend` as orchestration code rather than a home
for feature-specific Vulkan resources.

## MSAA

The planned motion-vector and TAA work follows
[Temporal rendering contracts](temporal-rendering.md): stable submitted-frame
snapshots, unjittered physical motion, separate raster/color history grids, and
a focused temporal owner. The first delivery supports TAA at 1x samples;
combining it with MSAA requires a later explicit design. TAA is not yet enabled.

Deferred raster MSAA is documented in [MSAA](msaa.md). The short version:
multisampled depth and G-buffer attachments are private `FrameResourceManager`
resources, while the render graph and downstream passes continue to consume the
resolved single-sample depth and G-buffer images.

Forward opaque lighting uses the original multisampled prepass depth and a
matching multisampled HDR color target. It resolves shaded color before a
separate single-sample transparency scope that loads the opaque result. OIT and
post-processing retain their single-sample resources.

## BIM GPU Filtering

The BIM renderer uses GPU visibility masks and compacted indirect draw streams
for large model filtering and LOD. The safety and performance contracts are
documented in [BIM GPU Visibility And Draw Compaction](bim-gpu-visibility-compaction.md).
