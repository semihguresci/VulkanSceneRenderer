# Resource lifecycle implementation contracts

These contracts cover material and source/runtime resource retirement, plus
the remaining lifecycle work in #108. The existing ImGui Material inspector
exposes independent Material and Resource catalogs with unused-entry deletion.
Texture slot clearing preserves source resources. Original creation/import
history and independent Delete commands retain distinct replay authority.
Complete reference authority, CPU payload reclamation and external renderer
acceptance remain tracked separately.

## Identity and retention

`ResourceLifecycleCatalog` issues opaque identities in explicit archive,
evaluated and runtime domains. A handle contains a stable slot and a unique
incarnation. A numeric index, old UI inventory or surviving handle is a weak
intent; it cannot authorize resurrection or prevent reuse.

Live-dependency pins block logical retirement unless an exact outgoing owner is
explicitly included in the same checked transition. History and GPU-reader pins allow
logical retirement while retaining the identity and its externally owned
payload. Preparation pins either block retirement or are explicitly invalidated
by a catalog revision change. A separate retirement command owns reversible
phase authority. Reuse requires retirement and release of every pin and command;
the reused slot receives a new incarnation.

The catalog bounds aggregate owned metadata, references, snapshots, commands,
registrations, reason strings and preparation work. It owns neither TinyGLTF
payloads nor Vulkan allocations. Its pin inventory is not a reference proof.
All operations follow the owning scene thread's synchronization contract.

SceneManager uses one runtime material identity domain and an explicit current
numeric-row-to-handle map. Authored/evaluated ownership is proved through exact
source mapping and selected records. Internal catalog slots increase monotonically;
they are not runtime material indices. A detached owned append-tail address can
be used by a fresh incarnation, while its old command retains a different
catalog handle. Retired middle material slots are never reused by fresh appends.
Full merge generations fork the current retirement mask into a separate catalog.
Material-equivalent subtree clones retain the same material domain; their later
material actions must reverse before clone history exchanges the payload back.
Shared-domain exchanges retain the greatest internal slot high-water mark.

Transactional producers stage registrations and their pins before publication.
`stagePin` attaches an already-owned RAII token to a prepared registration,
accepting its exact prospective handles and existing active handles. A pending
token reserves bounded metadata without appearing as a live cell dependency.
Registration exchange publishes rows and activates attached pins together;
abandonment cancels them. The outer transaction can therefore own history and
GPU retention tokens before the allocation-free final commit.

Late `stageDependencyRelease` attachment captures exact active outgoing live
pins on an existing registration or phase-less transition. It does not grant
permission from a resource index, owner label or History pin. Exchange rechecks
the captured owner/serial/row witnesses, releases those dependencies and enables
the pending successor pins in one catalog revision. Residual live dependencies
and preparation blockers still apply. An outgoing reservation provides no
prospective capacity credit.

## Logical resource generations and selected history

`TextureResourceLifecycleGeneration` maintains independent Archive and Evaluated
domains for Image, Texture and Sampler rows, plus a Runtime domain for
RuntimeTexture rows. Its current numeric table addresses map to owner-issued
handles. Internal slots increase monotonically: Undo can detach an owned source
or runtime suffix, and a new branch can reuse its numeric table address while
the earlier command retains a different logical incarnation. Equal source
indices, pixels, sampler policy or GPU image handles never merge these identities.
Source-to-runtime roles remain explicit and may share an actual image allocation.
Logical source sampler identity is also separate from a shared canonical
`VkSampler` cache entry; selected source retention does not authorize destroying
that GPU object before its owning generation and readers retire.

The generation owns immutable source/runtime payload versions and exact row maps.
`TextureResourceReferenceOwner` retains selected payloads and pin controls,
independently of the complete parent generation. Selection closes each source
texture over every known authored image alternative and its source sampler.
Material selection also extracts raw core, extension and legacy/fallback texture
references, including shadowed bindings, independently in both source domains.
Actual runtime roles and selected export bindings are retained explicitly.

