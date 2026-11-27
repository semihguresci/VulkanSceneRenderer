# Export a BIM scene for OpenUSD robotics tools

`BimRoboticsUsd` reads native IFC STEP or IFCX and writes one standalone `.usda`
stage. The exporter runs on the CPU and can be built with Jolt, URDF and the
TinyUSDZ importer disabled. The production exporter requires no OpenUSD SDK.

## Export the sample

Use Developer PowerShell with the repository's pinned vcpkg and Vulkan SDK
setup. [Build and test](build-and-test.md)

```powershell
$build = 'out/build/robotics-usd'
$output = 'out/robotics-usd-demo'
cmake --preset windows-release -B $build `
    -DENABLE_TESTS=ON -DENABLE_PHYSICS=OFF -DENABLE_ROBOTICS_URDF=OFF `
    -DENABLE_TINYUSDZ_USD_LOADER=OFF -DENABLE_SAMPLE_MODEL_DOWNLOAD=OFF `
    -DENABLE_BIM_SAMPLE_MODEL_DOWNLOAD=OFF -DENABLE_USD_SAMPLE_MODEL_DOWNLOAD=OFF
cmake --build $build --config Release --target BimRoboticsUsd bim_robotics_usd_tests
New-Item -ItemType Directory -Force $output | Out-Null
& "$build/BimRoboticsUsd.exe" tests/fixtures/robotics/lidar-corner.ifc `
    "$output/corner.usda" lidar-corner --import-scale 2.5
ctest --test-dir $build -C Release --output-on-failure -R '^bim_robotics_usd_tests$'
```

Choose a persistent source namespace for repeat exports of the same asset.
Use `--metres-per-renderer-unit N` for a source with unknown length units once
you know how many metres one imported world unit represents. This override
applies after the importer has used `--import-scale`. The sample uses authored
millimetres and compensates for the renderer import scale automatically.

## Stage contract

| Stage data | Authored representation |
| --- | --- |
| Frame | `/World` default prim, `metersPerUnit = 1`, `upAxis = "Z"`. REP-103 uses +X forward, +Y left and +Z up. The stage remains building-local. |
| Products | `Xform` prims retain the source affine placement in the composed hierarchy. `ifc:guid`, `ifc:sourceId`, `ifc:class`, `robotics:entityId` and `robotics:kind` preserve provenance. `SemanticsLabelsAPI:class` authors the robotics class label. |
| Hierarchy | Unambiguous, acyclic containment, aggregation, nesting and child relationships define the prim tree. All retained authored relationships also appear as `ifc:*` relationship targets. Other roots are direct children of `/World`. |
| Visual meshes | Triangle meshes retain local winding, source normals, sidedness and independent occurrence transforms, including reflections. `robotics:representationId` identifies the source representation. Standalone geometry appears under `/World/Unclassified`. |
| Materials | `/World/Materials` contains bound `UsdPreviewSurface` shaders with color, opacity, emissive, metallic, roughness and IOR. The supported color and opacity streams are exposed as display primvars. |
| Collision | `/World/Colliders` contains cooked guide meshes with `PhysicsCollisionAPI`, `PhysicsMeshCollisionAPI` and `physics:approximation = "none"`, or box proxies with `PhysicsCollisionAPI`. Each proxy targets its owning product with `robotics:entity`. |

The native STEP conversion maps source +X to robotics −Y, source +Y to
robotics +X and source +Z to robotics +Z. Product and representation frames are
converted independently and then expressed relative to their authored parent.
Product prim names derive from the stable semantic entity ID. Keep the
source hierarchy and namespace stable when using prim paths as persistent keys.
For repeated IFCX instances sharing one GUID, use `robotics:entityId` as the
unique key. IFCX visual and collision placement uses the authored double-precision
transform. A drawable with an invalid authored transform or ancestor is omitted
and diagnosed. [Coordinate conventions](coordinate-conventions.md#semantic-bim-export-for-robotics)

Collision proxies describe the imported pose. Doors and furniture boxes are
static in this exchange stage. The stage preserves the original collider recipe
in `robotics:colliderKind` for consumers that assign runtime behavior. Native
curve and point drawing, texture networks, advanced material layers,
articulations and animation are outside this exporter. Import warnings,
omitted data and ambiguous relationships are recorded in
`/World.robotics:diagnostics`. A failed import or undefined metre scale leaves
an existing destination intact.

## Validate with OpenUSD or Isaac Sim

Use a Python environment with `usd-core` installed. The external check opens
the exported files with OpenUSD and verifies the composed poses, material
binding, visual and collider bounds, GUIDs, hierarchy and stable IDs. It also
compares repeated exports at the same renderer import scale byte for byte and
checks precise IFCX occurrences and malformed IFCX placements.

```powershell
python -B tests/validation/robotics_usd_roundtrip.py `
    --exporter "$build/BimRoboticsUsd.exe" --output-dir "$output/openusd-check"
```

For Isaac Sim, use its bundled `python.bat` and add `--isaac`. This opens the
stage in a headless session and uses PhysX to check the collider identity and
ray distance of both walls. The script exits with a skip status if its selected
SDK is unavailable. [Isaac Sim standalone workflow](https://docs.isaacsim.omniverse.nvidia.com/latest/introduction/workflows.html)

```powershell
& 'C:\isaacsim\python.bat' tests/validation/robotics_usd_roundtrip.py `
    --exporter "$build/BimRoboticsUsd.exe" --output-dir "$output/isaac-check" --isaac
```
