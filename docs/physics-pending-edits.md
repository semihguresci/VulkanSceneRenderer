# Pending transforms when playing or detaching

`tests/validation/physics_pending_edit_regression.py` covers two same-frame
input sequences in both raster rendering techniques. Each fixture creates a
primary glTF dynamic cube with zero gravity and an initial X velocity of one
scene unit per second. Its imported source parent is selected by name for an
`objectTranslation` track. Adjacent keys hold zero through frame 8, jump to
`[4,2,0]` at frame 9 and hold that offset through unload.

The **pending-play** condition pauses at frame 8, then edits the parent and
presses Play at frame 9. The body must begin its first simulation batch at the
edited world position, retaining the child's previously simulated local offset
and its existing velocity. The edit is consumed once; applying it again before
each substep would erase most of that batch's movement.

The **pending-detach** condition attaches an unrelated BIM dynamic cube moving
along Z. After pausing at frame 8, frame 9 edits the primary parent while
detaching the BIM product. Publishing remaining body transforms must preserve
the pending primary edit. Play resumes at frame 10. Reattaching the BIM product
as static at frame 11 exposes its actual rendered matrix, bounds and normals:
they must return to the original source placement, including after its earlier
motion. Product identity and imported geometry counts remain stable.

The runner deliberately supplies `--fixed-dt 0.06666666666666667` after the
shared capture helper's default `1/60` argument. The existing sequential command
line parser applies the later value, giving four physics ticks per playing
render frame. This exercises repeated teleport errors within the first batch.
No simulation time should pass while paused. Both sequences clear physics at
frame 23 and unload at frame 27 while the named translation track continues.

The checker predicts motion independently from the configured initial velocity
and pinned Jolt damping. Jolt 5.6 defaults linear damping to `.05` and multiplies
velocity by `1-.05*dt` before each integration. A geometric series predicts
displacement; post-edit motion starts from the captured paused pose plus the
known `[4,2,0]` offset. Position tolerance is `.0003` world units, well below the
roughly `.05` displacement lost by repeating the edit across four ticks. The
checker also compares actual scene/render matrices and authored mesh centers,
derives BIM bounds and inverse-transpose normals from source vertices, and
requires nonempty images with changed hashes.

```powershell
$env:CONTAINER_RUN_GPU_PHYSICS = '1'
python -B tests/validation/physics_pending_edit_regression.py `
  --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe `
  --output out/physics-pending-edit-review --require-opt-in
```

Use `--case pending-detach --technique deferred-raster` for a focused probe.
`--check-existing` revalidates saved records and removes old pass results before
checking. Capture runs require exclusive renderer/GPU use and an unchanged
executable; reports include its SHA-256. The full matrix contains four cases
and 40 captures. Native pass results must come from completed application runs.

```powershell
python -B -m unittest discover -s tests/validation -p test_physics_pending_edit_validation.py
```

CPU checker tests include coherent but incorrect geometry for lost/repeated
edits, lost child offsets and velocities, unrestored BIM placement, wrong
fixed-step counts, stale transforms/bounds/normals, incomplete lifecycle state
and invalid or unchanged image evidence. These tests validate the checker and
do not establish native renderer correctness on their own.

## Verified review pass

Visual Studio Debug and Release applications built and passed 228 selected C++
cases in each configuration, including ten new cases for pending edits and demo
ownership. The Python physics checkers passed 111 cases; one optional JSON Schema
case skipped because `jsonschema` was unavailable. Four offline dependency
configuration cases also passed.

Release passed all four pending-edit cases and 40 captures. Debug passed both
conditions in deferred rendering with 20 captures, and the saved Debug telemetry
was rechecked after strengthening validation of the complete BIM proxy matrix.
Representative before/after detach images were visually inspected.

Release also passed the demo, moving-support, collider-scale and scene-diversity
suites, including their ray/TAA cases. Across both configurations this review
validated 34 native cases and 251 captures with matching physics, scene and
rendered transforms, correct lifecycle retirement and no Vulkan validation
errors. The demo floor comparison found no extra dark pixels outside the expected
box/contact-shadow regions in either rendering technique.

Logs, per-configuration test XML and executable hashes, captures and the final
source-hash summary are under `out/physics-lifecycle-review`. The
`validation-summary.json` checks PNG hashes, saved telemetry sidecars and native
report executable identities. Physics-disabled frontend integration and
application objects were freshly compiled; a complete disabled application was
not built or launched.
