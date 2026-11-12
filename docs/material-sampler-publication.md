# glTF material sampler integration

Material textures retain all six glTF minification filters, both magnification
filters and the three wrap modes on each axis. Omitted filters use linear
magnification and trilinear minification. Anisotropy is disabled so that authored
nearest and mip filtering remain observable. The loader validates sampler
references and integer fields before parsing and preserves equivalent integral
decimal and exponent forms. [glTF sampler properties](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#sampler)

Explicit `NEAREST` and `LINEAR` minification select mip zero. The Vulkan sampler
uses a maximum LOD of `0.25` with nearest mip selection so minification can
still use a different texel filter from magnification. A one-level image view
also keeps its authored texel filters. [Vulkan sampler mapping](https://docs.vulkan.org/refpages/latest/refpages/source/VkSamplerCreateInfo.html)

Each scene packs its active policies into dense sampler slots from the 108
canonical policy keys and publishes the matching texture metadata. Samplers
are created lazily and shared by policy. Texture edits preserve the authored
filter fields for portable export, including the distinction between an
omitted filter and an explicit default. Both forms can share the same
Vulkan sampler and image allocation.
The device budget reserves eight samplers for the other pipeline sets and
checks both per-stage and aggregate layout limits. Scene replacement checks the
incoming policies before retiring the old scene; appends include current rows.

Stable texture slots keep their descriptor positions. Inactive rows bind a
private white fallback with default filtering, including an empty catalog.
Invalid active allocation identities reject instead of silently using fallback.
Scene descriptors validate the table revision, allocation incarnations and
sampling plan before publishing. Published image ownership is retained separately
from non-owning texture aliases.

`material_sampler_gpu_tests` compares all policy combinations against an
independent CPU texel/mip oracle. `scene_sampler_publication_gpu_tests` executes
the real scene descriptors, including a retired middle row, an empty/all-retired
catalog, stale metadata and invalid allocation capabilities. Both use Vulkan
validation and require their documented GPU environment switches.

`material_sampler_runtime_regression.py` checks the actual forward/deferred
renderers, transparent and masked coverage, mip policies and shared images.
Its twelve-policy scene requires thirteen material sampler slots including the
default and catches shader clamps to the former nine-entry prefix. The two
minification scales sample distinct colored/averaged mip levels; scales that
only reach uniform checkerboard mips cannot establish a filtering difference.

See [glTF image ingestion](gltf-texture-ingestion.md) for encoded-source
selection and color/data roles. Raster derivatives select texture mips.
The hard-ray and DDGI smoke cases check execution of the sampling paths and
descriptor compatibility. [Ray texture footprints](reference-path-tracing.md)

## Release regression

Set `$build` to a Release build directory and `VK_LAYER_PATH` to the installed
Khronos validation layer if it is outside the Vulkan loader's search path.
These captures use synchronization validation.

```powershell
python -B tests/validation/gltf_sampler_filter_runtime_regression.py --renderer "$build/VulkanSceneRenderer.exe" --output out/sampler-live --case sampler-edit
python -B tests/validation/gltf_sampler_filter_runtime_regression.py --renderer "$build/VulkanSceneRenderer.exe" --output out/sampler-filter --roundtrip --ray-smoke --case mag --case min --case shared --case transparent --case mask --case edit
python -B tests/validation/material_sampler_runtime_regression.py "$build/VulkanSceneRenderer.exe" out/sampler-dense --ray-smoke
```

The live-edit fixture holds the image and wrap modes constant. It changes
the filters on one probe, checks the other probe's binding and pixels, and
replays undo and redo. An explicit-to-omitted edit keeps the same effective
sampler slot and pixels. The saved GLB omits both filter fields for that
binding and reloads with the same pixels in each raster technique.

The November 12, 2026 Release run on an RTX 2080 SUPER passed 124 checks
across 148 native captures. All 48 save/reload pairs matched pixel for pixel.
The twelve-policy fixture reached twelve distinct sampler slots on one source
image, with the default at slot zero. Both raster paths also completed the
hard-ray and DDGI smoke cases. Nine focused CTest targets and the two staged
sample-loader cases passed, totaling 208 C++ cases. Ten sampler consumer and
contract shaders passed Vulkan 1.4 SPIR-V validation.
[Release sampler results](../tests/visual-regression/samplers/windows-nvidia/sampler-filter-results.json)
