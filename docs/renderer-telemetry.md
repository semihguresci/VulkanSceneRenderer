# Renderer Telemetry

Renderer telemetry captures per-frame CPU timing, GPU pass timing, render graph
state, synchronization state, workload size, culling counters, and resource
capacity. It is intended for live debugging first, with enough structure to be
useful in automated smoke runs.

## Runtime View

The live ImGui window is named `Renderer Telemetry`. It shows:

- FPS, CPU frame time, known GPU time, and CPU p95 over the recent history.
- Current frame index, swapchain image index, frame slot, and max frames in
  flight.
- GPU timing source and profiler status.
- Swapchain dimensions and image count.
- Concurrency mode and the reason serialized frame submission is still active.
- CPU phase timings for frame wait, readbacks, image acquire, image fence wait,
  resource growth, GUI, scene update, descriptor updates, command recording,
  queue submit, screenshot capture, and present.
- Workload counts: objects, draw calls, submitted lights, and OIT node
  capacity.
- Culling counters for frustum and occlusion results.
- Light clustering counters, including active clusters, max lights per cluster,
  dropped light references, and the legacy tile-light timing fallback.
- Render graph pass state, including enabled/active/skipped state, skip reason,
  CPU record time, GPU timing availability, and blocker resource or pass.
- Synchronization counters for swapchain recreates and `vkDeviceWaitIdle`
  calls.
- The **Asset streaming** section shows memory and staging peaks, geometry LOD,
  texture residency, and pending copies. Its viewport markers and the capture
  fields for startup, cache events, and AS builds are described in
  [Derived asset cache and streaming diagnostics](derived-asset-cache.md).

## Code Ownership

Telemetry is split across focused components:

- `RendererTelemetry` stores frame snapshots, history, summary statistics, pass
  timing, culling data, resource data, and profiler status.
- `RenderPassGpuProfiler` owns Vulkan query pools for pass-level GPU timing.
- `FrameRecorder` wraps active render graph passes with CPU timing hooks and GPU
  query begin/end hooks.
- `RendererFrontend` records frame-level CPU phases, collects previous-frame GPU
  results after the relevant fence wait, and publishes telemetry to the GUI.
- `GuiManager` renders the live telemetry window.

The renderer still runs with serialized GPU resource concurrency. Telemetry
surfaces this explicitly so frame-time numbers are not mistaken for true
multi-frame-in-flight behavior.

## GPU Timing Backends

`RenderPassGpuProfiler` tries backends in this order:

1. `VK_KHR_performance_query`
2. Vulkan timestamp queries
3. No pass-level GPU timing

The performance query backend is used only when all of these are true:

- The physical device exposes `VK_KHR_performance_query`.
- `performanceCounterQueryPools` is supported and enabled.
- Required extension entry points are available.
- The graphics queue family exposes performance counters.
- A nanosecond counter is present.
- The selected counter requires exactly one counter pass.
- The profiling lock can be acquired.
- The performance query pool can be created.

If any requirement fails, the profiler falls back to timestamp queries when the
graphics queue family has valid timestamp bits and a timestamp query pool can be
created.

The selected backend and fallback reason are visible in the telemetry window and
logged at startup, for example:

```text
GPU pass profiler: using timestamp queries; performance queries unavailable: device does not expose VK_KHR_performance_query
```

Performance query readback uses `vkGetQueryPoolResults` without availability,
wait, partial, or 64-bit result flags, because those flags are not valid for
`VK_QUERY_TYPE_PERFORMANCE_QUERY_KHR`. Timestamp readback uses 64-bit results
with availability flags.

## Data Latency

GPU pass timings are collected at the start of the next frame after the frame
fence wait. The GUI reports the GPU result latency in frames. With the current
serialized frame policy, the expected latency is one completed frame after the
first frame has produced queries.

The pass table distinguishes between missing GPU data and a valid zero-duration
query result. A pass with a valid GPU query result displays `0.000`; a pass
without GPU timing displays `-`.

## Validation

Build and run focused checks:

```powershell
cmake --build out/build/windows-release --target VulkanSceneRenderer renderer_telemetry_tests render_graph_tests --config Release
ctest --test-dir out/build/windows-release --output-on-failure -R 'renderer_telemetry_tests|render_graph_tests|rendering_convention_tests'
```

Run a bounded hidden smoke test that exercises Vulkan startup, frame submission,
query backend selection, screenshot readback, and shutdown:

```powershell
.\out\build\windows-release\VulkanSceneRenderer.exe `
  --hidden --no-ui `
  --model models\basic_triangle.gltf `
  --width 320 --height 240 `
  --warmup-frames 2 --capture-frame 3 `
  --screenshot out\build\windows-release\telemetry_smoke.png
```

On hardware that does not expose `VK_KHR_performance_query`, this command should
still complete through the timestamp-query fallback.

## Adding New Metrics

Prefer adding metrics at the owner of the data:

- Frame lifecycle CPU timing belongs in `RendererFrontend::drawFrame`.
- Pass CPU and GPU timing belongs in `FrameRecorder` hooks or
  `RenderPassGpuProfiler`.
- Render graph status belongs in `RenderGraph` and is copied into
  `RendererTelemetry::setRenderGraph`.
- Resource capacities should be sampled in `RendererFrontend` after the resource
  owner has been updated for the frame.
- GPU readback counters should be staged by the resource owner and collected
  only after the frame fence that protects the readback has completed.

Keep the telemetry snapshot plain-data oriented. GUI labels and table layout
belong in `GuiManager`, not in the telemetry storage type.

## Current Limits

- `VK_KHR_performance_query` is hardware and driver dependent. Unsupported
  devices use timestamp queries automatically.
- Multi-pass performance counters are intentionally skipped for now; supporting
  them requires multiple submissions per frame with
  `VkPerformanceQuerySubmitInfoKHR::counterPassIndex`.
- The profiler selects the first nanosecond performance counter. More detailed
  vendor counters can be added later as named metrics.
- CPU phase timings include CPU-side waiting caused by the current serialized
  frame policy.
- The telemetry history is in-memory only and is not exported to disk.