`TextureResourceLiveOwner` is separate from selected History ownership. Each
active source texture owns its explicit image alternatives and source sampler;
each color/data role owns its runtime texture. Source Image/Sampler/buffer-view
normative metadata can retain an opaque owner even with no known edge. Active
materials separately own their authored, evaluated, cached-export, rest-state,
runtime and proved provenance targets. The exporter fallback contributes only
the runtime-to-archive mapping cells actually selected by that material. An
unused mapping cell does not pin an unrelated texture. Recognized BIM/provider
material consumers are transitive through active material definitions; their
external reference schemas still require a complete authority audit.

Live owner labels include the exact opaque owner handle and scope. Replacing
an existing owner requires its exact active control, including owners with an
empty target set. Weak current-owner ledgers and bounded pending reservations
avoid predecessor chains. Owner count, reference count, storage and traversal
limits apply before publication. Unchanged endpoint states and unaffected role
controls can be shared; generation-level role/correspondence witnesses remain
exact. History ownership never substitutes for a live owner.

Resource candidates are structural until explicitly sealed. A material/resource
edit stages the other material owners and its selected validated successor on
the same source-catalog mutation, then seals once. A standalone resource edit
must preserve every active material dependency. It cannot remove a bound tail
by releasing the material's owner. Abandonment or failed sealing cancels pending
controls and leaves the preceding live controls intact. Publication exchanges
source controls, material owner maps and their prepared CPU/GPU generations at
the existing allocation-free commit boundary.

Selected payload ownership includes exact source Image/Texture/Sampler records,
retained image data and the bounded encoded buffer-view slice when applicable;
it does not retain an unrelated complete buffer or source catalog as a fallback.
Copies and unions share selected immutable payloads and pin controls without
retaining the full parent table. This is CPU source ownership, separate from
`TextureImageOwnership` and its actual allocator-issued image leases. Neither
logical handles nor copied runtime resource records own Vulkan objects.

Factor, coalesced gesture, Cancel, texture Clear/mapping/import and plain
assignment histories retain the required selected endpoint union. Selected
material handles additionally receive History pins in the existing material
catalog. Make Unique/append and material retirement receipts retain their own
selected resource closures alongside their existing material phase authority.
Resource receipts retain exact owned-tail and selected replay-input identities,
including a sampler-only alias of a prefix image. Failed/coalesced updates build
the successor owner, reservation and callbacks before publication, leaving the
previous gesture and history cursor unchanged on failure.

Raw source extraction distinguishes complete reference results from bounded
`RecordIncomplete` results that preserve accepted opaque normative metadata.
Known references and indices still validate; malformed known references reject.
An incomplete selected endpoint owns an independent opaque blocker even if its
known selected resource set is empty. `opaqueReferenceOwnerCount()` exposes those
owners. This preserves selected retention without pinning every catalog row;
an incomplete result cannot authorize arbitrary unused-resource retirement.
An empty blocker count also cannot replace the missing external reference proof.

Owner validity and permission to replay are separate. `valid()` accepts retained
retired identities and still-pending candidate pins. Abandoning their registration
or transition cancels escaped pending tokens. `active()` additionally requires
active selected resources and additional material pin targets;
`resourceDependenciesActive()` checks the selected resource dependencies while
allowing an authorized material command to restore its own retired material.
`resourceDependenciesMatch(currentGeneration)` checks exact selected authority
and handle membership, including the authority of empty opaque
selections. A different generation with identical numeric addresses is insufficient.
Material asset/owner/phase and command-cookie guards remain required as well.

The existing trusted mutable source API can append an independent evaluated
image, detach an evaluated suffix, or update unrelated names/extras/opaque
metadata before a fresh edit. Explicit bounded reconciliation observes those
already-authoritative changes without forking surviving identities. Compatible
metadata gets a new immutable payload version at the same handle; history does
not rewind an unrelated current prefix. Removed suffix identities retire and
later numeric reuse receives fresh internal slots. Reconciliation rejects
untracked binary/image-format/runtime-capability/sampler-policy replacement,
known texture dependency changes, and mixed append/retirement in one step.
Fresh preparation may reconcile before taking its snapshot; final guards never
reconcile, allocate or mutate. Old prepared pointers/revisions and full payload
matches become stale. This maintenance is not an unused-resource Delete proof.

