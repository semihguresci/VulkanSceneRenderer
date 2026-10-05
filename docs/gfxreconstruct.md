# Vulkan runtime debugging with GFXReconstruct

The optional launcher arms a Vulkan API capture before the renderer creates its
instance. You can select presented frame ranges or toggle capture while
navigating a scene. The output includes replayable GPU commands/resources,
renderer logs, shader identities, and a journal of lighting and temporal state.
Vulkan-Hpp RAII, VMA, Vulkan 1.4 dynamic rendering/synchronization2, GPU culling,
and Slang remain the renderer's implementation.

## Install and check

Install Python 3 and GFXReconstruct through the [Vulkan SDK](https://vulkan.lunarg.com/)
or a matching standalone build. The launcher finds tools through `--tools`,
`VULKAN_SDK`, then PATH. A Windows SDK typically stores the tools and
`VkLayer_gfxreconstruct.json` in its `Bin` directory. The renderer, capture DLL,
and inspection/replay executables must have matching architectures and tool
versions. Ordinary launches do not search for or enable the capture layer.

These examples use a Visual Studio Release build. In a new extracted package,
use `.\VulkanSceneRenderer.exe` instead of the build path; the launchers are
included in `tools`. The older v0.2.0 TAA preview predates this integration and
does not accept `--gfxrecon-session`.

```powershell
# Validate paths, versions, options and environment without launching/writing files.
.\tools\gfxreconstruct.ps1 capture `
  --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe `
  --tools C:/VulkanSDK/1.4.328.1/Bin --frames 9 --dry-run
```

The PowerShell wrapper invokes `python`. Set `CONTAINER_PYTHON` to an explicit
interpreter path if necessary. You can also run `python tools/gfxreconstruct.py`
with the same arguments on Windows or Linux (ELF tool/manifest discovery is
supported; the documented GPU validation currently targets Windows).

## Capture frames after TAA warmup

```powershell
.\tools\gfxreconstruct.ps1 capture `
  --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe `
  --frames 9 --output-dir out/captures/cornell-taa `
  '--' --hidden --no-ui --validation --taa --msaa 1 `
  --display-mode lit `
  --width 640 --height 480 --fixed-dt 0.016666667 `
  --capture-frame 9 --screenshot F:/Projects/Container/out/cornell-original.png
```

Quote the `'--'` separator in PowerShell: it separates launcher options from
renderer arguments. With Python directly, an unquoted `--` works. Use absolute
paths for external scenes, capture sequences, and original screenshots: the
launcher runs the child from the executable directory so packaged assets resolve.

`--frames 9-12,20` records separate trimmed captures for the selected ranges.
Use `--capture-name cornell-shadow` to change the filename stem; frame/trigger
suffixes are added by GFXReconstruct. The default stem is `capture`.
The renderer exits after the last selected **present call**, letting its normal
cleanup and journal finish. Screenshots/capture sequences can also terminate it;
ensure their duration reaches the desired presentation boundary. `--all-frames`
records until the application exits and can create much larger files.

The output directory must be new or empty. A unique directory under
`captures/gfxrecon` is generated when none is supplied. Existing artifacts are
never overwritten by a repeated launcher invocation. Settings are applied only
to the child process; existing layer paths and validation configuration are
preserved. The renderer explicitly enables the capture layer independently of
`--validation`; `--no-validation` captures also work.

## Capture an artifact interactively

```powershell
.\tools\gfxreconstruct.ps1 capture `
  --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe `
  --trigger F3 --output-dir out/captures/interactive `
  '--' --taa --msaa 1 --render-technique deferred-raster
```

Capture is armed when the scene opens. Focus the renderer window and press F3
to start recording, reproduce the artifact, then press F3 to stop. Repeat during the
same session. `--trigger-frames 1` records one frame per trigger instead.
Hotkey captures retain the layer's filename timestamps so later recordings
preserve earlier files. GFXReconstruct 1.0.5 timestamps have one-second
resolution; start recordings at least one second apart. Frame-range captures
retain deterministic filenames.
F3 is the default when no explicit mode is supplied. F1–F5 and F9–F11 are
accepted; F12 is accepted only outside Windows. F6/F7/F8 change renderer debug
views/culling, and TAB/CONTROL conflict with UI/navigation, so they are excluded.
The renderer's **Vulkan runtime capture** panel
shows mode, key, and output path. The layer's `layer.log` provides recording
feedback: the UI does not invent recording/completion state from keypresses.

Capture must be armed at startup; enabling a layer after instance creation is
not supported. Capture intercepts Vulkan, while CPU/import debugging still uses
the application logs and Visual Studio debugger.

## Inspect and replay

Use the actual `.gfxr` filename produced in the output directory:

```powershell
.\tools\gfxreconstruct.ps1 replay out/captures/cornell-taa/capture_frame_9.gfxr `
  --output-dir out/captures/cornell-replay --screenshots 1 --offscreen --validation
```

The helper runs `gfxrecon-info` and replay, retaining logs and exit codes.
`--screenshots 1-4` exports PNGs; omit `--offscreen` to show replay in a window.
Add `--extract-shaders` to write SPIR-V into `shaders`, or `--json-lines` for API
inspection in `api.jsonl`. Unsupported installed-tool options fail clearly.
Inspection/replay disable inherited capture settings so they do not capture
themselves. GFXReconstruct is not linked into the renderer or redistributed with
the package: install its tools separately.

Start with the original GPU/driver. Replay on another GPU requires compatible
Vulkan features, limits, formats, memory and extensions; this workflow does not
guarantee cross-device portability. Vulkan 1.4 header support alone is not proof
that all renderer features replay correctly. Keep the recorded tool version.

The helper defaults to `--memory-translation rebind`, using GFXReconstruct's
VMA allocator during replay. On the installed 1.0.5 tools, the default `none`
allocator emits misaligned mapped-memory flushes while restoring a trimmed
capture. `rebind` avoids those diagnostics; `--memory-translation none` remains
available for investigating the original allocation behavior. Replay errors
in the tool log produce a failing helper exit code even if the tool returns 0.

GFXReconstruct 1.0.5 also has a trimmed-state limitation with multisampled integer
attachments: its injected copy shader/barriers produce validation errors.
For MSAA use `--frames 1-9` (replay screenshot 9) or `--all-frames`; these record
creation and rendering without a late state snapshot. Late ranges and hotkeys
with MSAA are rejected on that version with an actionable message. This does
not change the renderer's MSAA configuration. Late TAA captures use 1x samples.

## Artifacts and frame numbering

| File | Contents |
| --- | --- |
| `session.json` | Tool/layer version, settings, command, executable SHA-256 and compiled SPIR-V SHA-256 identities |
| `runtime.json` | Compiled revision/configuration/compiler, GPU/driver/API, initial render/lighting/shadow/camera/AA state |
| `frames.jsonl` | CPU ticks (including skips), acquisition failures, successful renderer submissions, and actual/effective presentation results with temporal/scene state |
| `renderer.log`, `layer.log` | Renderer/validation and capture-layer diagnostics |
| `result.json` | Child exit code, elapsed wall time and capture sizes; file existence does not prove valid replay |
| `*.gfxr` | Captured Vulkan calls and resources; separate trims for ranges/triggers |

GFXReconstruct range selection counts presentation boundaries starting at 1.
The journal's `presentCalls` counts actual `vkQueuePresentKHR` calls, including
failures/suboptimal results. `successfulSubmissions` counts **renderer draw**
`vkQueueSubmit2` calls, not initialization/upload submissions. A CPU tick can
skip rendering or retry acquisition, so it is not a capture frame number.
Swapchain `imageIndex` and `frameSlot` are separate fields, unrelated to this
sequence. The per-present `telemetry.taa.submittedFrame`, epoch/reset reason,
camera jitter and matrices link to the captured history. Replay frame 1 of a
trim beginning at original frame 9 corresponds to original presentation 9.
Injected lifecycle tests retain both actual and effective presentation results.
GPU timings in telemetry retain their existing readback latency.
Light vectors retain the GPU struct field names and components, including
point/spot cone limits and area-light shape/size. Shadow bias, filter/contact
settings and the HDR environment identifier are recorded while capture is armed.
Build revision is evaluated at CMake configuration time; reconfigure after a
commit before building a capture intended to identify that commit.

Attach the trim, sidecars, logs, original screenshot and replay screenshot to a
bug report. Capture output is ignored by Git. Keep large traces outside source
history; bound ranges and select compression with `--compression LZ4|ZLIB|ZSTD|NONE`.

## Mapped memory and Visual Studio

Windows reserves F12 for the debugger. Pressing it with Visual Studio attached
can pause the app on a breakpoint (`0x80000003`), which can look like a crash.
Use F5/Continue to resume and F3 for capture; the launcher rejects F12 on Windows.
See [Microsoft's debugger reservation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey#remarks).
An ordinary Visual Studio launch does not arm GFXReconstruct. To debug with
capture, launch through the capture helper, then attach Visual Studio to the
renderer process. Keep the renderer window focused when using the trigger.

Default `--memory-mode page_guard` tracks mapped writes, including host-coherent
and persistent VMA mappings. It can use handled access violations on Windows,
which a debugger may intercept. Inspect the fault and layer stack; do not
assume an unrelated application access violation is harmless. For easier
debugger use, select `--memory-mode unassisted`, which copies mapped data on
submission/unmap and may cost more time and disk space. `assisted` is excluded:
the renderer does not explicitly flush every host-coherent write.

If no capture appears, check layer discovery, the selected present range,
process filter, hotkey focus/conflicts, and logs. A failed renderer initialization
is visible in `renderer.log`; missing SDK/tools fail before launching.

## Verification

```powershell
python tests/validation/gfx_capture_launcher_tests.py
ctest --test-dir out/build/visual-studio -C Release -R 'gfx_capture_(tests|launcher_tests)' --output-on-failure
$env:CONTAINER_RUN_GPU_GFXRECON = '1'
python tests/validation/gfx_capture_replay_smoke.py `
  --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe `
  --tools C:/VulkanSDK/1.4.328.1/Bin --output out/capture-validation
```

The GPU test is opt-in and skips clearly without the flag, tools, Pillow, or a
device/runtime meeting the renderer's Vulkan 1.4 requirements. Other capture,
initialization, validation, and replay failures fail the test.
Add `--matrix` to check both techniques with native/TAA/MSAA, BIM/mixed scenes,
textured PBR assets, and camera motion, skipped ticks, acquisition retry, reset,
reload, and resize. Use `--matrix --case lifecycle` to select an individual case.
Add `--memory-mode unassisted` to check the debugger-friendly tracking mode.
It captures a single frame after eight TAA submissions, replays with validation,
and compares original/replay RGB with normalized MAE <= 1/255 and maximum channel
error <= 4/255. Keep `comparison.json` as evidence. Capture size and elapsed wall
time are recorded, including initialization and shutdown. Use comparable
uncaptured runs when measuring capture overhead.

### Observed Windows validation, 2026-10-04

Visual Studio Release (MSVC 19.51.36260.0), NVIDIA GeForce RTX 2080 SUPER,
Vulkan device API 1.4.325, driver version field 2480242688, SDK 1.4.328.1,
GFXReconstruct 1.0.5 (614fbd3). Tests started at 320x240 using the default
diagnostic display, a fixed 1/60-second step, LZ4 and `page_guard`; replay used
`rebind` with validation enabled. The lifecycle sequence resized to 360x256.

| Case | Capture selection | Trace size, bytes | Replay RGB error |
| --- | --- | ---: | --- |
| Deferred, TAA | 9 | 19,798,201 | MAE 0, maximum 0 |
| Deferred, native | 9 | 18,999,303 | MAE 0, maximum 0 |
| Forward, TAA | 9 | 20,013,927 | MAE 0, maximum 0 |
| Forward, native | 9 | 19,214,158 | MAE 0, maximum 0 |
| Deferred, MSAA 4x | 1-9 | 111,123,565 | MAE 0, maximum 0 |
| Forward, MSAA 4x | 1-9 | 111,074,095 | MAE 0, maximum 0 |
| BIM, TAA | 9 | 22,367,369 | MAE 0, maximum 0 |
| Mixed glTF/BIM, TAA | 9 | 22,153,968 | MAE 0, maximum 0 |
| Deferred, textured PBR cube, TAA | 9 | 23,327,366 | MAE 0, maximum 0 |
| Forward, textured PBR cube, TAA | 9 | 19,831,697 | MAE 0, maximum 0 |
| Camera/skip/acquire/reset/resize/reload, TAA | 6-17 | 22,074,251 | MAE 0, maximum 0 at four samples |

Cornell cases exercised authored area lighting and local shadows; BIM/mixed
cases exercised directional and HDR environment lighting. All listed captures
and replays completed without validation errors. API JSON export contains
compute dispatches, indirect-count draws, dynamic rendering and synchronization2
submissions. Shader extraction and JSON Lines conversion completed successfully.
The lifecycle test distinguishes 18 CPU ticks from 17 presentations and checks
TAA submission identity across skipped work and acquisition retries.
The cube cases exercised five bindless material textures: base color, metallic/
roughness, normal, occlusion and emissive. Two separate frame selections in one
session (`9,12`, with a custom filename stem) also replayed with exact matches.
Device creation/replay includes the enabled buffer-device-address feature, but
the current raster workloads do not call `vkGetBufferDeviceAddress`. These
results do not demonstrate relocation of shader-consumed device addresses.

The same lifecycle sequence passed with `unassisted`: 32,286,194 bytes, four
exact screenshot matches, and no validation errors. It took about 89 seconds
including initialization/shutdown, while the page-guard run took 4.72 seconds;
these runs had different background build activity and are not a controlled
performance comparison.

A copied runtime with paths containing spaces ran normally and captured with
validation disabled, after removing SDK/layer environment settings. The package
launcher used an explicit installed tool path; three nine-frame original/capture
pairs had identical pixels, and replay with validation also matched exactly.
Median child wall time was 2.68 seconds normally and 4.41 seconds with capture,
with traces around 19.8 MB. These short runs include initialization, screenshots,
snapshot creation and shutdown, so they do not measure interactive frame-rate
overhead. The installed SDK remained on the development machine.

Capture validation found and fixed two application defects: object descriptors
used padded VMA allocation sizes instead of the logical buffer range, and scene
reload could leave shadow-cull descriptors bound to a destroyed object buffer.
The lifecycle regression exercises the latter with capture armed.
Texture testing also found a malformed embedded buffer in the generated PBR
cube: a missing normal and damaged UV encoding prevented glTF loading. The
840-byte buffer now contains all 24 positions/normals/UVs and 36 indices; the
generator validates its base64 encoding and declared size before writing it.

Interactive F3 start/stop and repeated recording were exercised in a Windows
session. That check exposed same-session overwrite when filename timestamps
were disabled; the launcher now enables timestamps for hotkey mode. Two
one-frame F3 captures from the same TAA session produced separate files
(25,995,286 and 26,000,505 bytes). Both were inspected and replayed with
validation and screenshot export. Local evidence is under
`out/capture-validation/hotkey-preserve-20261004/`.

References: [LunarG Part 2 setup guide](https://www.lunarg.com/mastering-gfxreconstruct-part-2/),
[Vulkan usage](https://github.com/LunarG/gfxreconstruct/blob/dev/USAGE_desktop_Vulkan.md),
[SDK 1.4.328.1 Windows capture tools](https://vulkan.lunarg.com/doc/view/1.4.328.1/windows/capture_tools.html).
