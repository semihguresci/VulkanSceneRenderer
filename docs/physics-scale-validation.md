# Collider growth and authored source units

`tests/validation/physics_scale_regression.py` creates two small, local fixtures
and captures each in forward and deferred raster rendering. It uses the
`objectScale` capture track to multiply the authored source parent's basis
columns. Track values are relative to that original transform, so a source
scale of `.001` followed by a relative factor `1.005` produces `.001005`.

The static-growth fixture lets a BIM dynamic cube and a distant primary glTF
cube sleep with gravity disabled. At frame 91 a glTF static cube grows from
half extents `(.5, .5, .5)` to `(5, .5, 2)`. The BIM sleeper at `(4, 0, 0)` lies
outside the old collider bounds and inside the new collider. It must wake and
separate through collision response; the remote cube at `(20, 0, 0)` must keep
its authored pose. This catches insertion of a new static collider that would
otherwise leave a sleeping body embedded in its new bounds.

The source-unit fixture imports vertices at `+/-10000` with authored scale
`.001`. A relative `1.005` scale edit at frame 4 must remain visible after
subsequent simulation updates: the rendered basis is `.001005` and the cube's
world extent is `20.1` units. An absolute scale tolerance of `1e-5` would miss
this valid edit and restore the old rendered size.

Both sequences clear physics before unloading the model. The scale track
targets the imported parent by name and node index and continues beyond
unload, exercising suspension of retired animation targets. The checker also
compares actual rendered and scene matrices, derives BIM bounds and normal
transforms from the authored vertices, checks stable product identity and
geometry counts, verifies uninterrupted fixed-step playback and requires
different PNG hashes before and after the scale change.

```powershell
$env:CONTAINER_RUN_GPU_PHYSICS = '1'
python -B tests/validation/physics_scale_regression.py `
  --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe `
  --output out/physics-scale-review --require-opt-in
```

Use `--case unit-scale --technique deferred-raster` for a focused probe.
`--check-existing` rechecks saved results and removes old pass records before
validation. Native captures require exclusive use of the renderer and must run
without concurrent application builds. Reports bind captures to the executable
SHA-256. The complete matrix contains four cases and 30 captures.

The CPU checker tests are independent of GPU execution:

```powershell
python -B -m unittest discover -s tests/validation -p test_physics_scale_validation.py
```

They include valid synthetic records and failures for the two original bugs,
unexpected wakeup, stale bounds/normals/render matrices, lost source identity,
paused playback, missing lifecycle retirement and incomplete evidence. Native
pass results must be established from the application captures; synthetic
checker records alone do not establish renderer correctness.

## Verified review pass

Visual Studio Debug and Release applications built successfully and passed 218
selected C++ cases in each configuration, including 15 new regressions for this
review. The Python checkers passed 94 cases; one optional JSON Schema case
skipped because `jsonschema` was unavailable. All four offline dependency
configuration cases passed.

Release passed this suite's four cases and 30 captures, the scene diversity
suite's 16 cases and 113 captures, and the moving-support suite's four cases and
32 captures. Debug also passed both scale conditions in deferred rendering
with 15 captures. Together these cover 26 native cases and 190 captures, with
matching physics, scene and rendered geometry and no Vulkan validation errors.
Representative before/after static-growth images were visually inspected.

Build logs, per-configuration test XML and executable hashes, captures and the
source-hash validation summary are under `out/physics-edit-review`. The final
`validation-summary.json` verifies saved telemetry against its sidecars and
binds every native report to the tested executable. Physics-disabled frontend
integration and application objects were freshly compiled; a complete disabled
application was not built or launched.
