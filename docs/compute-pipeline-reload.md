# Managed compute pipelines

`PipelineManager` can create a managed compute pipeline from a SPIR-V path and
pipeline layout. The returned `ComputePipeline` owns a shared handle slot. Owners
may use `share()` when several render passes need the same slot; recording reads
its current Vulkan handle. The manager observes these slots weakly.

Prepare replacement shaders with `prepareComputeReload(directory)`, where the
directory contains `spv_shaders`. Preparation creates replacements without
changing live handles. A failed preparation leaves every live pipeline and the
active shader directory unchanged. `publishComputeReload()` swaps all prepared
slots together and makes the directory apply to subsequently created pipelines.
Unchanged shader bytes do not create replacements.

Temporal consumers can retain `ComputePipeline::contentIdentity()`. Publication
swaps this identity with the executable; preparation and abandonment leave the
live identity unchanged. Retaining it prevents identity reuse while old history
still refers to those bytes, without retaining a Vulkan object. A reset owner or
manager-retired slot returns an empty identity.

The caller must retire queue readers before publishing and disposing of the
candidate containing the old pipelines. The pipeline layout and device remain
borrowed resources. Manager shutdown also requires retired readers; it clears
the handles in any surviving shared slots. All 20 compute slots in the eight
existing `PipelineManager` owners now use this API; the raw compute-creation
overload has been removed so future callers register their executable state.
Recording and reload publication must be serialized on the owning render thread;
the shared slots do not provide concurrent publication or recording guarantees.

The migrated owners are bloom downsample/upsample (2), exposure histogram/
adaptation (2), environment BRDF/IBL/GTAO (6), clustered light culling (1),
frustum/Hi-Z/occlusion culling (3), shadow culling (1), BIM visibility/compaction/
residency (3), and temporal resolve/composition (2). Their descriptor layouts,
push constants and executable shader bytes are unchanged. Owners retire their
slots before destroying borrowed pipeline layouts. Bloom resources can be destroyed and
recreated while the manager stays alive; expired slots do not join a later
reload transaction. This migration supplies the owners needed by the editor's
reload controller; publication of that controller remains a separate step.

Four real Vulkan tests dispatch distinguishable shader programs and verify
prepare/publish behavior, shared-slot updates, partial preparation failure,
unchanged shaders, subsequent creation and both owner/manager destruction orders.
Repeated publications retain distinct content receipts, including when a shader
returns to its earlier program. They use synchronization validation and GPU
buffer readback.

Three additional production-owner GPU cases cover bloom retirement/recreation,
automatic exposure histogram/adaptation dispatch before and after publication,
and all six environment/IBL/GTAO registrations. Their replacement modules differ
only in SPIR-V generator metadata, retaining each real shader interface and
instructions. Exposure readback checks the histogram against a constant HDR
input and the resulting target exposure. Environment destruction now retires
both GTAO slots and their layouts/descriptors, preventing an explicitly destroyed
owner from participating in later reloads.

Build `managed_compute_pipeline_gpu_tests` and `managed_compute_owner_gpu_tests`, then run:

```powershell
$env:CONTAINER_RUN_GPU_MANAGED_COMPUTE = '1'
ctest --test-dir <build-directory> -C Release -R '^managed_compute_(pipeline|owner)_gpu_tests$' --output-on-failure
```

The owner-publication checkpoint passes 203 cases in ten selected executables
(seven Vulkan). Its 95 SPIR-V modules match the prior published producer bytes.
Eighty raster/AA/shadow captures match the prior checkpoint exactly. Ten further
captures include four prior-executable baselines: active bloom matches in both
raster techniques, bloom on/off changes the image, and the original ACCA IFCX
matches exactly in deferred and forward mode with bloom enabled. All native runs
use synchronization validation. Eight existing broader source-convention
failures remain separately recorded, unchanged from the parent.

The full local editor build separately passes 659 cases in 19 executables
(ten Vulkan), with the same 105 engine shader modules. Its original ACCA replay
at 128 samples matches the frozen prior editor image with zero unsupported or
rejected samples. The original IFCX is referenced in place and excluded from the
evidence bundle. The viewport and reload-controller publication is still pending.
Evidence: `out/feature-parity/managed-owners-publication-acceptance/results.json`.
