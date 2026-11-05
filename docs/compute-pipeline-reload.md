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

The caller must retire queue readers before publishing and disposing of the
candidate containing the old pipelines. The pipeline layout and device remain
borrowed resources. Manager shutdown also requires retired readers; it clears
the handles in any surviving shared slots. The existing raw-pipeline creation
overload remains available while renderer owners migrate to managed slots.

Three real Vulkan tests dispatch distinguishable shader programs and verify
prepare/publish behavior, shared-slot updates, partial preparation failure,
unchanged shaders, subsequent creation and both owner/manager destruction orders.
They use synchronization validation and GPU buffer readback.

Build `managed_compute_pipeline_gpu_tests`, then run:

```powershell
$env:CONTAINER_RUN_GPU_MANAGED_COMPUTE = '1'
ctest --test-dir <build-directory> -C Release -R '^managed_compute_pipeline_gpu_tests$' --output-on-failure
```
