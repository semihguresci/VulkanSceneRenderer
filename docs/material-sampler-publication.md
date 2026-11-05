# glTF material sampler integration

Material textures retain all six glTF minification filters, both magnification
filters and the three wrap modes on each axis. Omitted filters use linear
magnification and trilinear minification. Anisotropy is disabled so that authored
nearest and mip filtering remain observable.

The 108 canonical policies are resource identities, not descriptor indices.
Each scene packs its active policies into dense sampler slots and publishes the
matching texture metadata. Samplers are created lazily and shared by policy.
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
selection and color/data roles. Interactive texture authoring and persistence
remain under #108/#121. Raster derivatives select texture mips; existing ray
transport still uses explicit LOD and retains its separate footprint/filtering
requirement under #47.
