# VulkanSceneRenderer Windows x64 Preview

New packages include optional capture launchers in `tools` and a
`GFXRECONSTRUCT.md` guide. Vulkan API capture/replay requires a separate
GFXReconstruct installation and Python 3; normal rendering needs neither.

New source-built packages also include optional ray shadows:
`VulkanSceneRenderer.exe --ray-shadows soft --ray-shadow-samples 8`.
Use `--ray-shadows hard` for hard visibility, or `--no-ray-query` to force raster
fallback. Ray support is optional; Vulkan 1.4 alone does not imply it. The Shadows
panel reports support, memory and build/query/filter timings. Transparent
receivers retain raster shadows; blended/transmissive blockers do not cast binary
ray shadows. Height-displaced meshes trigger raster fallback.

Rectangular and disk lights use LTC integration when the packaged lookup tables
are valid. Compare the integration modes from the extracted directory:

```powershell
.\VulkanSceneRenderer.exe --area-lighting ltc
.\VulkanSceneRenderer.exe --area-lighting sampled --area-light-samples 25
```

`--area-light-samples` accepts 9, 25 or 64 for sampled integration and LTC
fallback; it is independent of shadow quality and ray sample counts. Keep
`materials/ltc` beside the executable. Missing or invalid tables automatically
select sampled lighting; Lighting Settings and capture telemetry report the
effective mode and fallback reason. See the included
[LTC guide](ltc-area-lighting.md)
for approximation limits and measured GPU costs.

Try temporal anti-aliasing with `VulkanSceneRenderer.exe --taa --msaa 1 --display-mode lit`.
Use `--render-technique forward-raster` for the forward path. TAA currently
supports native-resolution rigid surfaces at 1x samples; transparent and emissive
pixels use current color. Existing MSAA remains available with `--no-taa`.

From the extracted directory, this reproducible motion capture also works without
build tools:

```powershell
.\VulkanSceneRenderer.exe --hidden --no-ui --no-validation --taa --model models/validation/taa_scene.gltf --width 640 --height 360 --capture-sequence models/validation/taa_object.json --screenshot motion.png --fixed-dt 0.016666667
```

Selected frames produce PNGs and effective-setting/timing JSON sidecars. Debug
views include `taa-velocity`, `taa-age`, `taa-rejection`, `taa-blend`, and
`taa-reactive` through `--display-mode`.

Extract the entire ZIP before starting `VulkanSceneRenderer.exe`. Keep the DLLs,
`spv_shaders`, `materials`, `models`, and `hdr` folders beside the executable.

You need Windows 10 or 11 x64, a GPU with a Vulkan 1.4 driver, and the latest
Microsoft Visual C++ v14 Redistributable (x64):
https://aka.ms/vc14/vc_redist.x64.exe

Visual Studio, the Vulkan SDK, Python, and Slang are not required to run this
package. Install the Vulkan SDK only if you want optional `--validation` checks.
Update your GPU driver if startup reports missing Vulkan features. If Windows
reports missing `MSVCP140.dll` or `VCRUNTIME140.dll`, install the redistributable.
The application is unsigned; Windows may display a download warning.

Double-click the executable to open the Cornell box local-light sample.
From PowerShell in the extracted folder:

```powershell
.\VulkanSceneRenderer.exe
.\VulkanSceneRenderer.exe --model "C:\Models\scene.glb"
.\VulkanSceneRenderer.exe --render-technique forward-raster --msaa 4
.\VulkanSceneRenderer.exe --model models/validation/open_shadow_wall_blocker.gltf
```

For `.gltf` input, retain referenced textures and binary buffers. BIM and USD
files can be opened with `--model` or `--bim-model`. Large external model
collections shown in the project gallery are not included in this package.
The renderer performs startup checks for its required Vulkan 1.4 features;
the API version alone does not guarantee that a device supports every feature.

This is a preview for testing. Report bugs with the version and source commit
from `build-info.json`, your GPU and driver version, launch arguments, and any
console output or screenshot:
https://github.com/semihguresci/VulkanSceneRenderer/issues

The repository is MIT licensed. Dependency licenses are in
`THIRD_PARTY_LICENSES`; see `THIRD_PARTY_NOTICES.md` for asset attribution.
