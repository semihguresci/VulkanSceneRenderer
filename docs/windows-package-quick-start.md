# VulkanSceneRenderer Windows x64 Preview

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