Ordinary factor, binding and resource-free texture replay require active matching
dependencies. A mixed before/after resource-history union may retain a detached
tail: replay uses its original resource command and a freshly checked prospective
generation instead of requiring every union member already active. Retaining a
History pin, payload or image never authorizes activating a retired dependency.

Merge and clone replay prepare a fresh dependency receipt before the final
asset exchange. Independent merge catalogs can retain their active controls
while inactive. A shared clone must rebase material owners against today's
active controls after intervening material edits; it cannot swap canceled
controls back into service. Its resource payloads, role/cookie correspondence
and exact row incarnations must still agree with the live source graph.
Unrelated source changes or a different resource branch reject rather than
being rewound by graph history. Preparation owns the snapshots, replacements
and storage; the final guard checks both endpoint witnesses and each future
owner's exact membership in that mutation. Consumed receipts are released
outside the allocation-free asset exchange.

Prepared scene, material and resource descriptor generations retain complete
corresponding logical sets: old readers use GpuReader pins and the future active
generation uses Preparation pins. Full selection skips runtime tombstones.
Prospective owners join the exact pending registration/transition before commit.
The publisher rechecks current generation identity and exact source/runtime
tables, selected owner validity and the prospective command proof. Pin count
changes do not advance the catalog revision, so current pin/phase checks cannot
be replaced by comparing a saved revision alone. Old owners survive exchange in
the consumed token and release after the outer reader-retirement boundary.

Full merge candidates fork logical resource authority into an independent
catalog; equivalent subtree clones share the exact existing logical generation.
The complete generation travels with its corresponding asset payload during
Undo/Redo. Candidate guards capture the current generation identity and validate
both resource-table views. Selected histories do not acquire another catalog's
authority merely because its source bytes or physical images match.

## Staged outgoing dependency replacement

The generic catalog supports outgoing-owner overloads of `prepareRetirement`
and `prepareChange`. Preparation captures exact active LiveDependency token
serials and resource rows. Exchange rechecks those owners and residual external
dependencies; resetting or replacing an outgoing token makes the preparation
stale. Successful exchange deactivates only the supplied exact tokens, preserving
their retained storage for release outside publication. Failure or abandonment
does not release the old live edges.

`prepareDependencyChange` stages an outgoing-owner replacement without changing
resource phases or issuing retirement authority. Transition `stagePin` prepares
incoming ownership over the supported current/prospective identities. Pending
pins activate only at successful exchange and cancel on abandonment. No temporary
ordinary LiveDependency pin is published to represent a future edit. All bounded
allocation, normalization, payload retention and callbacks precede the no-throw,
allocation-free exchange; old payload/pin destruction stays outside it.

These generic mechanisms do **not** mean every production dependency is installed.
Complete live edges for source materials, source image alternatives, private
export/request caches, animation/rest state, BIM and external providers still
need coordinated scene publication and bounded reference authority.
`TextureResourceLifecycleGeneration::dependencyAuthorityComplete()` deliberately
returns `false`. Zero live counters cannot authorize arbitrary Image, Texture,
Sampler or RuntimeTexture Delete. The current integration supplies registrations,
selected history and preparation retention, not complete resource retirement.

## Reference proof and animation

`AuditUnusedMaterialReferences` checks an arbitrary source/runtime material pair,
including runtime-only materials with an explicit absent source ID. It inspects
authored and evaluated source, all graph nodes, parsed and raw variants, material
LODs and raw material animation pointers. Hidden, inactive and retired graph
nodes still count. Unknown normative reference schemas reject; extras remain
opaque. Aliased source-to-runtime mappings reject conservatively. Existing tail
wrappers retain their stricter tail ownership rules.

