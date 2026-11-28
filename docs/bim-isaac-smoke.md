# Validate a BIM robot replay in Isaac Sim and Isaac Lab

The smoke test uses a small project-authored IFC scene with a floor and two
walls. The native simulator records a short drive and turn. A portable USD
replay retains the actual pose samples, metric geometry, IFC GUIDs, material
bindings and collision proxies. It uses a visual robot proxy for pose playback.
[BIM OpenUSD stage contract](robotics-openusd.md)

## Prepare the portable bundle

Build the native CPU tools with physics enabled. This step uses the regular
repository build and requires no Isaac SDK or rendering device.

```powershell
$build = 'out/build/isaac-smoke'
cmake --preset windows-release -B $build -DENABLE_PHYSICS=ON
cmake --build $build --config Release --target BimRoboticsUsd BimRoboticsSim BimRoboticsCollision
python -B tests/validation/robotics_isaac_smoke.py prepare `
    --exporter "$build/BimRoboticsUsd.exe" --simulator "$build/BimRoboticsSim.exe" `
    --collision "$build/BimRoboticsCollision.exe" --bundle out/isaac-corner-bundle
```

On Linux, use the Linux Release preset and executable names without `.exe`.
Keep the entire bundle together when copying it to another host. Its manifest
records the native tool hashes, input and payload hashes, sample ticks and
commands for reference. The archived IFC adds a red material and rotates one
wall's local frame while preserving its world bounds. `replay.usda` composes
the neighboring `scene.usda` by relative path. With physics enabled,
`ctest -R '^robotics_isaac_compare_tests$'`
exercises native preparation and relocation as part of the CPU validation suite.

## Compare in OpenUSD

Use a Python environment containing `usd-core`. This checks scale, the
composed stage, material bindings and semantic labels, and compares all USD
replay samples with the native trajectory. It also verifies the native ray
receipts against independent metric witnesses in the IFC corner and checks the
archived payload fingerprints against the manifest.

```powershell
python -B tests/validation/robotics_isaac_smoke.py check `
    --bundle out/isaac-corner-bundle --backend openusd --output-dir out/isaac-corner-openusd
```

## Compare in the simulator

Use Isaac Sim 6.2.0 and, for the Lab comparison, the matching Isaac Lab 3.0 GA
environment. NVIDIA's version and device requirements apply to the host that
runs the comparison. Copy the bundle and use a new output directory per run.
[Version compatibility](https://docs.isaacsim.omniverse.nvidia.com/6.2.0/installation/compatibility.html) · [Host requirements](https://docs.isaacsim.omniverse.nvidia.com/6.2.0/installation/requirements.html)

Run the Isaac Sim check with the simulator's Python launcher. On Windows:

```powershell
& 'C:/isaacsim/python.bat' tests/validation/robotics_isaac_smoke.py check `
    --bundle out/isaac-corner-bundle --backend isaac-sim --output-dir out/isaac-corner-sim
```

Use `python.sh` in a Linux Isaac Sim installation. For the Isaac Lab check,
run the same script in the Python environment configured for Isaac Lab and
select `--backend isaac-lab`. The simulator modes load the real Kit stage and
record the installed version. The Lab mode starts Kit through Isaac Lab and
checks the authored PhysX scene. [Isaac Sim standalone Python](https://docs.isaacsim.omniverse.nvidia.com/6.2.0/python_scripting/manual_standalone_python.html) · [Isaac Lab installation](https://isaac-sim.github.io/IsaacLab/main/source/setup/installation/index.html)

The simulator checks query world-space robot poses at the selected timeline
times. Real PhysX rays must hit the expected wall and slab proxies at the
native distances and miss in the open direction. Collider ownership maps each
hit to its original IFC GUID. The semantic camera check verifies that
Replicator consumes the authored class labels. [Semantic label schema](https://nvidia.github.io/simready-foundation/latest/guides/semantic_labels/semantic_labels.html)

`result.json` identifies the backend, bundle digest, comparison results,
and any failure. For simulator runs, `sdk-process.log` captures Kit output and
`sdk/` holds the runtime report and camera evidence. The default SDK timeout is
900 seconds. Use `--timeout` to allow a longer startup. A missing SDK returns
exit code 77 with status `unavailable`.
