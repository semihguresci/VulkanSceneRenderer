# DDGI device validation matrix

Run the matrix on a desktop Vulkan 1.4 device with a Release renderer and its
staged shaders. Build with tests enabled and build the CTest targets for DDGI,
ray-scene, and the raster lighting recorders in the same build directory. The
runner reports missing registrations or missing test binaries as failures.
Use Python 3.10 or newer with NumPy and Pillow, CTest, and a Vulkan SDK with
`vulkaninfo`, `spirv-val`, and Khronos validation.

On Developer PowerShell, for a Ninja Release build, run this from the
repository root.

```powershell
python -B tests/visual-regression/ddgi/run_device_matrix.py `
    --build-dir out/build/windows-release `
    --validation-layer "$env:VULKAN_SDK/Bin/VkLayer_khronos_validation.json" `
    --device 0 --output out/ddgi-device-matrix/adapter-0
```

On Linux, pass the installed validation layer manifest, for example
`/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json`. Override
`--exe` for a staged runtime and `--vulkaninfo`, `--spirv-val`, or `--ctest`
when the preferred SDK or build tool is outside `PATH`. Use a new output
directory for each run.

`--device` checks the adapter the renderer actually used against the expected
vulkaninfo index, UUID, or exact device name. The renderer keeps its normal
device selection. Use the Vulkan loader's driver selection environment when a
system exposes several ICDs. Two indistinguishable adapters require isolation
or capture telemetry with an unambiguous identity. The independent GPU contract
executables currently lack per-case device receipts, so the matrix reports
their coverage as untested when more than one adapter is visible. Native
captures still verify their own adapter identity. [Vulkan driver discovery and selection](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md)

## Coverage

| Phase | Evidence |
| --- | --- |
| CPU contracts | Registered CTest commands for probe placement, scheduling, scene transitions, ray scene, debug controls, captures, and both raster paths. |
| SPIR-V | CTest contract validation plus validation of the staged DDGI update, trace, and debug shaders for Vulkan 1.4. |
| GPU contracts | Production update, lookup, transport, material, and ray-visibility suites. The report preserves each GoogleTest result and skip reason. |
| Native correctness | Khronos synchronization validation for Forward and Deferred raster. Cases cover forced no-ray-query fallback, surface-budget rejection, TAA, probe overlays, async and serialized queue routes, resize, model reload, and thin and thick glass policies. |
| Release timing | Both raster techniques with GI off, paused, and low, default, and full-volume update budgets. The default uses 120 warmup frames, six settling frames, 90 measured frames, and three independent launches per mode. |

`--mode correctness` runs the contracts and validated images. `--mode timing`
runs the repeated Release sweep separately. `--mode cpu` runs CPU and shader
checks without opening a Vulkan device. Ordinary CPU CI still runs the existing
DDGI Python contracts without launching the matrix or requiring a GPU.

For a shorter smoke run, use `--warmup 48 --samples 4 --repeats 1 --width 240
--height 180`. The output records that workload separately from the default
repeated measurements. Capture readback affects GPU duty cycle, so the timing
report describes the measured render passes and allocations for that workload.

## Read the evidence

Start with `matrix.json`. It records the build, staged shader and model hashes,
arguments, relevant device environment, validation layer, per-phase outcomes,
and capture adapter matches. Keep the other files alongside it.

- `vulkaninfo.txt` retains the raw device report. `vulkan-inventory.json`
  indexes versions, feature flags, formats, sampler and descriptor limits,
  and queue families. Review its warnings when a tool omits a capability.
- `cpu/`, `contract-spirv/`, `production-spirv/`, and `gpu/` preserve commands'
  output and fresh GoogleTest XML where applicable.
- `correctness/` retains renderer logs, captures, telemetry, actual queue
  routes, and the reasons for unsupported paths. Capability fallback requires
  a usable raster capture with a reported reason and zero DDGI work.
- `timing/performance-results.json` contains raw samples and launch settings.
  `timing/timing-summary.json` records medians, percentiles, memory totals,
  and paired lighting comparisons. Timing captures require validation off.

A test with skipped cases carries those cases and their reasons. Mixed coverage
is marked `partial`. A timing-only run exits with status 2 when the device
reports DDGI unavailable. Missing artifacts, wrong adapters, wrong validation
modes, and execution errors fail the selected phase. A device absent from the
run has no result. Published
renderer validation on the RTX 2080 SUPER does not establish results for AMD,
Intel, or newer NVIDIA devices. [Cross-device tracking issue](https://github.com/semihguresci/VulkanSceneRenderer/issues/80)