The scene owner must additionally audit BIM metadata, retained export source
records, provider payloads and all other owners outside that view. Queued UI
requests require generation and incarnation guards. A source-backed proof cannot
authorize removal from a different runtime or retained scene generation.

The material transaction now audits private export records and the frontend
audits BIM object/metadata rows plus registered provider payloads. Unknown
providers or unproved normative cache schemas reject conservatively. Renderer
defaults and implicit primitive defaults remain protected. Runtime/BIM/manual
material creation registers an incarnation before the row can be retired.

`AnimationPlayer::withUntrackedMaterialRest` changes one unused rest row after
exact authored-rest, source-shape and curve checks. Raw and decoded animation
targets block the change. It preserves the current clock and playback state and
copies no image or buffer payload. It does not establish reference safety.

## GPU image ownership

`TextureResource::allocationIdentity` is a weak allocator-issued capability for
one image incarnation. Names and sampler aliases may share that identity;
matching raw Vulkan handles never establish ownership. Per-image leases permit
unreferenced siblings in a released upload batch to be reclaimed independently.
An unreleased aggregate batch lease continues to protect the entire batch.

The live main asset uses an immutable `TextureImageOwnership` generation rather
than a vector of aggregate batch owners. A complete set records the allocator,
TextureManager revision, exact active/dead row coverage and runtime-slot-to-image
mapping. Its images and pins are sorted and unique by allocator incarnation.
Sampler aliases of one allocation share its pin; equal pixels or matching views
cannot merge distinct allocations. Every active catalog row is covered, including
images that are currently undrawn. Tombstones retain no image pin.

`PrepareTextureImageOwnership` checks finite storage limits and actual allocation
capabilities, reuses exact inherited pins and acquires missing pins before
returning complete coverage. Revalidation checks its owner, revision, row map,
incarnations, image/view pairs and leases without allocation. Full sets travel
with live and inactive merge/clone assets and prepared scene, material and resource
descriptor generations. A sampling plan is a non-owning CPU description; it does
not replace this image retention.

Long-lived material history retains independent selected sets. Append and
retirement receipts keep their selected material's actual image union. Resource
receipts retain owned-tail and replay-input images, including a sampler-only tail
that aliases a prefix image. Factor, binding, texture Clear/mapping and pending
gesture history retain the required before/after endpoints. Selection and union
copy only the selected pins and capabilities, without retaining a complete parent
catalog. These actual-image pins supplement explicit source/role provenance;
they are not logical source-resource retirement authority.

Resource Undo/Redo rebuilds complete live coverage from all surviving active
rows, using current and selected command pins. It does not subtract a command's
pin from the live set: a surviving prefix and an owned tail may share the same
allocation. Prepared generations keep the corresponding old/new complete sets
through descriptor staging and exchange. Old descriptor owners remain retained
until the outer publication retires their raster, ray and DDGI readers.

Acquiring a pin for an initial group-zero image preserves its original reset
ownership. `PrepareTextureImageOwnershipAdoption` stages the eligible sorted,
unique group-zero pin list while complete alias coverage is still held. Initial
loading finishes fallible metadata, clip-state and descriptor allocation before
adoption; reload/recovery finishes its descriptor writes first. Final revalidation
checks complete current ownership and the exact eligible list, then
`adoptTextureImages` changes ownership without allocation. An empty list is a
valid no-op at this boundary. A failed or abandoned preparation leaves the reset
owner intact. The allocator's list check alone does not enumerate every alias;
the complete set and scene publisher provide that proof.

New uploads keep a local aggregate rollback owner after their batch commits,
because active batches cannot authorize descriptors or per-image pins. All new
alias pins and candidate/history ownership are prepared while that owner remains
alive. The transient aggregate is then released outside the allocation-free
exchange. Main assets and their new history no longer retain that entire batch.
Cancellation releases only independently owned candidate images; inherited pins
continue to protect the preceding scene.

