# Preview URDF robots in the renderer

`BimRoboticsUrdf` converts a URDF robot and its joint scenario to a portable
animated `.glb`. The renderer opens it through its normal glTF scene loader.
The existing `.json` output retains the robot’s kinematic state in ROS frames.
Build the converter with `ENABLE_ROBOTICS_URDF=ON`. Jolt and ROS are optional.

```powershell
$build = 'out/build/windows-release'
$demo = 'out/urdf-preview'
New-Item -ItemType Directory -Force $demo | Out-Null
cmake --preset windows-release -DENABLE_ROBOTICS_URDF=ON
cmake --build $build --config Release --target BimRoboticsUrdf VulkanSceneRenderer
& "$build/BimRoboticsUrdf.exe" tests/fixtures/robotics/two-link-arm.urdf `
    "$demo/arm.glb" tests/fixtures/robotics/two-link-arm-pose.json
& "$build/VulkanSceneRenderer.exe" --model "$demo/arm.glb" --animation-clip 0
```

The two-link arm moves through its authored shoulder and elbow axes. In the
editor, use **Scene Controls → Go to → Animation / authored cameras** to seek or
pause the `URDF joint motion` clip. Use **Go to → glTF scenes** to switch between
the `Visual` and `Collision` views. For a collision-only robot, the collision
view opens by default.

## Frames, assets, and motion

- The converter retains the link tree and separate joint-origin, joint-motion,
  visual, and collision nodes. The root converts REP-103 metres to glTF and
  renderer coordinates as `(x, y, z) → (-y, z, -x)`. Child nodes retain the
  authored local frames, including nonuniform and signed mesh scales.
- Box, sphere, +Z cylinder, and local binary or ASCII STL geometry are bundled
  in the GLB. Relative paths resolve beside the URDF. For
  `package://robot_preview/meshes/wedge.stl`, pass `--package-root` with the
  directory containing `robot_preview`. Supply additional roots by repeating
  the option. The exporter reports unresolved and unsupported meshes.
- URDF RGBA is authored as a glTF material. URDF texture references are
  reported separately because the supported shapes supply no authored UVs.
- The scenario’s `positions` set the initial pose. `targets` and `ticks`
  advance the same fixed-step `UrdfRuntime` used by the JSON report. GlTF
  translation and rotation tracks preserve prismatic travel, revolute motion,
  and complete continuous turns. Omit the scenario for a static preview.
- The collision scene is a geometry inspection view. Runtime primitive
  collisions use `UrdfCollisionBindings` when physics is enabled.
  [URDF runtime and physics](physics-integration.md#import-and-pose-an-articulated-urdf-robot)

Try a mirrored mesh on a prismatic joint using the packaged fixture.

```powershell
& "$build/BimRoboticsUrdf.exe" tests/fixtures/robotics/mesh-slider.urdf `
    "$demo/slider.glb" --package-root tests/fixtures/robotics
```

## Verify the frame agreement

The headless check reads the emitted GLB, composes both scene variants, and
compares link and shape placements with the JSON poses and numeric witnesses.
With `--renderer`, it also captures two poses and compares the renderer’s
published node matrices and image pixels. The native check requires Pillow,
Vulkan, a window system and a Khronos validation layer.

```powershell
ctest --test-dir $build -C Release -R '^urdf_asset_roundtrip$' --output-on-failure
python -B tests/validation/urdf_asset_roundtrip.py --exporter "$build/BimRoboticsUrdf.exe" `
    --renderer "$build/VulkanSceneRenderer.exe" --output-dir "$demo/native-check"
```

From a ROS 2 environment with `rclpy`, TF2, and Robot State Publisher, run the
optional reference check. It publishes the joint states from the neutral and
posed reports, checks the resulting link TF and visual origins, and saves the
comparison. The ROS processes use an isolated localhost DDS domain.
[Robot State Publisher](https://github.com/ros/robot_state_publisher/tree/jazzy)

```powershell
python -B tests/validation/urdf_ros_reference.py --exporter "$build/BimRoboticsUrdf.exe" `
    --output-dir "$demo/ros-reference"
```
