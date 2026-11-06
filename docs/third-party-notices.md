# Third-party Notices for Windows Packages

VulkanSceneRenderer is distributed under the MIT license in `LICENSE`.

The package includes compiled code from EnTT, fmt, GLFW, GLM, Dear ImGui,
Mapbox Earcut, Manifold, Clipper2, MaterialX, MikkTSpace, miniz, nlohmann/json, spdlog, stb, TinyEXR, TinyGLTF,
Vulkan headers/loader, Vulkan Memory Allocator, and TinyUSDZ. Copies of the
dependency license notices from the build are in `THIRD_PARTY_LICENSES`.
Slang compiler notices are included alongside those of the shader dependencies.
The display pass adapts Filmic, Uncharted 2, the minimal AgX fit, and
CMCCAT2000 white balance from
[`nvpro_core2`](https://github.com/nvpro-samples/nvpro_core2/blob/7cfe3ca9dc53ab23aeb8b55787fa676f1ff6c3d0/nvshaders/tonemap_functions.h.slang),
copyright 2022–2026 NVIDIA Corporation, and PBR Neutral from
[Khronos ToneMapping](https://github.com/KhronosGroup/ToneMapping/blob/180b1a7bddec33f73fe41712a2963cc3ad8e5547/PBR_Neutral/pbrNeutral.glsl),
copyright 2024 The Khronos Group, Inc. The shader adapts the curves to a
display-linear output before Vulkan's sRGB conversion. The Apache 2.0 license
is in `docs/tone-curves-Apache-2.0.txt` and packaged as
`THIRD_PARTY_LICENSES/ToneCurves-Apache-2.0.txt`.
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

Builds with `ENABLE_PHYSICS=ON` also include statically linked Jolt Physics
5.6.0, Copyright 2021 Jorrit Rouwe, under the MIT license. Its complete upstream
notice is included as `THIRD_PARTY_LICENSES/Jolt-MIT.txt`. No Jolt code is linked
when physics is disabled. See [the physics integration](physics-integration.md)
for the pinned source and available simulation features.

Builds with `ENABLE_OPENVDB=ON` also include OpenVDB under Apache-2.0, plus
its oneTBB, Boost, Imath, OpenEXR, Blosc and compression dependencies. The
package includes their installed notices as
`THIRD_PARTY_LICENSES/openvdb-dependency-*.txt`. Disabled builds do not link
OpenVDB. See [OpenVDB integration](openvdb.md) for the supported import,
volume rendering and voxel/SDF workflows.

Native sparse and `ENABLE_NANOVDB_IO` builds also use portable NanoVDB headers from OpenVDB 12.0.1
(Copyright Contributors to the OpenVDB Project, Apache-2.0). Their upstream
license is included as `THIRD_PARTY_LICENSES/NanoVDB-Apache-2.0.txt`. This path
uses Vulkan shader traversal or portable CPU file tools and does not ship or
require CUDA. Optional native ZIP/BLOSC codecs include their installed provider
and compression dependency notices in `openvdb-dependency-*.txt`, including
native-only packages when those providers are enabled.

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

Optional `ENABLE_GLTF_DRACO` builds include Draco 1.5.9 (Apache-2.0) only in the
adjacent bounded worker. `ENABLE_GLTF_MESHOPT` includes meshoptimizer 1.3 (MIT).
Their notices are staged as `Draco-Apache-2.0.txt` and `meshoptimizer-MIT.txt`.

Optional `ENABLE_TEXTURE_KTX` builds include KTX-Software 4.4.2 and its Basis
Universal, ASTC, Zstd, DFD and uthash components. Upstream license/notice files
are staged under `THIRD_PARTY_LICENSES/KTX`, or the installed package's aggregate
notice is retained. The restricted Ericsson ETC unpacker is disabled. Optional
`ENABLE_TEXTURE_WEBP` builds include libwebp 1.6.0; both COPYING and PATENTS are
retained. These options default off. Package build-info records each enabled
backend, and packaging fails if its required upstream notices are missing. See
[typed texture dependencies](texture-image-data.md) for exact pinned sources.

Optional reference denoising builds statically link NVIDIA's
[vk_oidn 0.2.0](https://github.com/nvpro-samples/vk_oidn/releases/tag/v0.2.0),
Copyright 2026 NVIDIA Corporation, under Apache 2.0. The SDK includes the
Open Image Denoise network weights. Its complete LICENSE and NOTICE are staged
as `vk_oidn-LICENSE.txt` and `vk_oidn-NOTICE.txt` in `THIRD_PARTY_LICENSES`.