Final pin release may wait for the device and free memory. Old owners, branch
truncation, gesture cancellation and history clearing therefore release outside
the allocation-free commit and after the applicable reader-retirement boundary.
Explicit force reset retains its requirement to retire all old readers and
history first; reset invalidates capabilities even when stale lease objects
survive. Those objects cannot authorize later publication or replay.

`TextureManager` supports stable tombstone slots and removes every name/content
cache entry targeting a retired row. Dead rows resolve only through an explicit
fallback accessor. Slot count is a high-water mark; active count is separate.
Successful registration/retirement changes a table revision. Exact detached
prefix copies preserve inactive slots and only the original surviving cache
aliases; re-registering a history prefix can incorrectly resurrect an erased
content alias or collapse numeric addresses.

SceneManager's shared `TextureSamplingPlan` keeps image descriptors, sampler
slots and metadata in the same stable order. A dead middle row receives the
private fallback; later survivors retain their indices. An empty catalog has
one initialized fallback descriptor and metadata row, while its authored/runtime
resource count remains zero. Policy-only preflight permits dry-run upload
placeholders; actual descriptor staging checks every active image's exact live
allocator capability. A null, stale, foreign or mismatched active image rejects
instead of silently receiving fallback.

The white one-texel fallback is outside all source/runtime catalogs and cache
lookups. An explicit private single-image batch lease owns it. Prepared bindings,
material/resource generations and retained commands share that owner; merge and
clone candidates retain it with their asset payload. It is deliberately separate
from main-catalog image ownership and cannot be selected as a user resource.
Incoming merge images acquire complete candidate per-image coverage after their
batch commits and before its transient aggregate owner releases. Descriptor
validation still requires committed, valid capabilities.

Plans capture table revision, active mask, allocator incarnations, image/view
pairs, sampler policies and metadata. Nonallocating revalidation checks those
values plus the live metadata/sampler plan and handles, so equal resolved
fallback views do not hide a phase change. Finite preparation budgets charge
the stable row range and bounded policy domain before allocating. Ordinary
material/resource editing, owned-tail history and merge/clone consumers support
these rows; source/role authoring rejects a dead resource identity.

Immediate import producers can append CPU resource rows before their explicit
GPU upload. A resource, merge or clone generation may restage that state only when the published
plan is an exact unchanged prefix, revision growth equals the appended row count,
and every appended image is active with a valid allocation capability. The token
captures the old plan scope and the full CPU table; merge and clone also capture
the exact complete image-owner identity. Their candidates always use complete
new sampling plans. An intervening refresh or phase change rejects a token even
when the image owner and outer revisions remain unchanged. Ordinary bindings and factor edits without a prepared
resource generation still require the complete current GPU sampling state.

Merge publication reuses a model scratch record constructed with the manager.
Three move assignments exchange each TinyGLTF source model without constructing
a temporary map at commit time. TinyGLTF's declared `noexcept` model move does
not itself prove that generic `std::swap(Model)` avoids MSVC sentinel allocation.

## Export and publication

Source Image/Texture/Sampler tables have independent phase masks: an empty mask
means all active; an explicit mask has exactly one zero/one cell per stable row.
The generation's `active(selection)` checks intended phases during preparation
and exact catalog phases after publication. Ordinary reconciliation cannot flip
a source phase or authorize restoration. A separate selected retirement receipt
retains original handles, selected payloads and exact color/data/evaluated
endpoints. Its checked replay restores only those identities and preserves
unrelated current rows, mappings and cookies. An active source texture cannot
target an inactive image or sampler. Whole masks are validated once, with
selected dependency checks per owner rather than quadratic full-mask scans.

The source-only SceneManager transaction prepares complete raw source witnesses,
selected phase/role/cache/export deltas and all material owners on the same
sealed mutation. Retirement requires complete reference evidence; original
activation can preserve compatible opaque blockers without claiming absence.
Current material dependencies are checked before clearing export mappings, so
clearing a cell cannot manufacture an unused resource. Cache invalidation keeps
stable slots with inactive tombstones; Undo restores only its exact selected
cells. Restaging uses the prospective exporter map. Borrowed players are compared
to the caller's current pointer before use. External provider/reader authority
is still an outer coordinator prerequisite.

