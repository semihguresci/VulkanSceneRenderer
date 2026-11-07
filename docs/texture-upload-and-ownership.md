# Typed Vulkan texture upload and ownership

`AllocationManager::createTextureFromImageData` uploads the format, component
mapping and supplied mip chain from `TextureImageData`. File and encoded-byte
uploads use the shared CPU decoder and then the same typed path. The requested
RGBA8 format on those two entry points selects linear or sRGB interpretation;
it does not force a decoded 16-bit or floating-point image back to eight bits.

The upload planner validates dimensions, format byte sizes, mip progression,
component swizzles, staging budget and combined buffer-offset alignment before
allocating staging storage or an image. A supplied authored pyramid, including
a single level or a partial pyramid, keeps exactly its authored level count.
An unauthored single level generates a complete pyramid only when the Vulkan
format supports linear blits. Otherwise it retains its base level.

Device format support includes sampling, linear filtering and transfer usage.
BC, ETC2 and ASTC formats are offered to the decoder only when their compression
feature was enabled on the logical device. `Application` passes the enabled
features to the allocator. Unsupported formats use the decoder's documented
fallback or produce an actionable error; physical-device support alone does not
authorize a compressed image.

Staging writes are flushed before transfer. Uploads transition every allocated
mip, copy each supplied level, perform any required blits, and make transfer
writes visible to both raster and compute consumers. Completion is awaited
before staging storage retires. Progress-callback failures also retire submitted
commands before image or staging destruction.

## Allocation ownership

Use these APIs on the allocator's owner thread with externally synchronized
Vulkan queue access.

An ordinary scene upload stays owned by the allocator until a synchronized scene
reset. Persistent textures have a separate reset scope. A texture-allocation
batch prepares detached scene images: abandoning it rolls back its own images,
while committing returns a shared lease that can be held by published resources
and undo history. Nested batches and resets during an upload or active batch are
rejected before changing state.

Individual image leases let selected images survive release of an aggregate
batch while unreferenced siblings retire. Ordinary scene images require an
explicit adoption after all published aliases acquire leases. Adoption checks
the complete sorted, unique list before changing ownership. The final retained
owner waits for submitted GPU readers before releasing an image.

`TextureResource` is a non-owning alias. Its allocation identity identifies both
the allocator and image incarnation; copying it does not pin the image. Handle
reuse, force resets, foreign allocators and mismatched image/view pairs cannot
turn a stale alias into a valid ownership token. Leases can safely outlive a
destroyed allocator. Explicit force resets require retiring GPU readers and
history first, and invalidate aliases in their reset scope.

## Stable texture table

`TextureManager` stores format/dimension/mip metadata and content identities.
Retirement leaves a stable inactive slot, erases name/content cache aliases and
uses a fallback when requested. It does not retarget surviving slots or destroy
an allocator-owned image. Prepared append/retirement operations retain finite
budgets and validate table identities and revisions before publication; their
commit moves prepared state without allocating.

The scene consumer now publishes dense sampler policies, matching texture
metadata and stable image descriptors, including a private fallback for every
inactive slot and empty catalogs. It validates allocation identities and table
revisions before writing descriptors and retains actual image ownership. See
[glTF material sampler integration](material-sampler-publication.md).

Table mutation alone does not update a scene's descriptors. A live consumer must
publish matching image/sampler/metadata descriptors before releasing old image
leases. [glTF image ingestion](gltf-texture-ingestion.md) describes the published
encoded-source selection and color/data-role mapping. Interactive texture
authoring and persistence remain under #108.

## Verification

`texture_upload_plan_tests` checks malformed chains, budgets, offsets and invalid
component mappings. `texture_manager_tests` checks stable slots, transactions,
allocation failures and stale or foreign table identities.

Run `texture_upload_gpu_tests` with `CONTAINER_RUN_GPU_TEXTURE_UPLOADS=1` on a
Vulkan device with validation layers. Its real-image readbacks cover 16-bit and
signed HDR values, authored/generated mips, compressed blocks, linear versus
sRGB mip generation, rollback, image pins and submitted-reader retirement.
An independent Slang sampling probe checks image-view swizzles, every authored
mip, values above one, negative channels, sRGB decoding, linear alpha and
independently authored BC3 coverage alpha.
Its sRGB oracle allows half one authored 8-bit code before applying the analytic
EOTF; linear RGB and alpha keep their separate float tolerance. This is a
format-fidelity check, not a claim that hardware sRGB decoding equals a CPU
`pow` result. [Khronos defines the RGB conversion and leaves alpha linear](https://docs.vulkan.org/spec/latest/chapters/images.html#images-reads-texel-decode).

Material samplers use floating-point transparent black, which is compatible
with the uploader's normalized/float formats and non-identity view mappings
without enabling extra border-swizzle features. See
[Vulkan's instruction/format/sampler/image-view validation](https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-input-validation).

`texture_upload_runtime_regression.py` exercises external PNG16 emission through
both real raster renderers. It compares the complete float image with an
independently calculated, material-only emission reference, and verifies that
an explicit eight-bit truncation produces a different image.
