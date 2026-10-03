# Third-party Notices for Windows Packages

VulkanSceneRenderer is distributed under the MIT license in `LICENSE`.

The package includes compiled code from EnTT, fmt, GLFW, GLM, Dear ImGui,
MaterialX, MikkTSpace, miniz, nlohmann/json, spdlog, stb, TinyEXR, TinyGLTF,
Vulkan headers/loader, Vulkan Memory Allocator, and TinyUSDZ. Copies of the
dependency license notices from the build are in `THIRD_PARTY_LICENSES`.
Slang compiler notices are included alongside those of the shader dependencies.
TinyUSDZ is Apache 2.0 licensed and contains additional third-party code;
its license files and bundled dependency notices are included separately.

The HDR environment `hdr/citrus_orchard_road_puresky_4k.exr` is
[Citrus Orchard Road (Pure Sky)](https://polyhaven.com/a/citrus_orchard_road_puresky)
from Poly Haven, distributed under
[CC0](https://polyhaven.com/license). Local validation scenes and generated
cube/triangle assets come from this repository. Downloaded Khronos,
buildingSMART, and OpenUSD sample collections are not included in the package.

The Microsoft Visual C++ runtime is a separate prerequisite, available from
[Microsoft](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
No Microsoft redistributable installer is bundled.