Source-only retirement leaves runtime sampled textures and Vulkan image leases
in service. It does not reclaim GPU images or remove encoded bytes from shared
binary buffers. Physical runtime retirement and a user Delete action remain
separate required integration and acceptance work.

`GltfResourceExportCompaction` projects Image/Texture/Sampler survivors densely
on a detached model. It validates masks, typed and shadowed legacy references,
all supported codec alternatives, samplers and supported pointer channels before
editing. Active incoming references to removed rows and unknown normative
reference schemas reject. Retired material/node owners are canonicalized only
in the detached output and their channels are dropped before existing owner
compaction. Metadata/reference/shape witnesses and bounded staging protect the
plan/apply boundary; survivor payload allocations are moved, not copied.

The common scene exporter uses this projection only for resource holes. Buffer
packing needed for geometry validation remains early; image/audio packing is
deferred until after projection, so a retired external image is never opened or
encoded. Both public save coordinators share this builder before their existing
light overlays and node/material compaction. Binary buffer liveness/repacking
and new editor retirement acceptance are not established by this contract.

Raw animation reference authority uses an explicit supported property grammar.
A known root with an unknown suffix remains incomplete, including unsupported
direct image/texture/sampler properties. Known target identities remain retained;
malformed and inactive targets reject. Recognized providers expose a borrowed
material-reference view for final checks, avoiding allocation from copied
snapshot strings. Unknown/default providers remain incomplete. These pieces do
not by themselves make `dependencyAuthorityComplete()` true.

`GltfMaterialExportCompaction` operates on a detached, unpublished export model
after explicit source-to-runtime material projection. It removes arbitrary
retired rows, preserves survivor order and remaps known primitive, variant, LOD
and material animation references. Incoming references to removed rows reject.
Binary and image payload allocations retain their identity. Live source and
runtime IDs stay stable.

Compaction checks bounded metadata, exact known references and table shapes
before editing. It allocates staging storage before mutation and then updates
the caller-owned model using move assignments and container swaps. Returning a
TinyGLTF model by move after mutation is unsafe on MSVC: map move construction
can allocate a sentinel despite TinyGLTF's declared `noexcept` move constructor.

Material deletion uses the existing coordinated publication transaction for
selected payload tombstones, catalog phases, descriptors, material routing,
ray/DDGI consumers and history. Replaying history prepares against today's
animation clock and exact identity/phase. Undo restores the material before older
factor/texture/binding history can replay. Failure preserves the preceding
scene, gesture and redo branch. Both save entry points compact only the detached
runtime-indexed output after light overlays and node compaction.

Per-image ownership, logical registrations and selected history retention do not
establish arbitrary image/texture/sampler Delete authority. Operational deletion
still needs complete authored/evaluated/private-cache/BIM/provider/history
reference proof, publication of every required live dependency, and conservative
rejection of unproved normative reference schemas. Reversible source/runtime
tombstones and exact retirement authority must coordinate cache/provenance
invalidation, reader publication and bounded dense image/texture/sampler export
in both save paths.
Exact payload preservation, portable save/reload, failure/history acceptance and
the corresponding editor actions remain required. Stable fallback descriptors
and actual-image pins alone cannot authorize deleting a referenced source row.

## Runtime resource transactions

`PrepareRuntimeTextureRetirement` stages a detached runtime table with stable
tombstones. Its receipt retains only selected original payloads and exact name
and content aliases. Replay requires that original receipt, row incarnation,
phase version and table lineage. New imports append; restoration rejects alias
collisions and equal-revision forks with different unselected state. The table
contains non-owning image capabilities, so this operation alone cannot retain or
restore a Vulkan allocation. `TextureManager::rowStorageBytes()` includes the
phase and identity columns for preparation and history accounting.

