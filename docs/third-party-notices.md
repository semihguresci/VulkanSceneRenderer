# Third-party Notices for Windows Packages

VulkanSceneRenderer is distributed under the MIT license in `LICENSE`.

The package includes compiled code from EnTT, fmt, GLFW, GLM, Dear ImGui,
Mapbox Earcut, Manifold, Clipper2, MaterialX, MikkTSpace, miniz, nlohmann/json, spdlog, stb, TinyEXR, TinyGLTF,
Vulkan headers/loader, Vulkan Memory Allocator, and TinyUSDZ. Copies of the
dependency license notices from the build are in `THIRD_PARTY_LICENSES`.
Slang compiler notices are included alongside those of the shader dependencies.
The LTC fitter and polygon integration adapt
[selfshadow/ltc_code](https://github.com/selfshadow/ltc_code), Copyright 2017
Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt. Its permissive license
requires retention of its notice and a citation to *Real-Time Polygonal-Light
Shading with Linearly Transformed Cosines*, ACM SIGGRAPH 2016. The complete
notice is included with the fitted assets in `materials/ltc/LICENSE.txt`.
The lookup tables are regenerated for this renderer's BRDF; they are not the
original Smith-GGX fit. See [LTC area lighting](ltc-area-lighting.md).
TinyUSDZ is Apache 2.0 licensed and contains additional third-party code;
its license files and bundled dependency notices are included separately.

Manifold 3.5.4 (Copyright 2021 The Manifold Authors) is Apache-2.0 licensed.
Its vcpkg build uses Clipper2 2.0.1 (Copyright Angus Johnson 2010-2025),
licensed under Boost Software License 1.0. Their license texts are included as
`Manifold-Apache-2.0.txt` and `Clipper2-Boost-1.0.txt`. Mapbox Earcut 3.2.4 is
ISC licensed and its notice is included as `Earcut-ISC.txt`. Manifold's shared
library is staged beside Windows executables and included in release packages.

The HDR environment `hdr/citrus_orchard_road_puresky_4k.exr` is
[Citrus Orchard Road (Pure Sky)](https://polyhaven.com/a/citrus_orchard_road_puresky)
from Poly Haven, distributed under
[CC0](https://polyhaven.com/license). Local validation scenes and generated
cube/triangle assets come from this repository. Downloaded Khronos,
buildingSMART, and OpenUSD sample collections are not included in the package.

The Microsoft Visual C++ runtime is a separate prerequisite, available from
[Microsoft](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
No Microsoft redistributable installer is bundled.
