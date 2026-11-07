# Selected library releases

The latest stable releases were checked against their official upstream
repositories on 2026-11-07. Scoped vcpkg overlays pin source commits and archive
hashes. CMake requires these versions and stages notices from the selected
prefixes; package metadata records the actual selections. Upgrading a version
requires updating the pins/notices and validating the affected integration.

| Library | Selected release |
| --- | --- |
| [GLM](https://github.com/g-truc/glm/releases/tag/1.0.3) | 1.0.3 |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/releases/tag/v3.5.0) | 3.5.0 |
| [fmt](https://github.com/fmtlib/fmt/releases/tag/12.2.0) | 12.2.0 |
| [spdlog](https://github.com/gabime/spdlog/releases/tag/v1.17.0) | 1.17.0 |
| [GLFW](https://github.com/glfw/glfw/releases/tag/3.5.1) | 3.5.1 |
| [Dear ImGui](https://github.com/ocornut/imgui/releases/tag/v1.93.0) | 1.93.0 |
| [MaterialX](https://github.com/AcademySoftwareFoundation/MaterialX/releases/tag/v1.39.5) | 1.39.5 |
| [miniz](https://github.com/richgel999/miniz/releases) | 3.1.2 |
| [EnTT](https://github.com/skypjack/entt/releases/tag/v4.0.0) | 4.0.0 |
| [nlohmann/json](https://github.com/nlohmann/json/releases/tag/v3.12.0) | 3.12.0 |
| [TinyGLTF](https://github.com/syoyo/tinygltf/releases) | 3.1.0, bundled deprecated C++ interface |
| [TinyEXR](https://github.com/syoyo/tinyexr/releases) | 3.4.0, retained C++ interface |
| [stb](https://github.com/nothings/stb/commit/4e9b368bfc071e97206ea5c6adba68dd0cbf8897) | Untagged snapshot `4e9b368…`; image header remains 2.30 |
| [GoogleTest](https://github.com/google/googletest/releases/tag/v1.18.0) | 1.18.0, test binaries only |

The existing Earcut 3.2.4, Manifold 3.5.4, Clipper2 2.0.1 and meshoptimizer 1.3
selections also match the checked latest releases. MikkTSpace has no versioned
release; its existing pinned source matches upstream. Optional disabled
integrations retain their own documented pins and need review before enabling
a new validation configuration.

The selected graphics toolchain uses Slang 2026.21, Vulkan SDK 1.4.363.1 and
its 1.4.363 headers, validation 1.4.363.1 and the separately versioned Vulkan
runtime loader 1.4.363.0. Optional enabled SDKs are DLSS 310.9.16, FSR 2.4.0
and OptiX 9.1.0; hardware/provider availability still determines active features.

TinyGLTF's current active parser is C11; its older C++ model API is deprecated
and no longer built or tested upstream. This project explicitly selects the
compatibility payload from the latest release and tests the consumed engine
paths. TinyEXR's root CMake still builds its C++ interface. Neither selection
implies adoption of the new C APIs. Exact source/interface metadata is retained
in the selected dependency index, rather than identifying those APIs by the
release number alone.

EnTT 4 uses component-pool `info()` in place of `type()`. JSON 3.12 changes its
versioned inline namespace, so all dependent objects must be rebuilt. ImGui
1.93 uses its current Vulkan pipeline information, dynamic font upload and
image-only texture registration. The Comfy preview uses the backend's equivalent
linear/clamp sampler and retains its existing descriptor retirement policy.