Original import history uses a separate append receipt. Cutting its owned tail
captures the current row identities, aliases and phase witnesses; restoring it
reinstates those exact rows. The logical catalog issues append authority only
from the original pending registration, over its complete issued set. Its own
suspension/resume round trip preserves prior retirement endpoint stamps while
catalog, physical phase and command revisions keep advancing. An ordinary
competing retirement still invalidates an older receipt. Equal payloads and
numeric row addresses never authorize identity restoration. This lets older
Import Undo/Redo compose with a later Delete of an imported texture.

The scene coordinator combines explicit source and runtime selections into one
logical catalog mutation. Runtime-only selection can clear an unused source
color role while preserving the same source texture's live data role. A source
texture's archive/evaluated correspondence and image/sampler alternatives are
candidate neighbors; only a bounded incoming-reference proof may select an
orphan. Shared neighbors remain active. Cache, role, correspondence and phase
changes publish together, and restoration uses their original identities.

Explicit Bind/Clear on a typed core material texture also updates its selected
loader-generated legacy mirror. A stale mirror is still a real raw-reference
blocker; retirement never ignores it to manufacture an unused resource. Read
precedence, unrelated slots and independent specular-glossiness/MR definitions
remain unchanged. Undo restores the exact selected original mirror data, while
mapping-only edits preserve current unrelated metadata.

The prepared successor owns a complete surviving image map, tombstone-aware
sampling metadata and descriptor inputs. History owns selected actual image
leases. The common descriptor preparation and writer serve both material edits
and resource retirement. Resource publication keeps material, object and
geometry facts unchanged while replacing the required frame, ray and DDGI
descriptor consumers after reader retirement. Consumed preparation objects keep
old owners and pools alive until outside the allocation-free publication window.

The editor's small Resources catalog is independent of a selected surface or
source material. Choices carry weak asset, catalog revision, domain, row and
incarnation intent. Selection does not bind a texture. Delete repeats current
reference and renderer preparation; stale requests reject. Undo and redo retain
the original retirement receipt. Source and runtime row addresses stay stable;
both save paths project resource holes only in detached export models.

Implementation and acceptance are separate. The native resource-retirement
checker checks catalog phases, older history replay, failures, unchanged raster
images and portable dense exports. Backend Vulkan tests must separately prove
actual image capability invalidation and allocator release after final history
and reader retention end. These contracts do not establish acceptance by
themselves or complete the remaining #108/#121 scope.

## Validation boundaries

An owned material append retains a registration-born `ResourceAppendCommand`.
Undoing that creation suspends the original material; redo resumes the current
active endpoint captured at suspension. Independent Delete commands keep their
ordinary retirement authority. Thus Make Unique, factor edit, Assign original,
Delete can be undone and redone using the same original receipts. A competing
ordinary Delete/restore cycle still invalidates its predecessor, and a prepared
generation from before suspension cannot become current again after resume.

Animation curve compatibility belongs to fallible preparation. Material
publication retains a bounded exact witness of the raw animation metadata,
selected encoded inputs and compatible decoded library. Final guards and direct
exchange compare that witness without reparsing or allocating. Changed raw
channels or selected input bytes invalidate a ready preparation before any
catalog, descriptor or player mutation; a previous successful readiness check
does not authorize later stale input.

CPU suites cover reference proof, rest-state preservation, compaction and catalog
authority. Allocation-denial checks cover publication boundaries and compaction
failure atomicity. Vulkan suites exercise material transactions and actual image
lifetime, aliases, batch migration, rollback and reader retirement. The material
retirement native harness separately checks deletion, current-clock history,
failed requests and dense save/reload in forward and deferred raster. Passing
native checks, real editor interaction, and remaining BIM/ray/DDGI acceptance
must be recorded against their actual executable; component tests alone do not
prove all those paths. Full image/texture deletion acceptance remains incomplete.

Validation of the logical resource catalog/history extension is recorded
separately against the resulting executable in `docs/feature-parity-progress.md`.
The runtime-retirement milestone includes source/runtime resource controls,
dense export and native portable save/reload; it does not establish a new
Windows desktop Delete interaction or complete external-reader acceptance.
