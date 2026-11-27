# ADR 0001 Robotics module boundaries

**Status** Accepted

## Context

BIM simulation needs the importer's units, placements and product identities.
Those facts also support rendering, but physics, fixed-step replay and sensor
sampling must run with no renderer or simulator middleware. Keeping the CPU
contracts in separate C++23 libraries lets headless tools build only the
capabilities they use. [Issue #293](https://github.com/semihguresci/VulkanSceneRenderer/issues/293)

## Decision

Use the following targets. The robotics suffixes are under the
`VulkanSceneRenderer_robotics_` prefix.

| Target or suffix | Owns | Dependencies |
| --- | --- | --- |
| `clock` | Fixed ticks, fractional elapsed time and queued whole ticks. | C++ standard library. |
| `scene` | Semantic entities and relationships, unit evidence, affine frame conversion and manifest I/O. | GLM. The implementation uses Vulkan headers for the CPU import model and nlohmann/json for the manifest. |
| `collision_geometry` | Immutable collision recipes, shared triangle data, support surfaces and walkability algorithms. | `scene`. The implementation reads the CPU import model. Walkability uses caller-supplied queries. |
| `assets` | IFC/IFCX preprocessing and derived cache, source digests, semantic and collision data, and CPU render payload with optional GLB preview. | `scene`, `collision_geometry` and Vulkan headers for the public CPU render model. The implementation links geometry import, fingerprints, TinyGLTF and miniz. |
| `drive` | Differential-drive kinematics, shaft/ground actuation and planar odometry covariance. | GLM. |
| `sensors` | Rational sensor schedules, organized LiDAR scans, simulated IMU/encoders and seeded noise. | `drive`. The LiDAR query contract accepts a caller-supplied raycast. |
| `twin` | Versioned robot, BIM and operator telemetry, clock-aware ingestion, canonical event ordering, and sampled replay. | `scene` for semantic identity and source provenance, `sensors` for calibrated camera validation, GLM for poses, and private nlohmann/json. |
| `VulkanSceneRenderer_physics_world` | Jolt bodies, shapes, constraints, motors, queries and opt-in contact snapshots. | GLM and the private Jolt backend. Requires `ENABLE_PHYSICS`. |
| `collision` | Live BIM collision bodies, door state, Jolt query adapters and room-portal analysis. | `scene`, `collision_geometry`, `VulkanSceneRenderer_physics_world` and private CPU import model headers. Requires `ENABLE_PHYSICS`. |
| `simulation` | Fixed-step differential-drive state and history, articulated motor commands and grasp operations. | `collision`, `drive` and `clock`. Requires `ENABLE_PHYSICS`. |
| `fleet` | Waypoint supervision and mission audit records for robots in one simulated collision world. | `simulation` for a shared fixed clock and `twin` for recording and replay. Requires `ENABLE_PHYSICS`. |
| `diagnostics` | GLB collision overlays in renderer coordinates. | Private `collision` and geometry dependencies. Requires `ENABLE_PHYSICS`. |
| `VulkanSceneRenderer_urdf` | URDF import, authored geometry and inertial data, and kinematic joint state and poses. | GLM, `clock` and private TinyXML2. Requires `ENABLE_ROBOTICS_URDF`. |
| `VulkanSceneRenderer_urdf_physics` | Kinematic primitive bindings from URDF poses into a caller-owned world. | `VulkanSceneRenderer_urdf` and `VulkanSceneRenderer_physics_world`. Requires both options. |

The tools compose these libraries. `BimRoboticsSim` combines the asset,
simulation and sensor paths. The collision backend supplies raycasts without
linking the scan or noise models. The renderer's `VulkanSceneRenderer_physics`
target owns editor scene bindings and uses `VulkanSceneRenderer_physics_world`.
There is no dependency from the robotics targets on the renderer frontend.

The shared CMake configure requires the Vulkan SDK and Slang. Some CPU import
types carry Vulkan declarations, so their compilation also needs Vulkan
headers. The headless targets run on the CPU and build separately from the
renderer executable and shaders. The outward adapter layer owns any ROS,
Isaac or training dependency. The URDF option controls its CMake targets
independently of Jolt. The shared vcpkg manifest may still provision TinyXML2
when the option is off.

## Coordinate and unit contract

- The renderer basis is −Z forward, −X left, +Y up. The robotics basis is
  right-handed REP-103 with +X forward, +Y left, +Z up. For a renderer world
  point `(x, y, z)`, the robotics world point is `s × (−z, −x, y)`, where `s` is
  `metersPerRendererUnit`. Transforms act on column vectors and JSON matrices
  are column-major. [REP-103](https://github.com/ros-infrastructure/rep/blob/master/rep-0103.rst)
- The importer resolves source lengths and placements. The semantic scene
  records source units, import scale, and metric provenance when known. Apply
  `roboticsFromRenderer` only once, to imported renderer world coordinates.
  When source units cannot be established, preserve identities and renderer
  geometry, report the missing metric evidence, and require a verified scale
  override before metric collision processing. Preserve any CRS and offset
  provenance separately from the local frame conversion.
- Semantic `*FromLocal` transforms are double-precision affine placements.
  Source scale, reflection, and composition are retained. Collision cooking
  incorporates the affine linear part into the local triangles, corrects
  mirrored winding, and stores a metric world origin. Runtime body, joint,
  and sensor poses use rigid transforms. An adapter should bake or preserve
  affine geometry separately from a rigid transform it publishes to consumers.
- Robotics data uses metres, radians, kilograms and simulated seconds.
  World-space vectors and rigid poses use the same REP-103 axes unless the
  field names specify a base, odometry, sensor or link frame. The generic
  `PhysicsWorld` uses its caller's scene units and gravity. `BimCollisionWorld`
  configures gravity along −Z in metres per second squared. Robotics bindings
  explicitly rotate Jolt's +Y-axis capsule and cylinder primitives when an
  authored or runtime primitive is +Z-axis.
- In drive reports, `mapFromOdometry` is the resolved spawn frame and
  `odometryFromBase` is the encoder-integrated estimate. `base` in history is
  the committed pose in the map frame. `commanded`, when present, is the
  nominal pose before actuator and collision effects. A LiDAR return measures
  distance along a sensor-frame ray. The scan includes the sensor's rigid
  world pose and tick. A stationary level base-aligned IMU reports positive Z
  specific force.

## Ownership and scheduling

- The CPU import caller owns the source model. Published semantic values,
  recipes and cache outputs can be shared for read-only use. The scene and
  collision outputs carry stable occurrence IDs and authored GUIDs when
  available. Cache entries store portable CPU data keyed by input content and
  configuration. Live bodies, solver state and door state are world-owned.
- `BimCollisionWorld` owns its collision snapshot, Jolt world, shared cooked
  shapes and active body and door mappings. Its query callbacks borrow the
  world. A drive simulation borrows that world and owns the attached robot's
  command queue, clock, state and reset lifecycle. Perform all mutable world
  operations, command scheduling, stepping and live queries on the owning
  thread. Copy committed data before handing it to renderer, middleware or
  training threads.
- `PhysicsWorld::step` completes Jolt's internal jobs before returning. When
  contact tracking is enabled, only reported, non-speculative solver pairs
  appear in the latest snapshot. The deepest observed manifold reports a
  pre-solve penetration depth. The pair event stream compares successful
  steps. An end can reflect sleep. Relocation, retirement or suppression of a
  body pair invalidates active contact evidence. Read borrowed contacts before
  the next world mutation or step. A gripper borrows its world and owns its
  temporary grasp joint, created at the live relative pose after bilateral
  contact. This is a constraint-based grasp shortcut.
- `SimClock` commits one tick after successful physics work. Sensors use that
  committed state and tick time. Paused wall time adds no ticks, and queued
  work can be drained in bounded batches. Reset re-creates the authored BIM
  collision state, restores the robot spawn and the encoder seed, and pauses
  the clock while retaining commands for replay. Each drive sensor model owns
  its tick history and needs its own reset. LiDAR uses the supplied raycast's
  threading rules. Drive math takes values and holds no mutable runtime state.
  Compare reproducibility
  with the same build, fixture, seeds and tick-addressed inputs. Solver tests
  use physical tolerances where appropriate.
- `UrdfRuntime` owns its CPU model and mutable kinematic state. A URDF Jolt
  binding borrows that runtime and its world and owns its kinematic collision
  bodies. The owner synchronizes the binding between runtime updates and
  physics queries. The articulated motor/grasp primitives operate on live Jolt
  bodies separately from this kinematic import path.

## Outward integration contracts

Use the existing versioned JSON reports as the starting point for adapters.
`container.robotics-scene` carries semantic identities, frame evidence and
relationships. `container.bim-collision-report` carries queries and can include
a `container.bim-room-graph`. `container.bim-robotics-simulation` carries
tick-indexed trajectories and enabled sensor streams.
`container.urdf-kinematic-state` carries authored URDF facts and runtime poses.
The library APIs expose the corresponding CPU data directly where needed.

- A ROS 2 adapter can translate committed timestamps, transforms, joint and
  sensor data to ROS messages and enqueue incoming commands for fixed ticks.
  The adapter owns ROS node, executor and transport lifetimes. It defines frame
  IDs and localization semantics for the consumer. The source `map` and
  odometry fields alone do not establish localization or a geographic frame.
  [REP-105](https://github.com/ros-infrastructure/rep/blob/master/rep-0105.rst)
- A USD export or Isaac adapter can consume the semantic and geometry data,
  author the target stage's `metersPerUnit` and `upAxis`, and preserve product
  IDs and composed transforms as stage metadata and geometry. When sharing
  transforms with a rigid-body simulator, apply the affine-to-rigid policy
  described above. The renderer's USD import and GLB preview paths are separate
  from such an exporter. [OpenUSD stage units](https://openusd.org/release/api/group___usd_geom_linear_units__group.html)
  [OpenUSD stage axes](https://openusd.org/release/api/group___usd_geom_up_axis__group.html)
- A training adapter can define episodes around the existing reset and step
  operations, with explicit tick-addressed actions and committed observations.
  It owns feature packing, rewards, termination policy and model execution.
  For reproducible datasets, keep the source asset and producer revision, unit
  evidence, configuration, seeds and action schedule with the episode data.

These are integration boundaries for future adapters. This decision delivers
the C++ contracts, the headless tools and the existing JSON interchange.

## Repeatable headless demo

Use PowerShell in a Visual Studio Developer Command Prompt from the repository
root. Set up the SDK and vcpkg as in [Build and Test](../build-and-test.md).
With Python and CMake 3.27 or newer, `robotics_module_boundaries` checks the
generated CMake dependency graph and prints the dependencies it verified.
[CMake File API](https://cmake.org/cmake/help/latest/command/cmake_file_api.html)
First build the CPU path with Jolt and URDF disabled. The source fixture stays
read-only. This exports the native IFC semantics and preprocesses the same
source into a separate cache.

```powershell
$core = 'out/build/robotics-core'
$out = 'out/robotics-module-demo'
New-Item -ItemType Directory -Force $out | Out-Null
cmake --preset windows-release -B $core -DENABLE_PHYSICS=OFF `
    -DENABLE_ROBOTICS_URDF=OFF -DENABLE_TESTS=ON
cmake --build $core --target BimRoboticsScene BimRoboticsAssets bim_robotics_scene_tests `
    bim_collision_geometry_tests bim_asset_cache_tests raycast_lidar_tests drive_sensor_tests drive_actuation_tests
& "$core/BimRoboticsScene.exe" tests/fixtures/robotics/room-connectivity.ifc "$out/rooms.json" rooms
& "$core/BimRoboticsAssets.exe" tests/fixtures/robotics/room-connectivity.ifc "$out/cache" rooms
ctest --test-dir $core --output-on-failure `
    -R '^(robotics_module_boundaries|bim_robotics_scene_tests|bim_collision_geometry_tests|bim_asset_cache_tests|raycast_lidar_tests|drive_sensor_tests|drive_actuation_tests)$'
```

Then build the Jolt path for live collision and fixed-step replay.
The native IFC room example exports doorway connectivity and crossing evidence.
The drive fixture traverses an open door in 180 ticks and reaches
`[1.5, 0, 1.2]` metres. The corner fixture records a synchronized LiDAR stream.
Install Python before configuring to register the CLI replay test.

```powershell
$physics = 'out/build/robotics-jolt'
cmake --preset windows-release -B $physics -DENABLE_PHYSICS=ON `
    -DENABLE_ROBOTICS_URDF=OFF -DENABLE_TESTS=ON
cmake --build $physics --target BimRoboticsCollision BimRoboticsSim physics_world_tests `
    bim_collision_world_tests bim_physics_determinism_tests articulated_grasp_tests
& "$physics/BimRoboticsCollision.exe" tests/fixtures/robotics/room-connectivity.ifc `
    "$out/room-graph.json" rooms tests/fixtures/robotics/room-connectivity-queries.json
& "$physics/BimRoboticsSim.exe" tests/fixtures/robotics/doorway.ifc `
    "$out/drive.json" doorway tests/fixtures/robotics/doorway-drive.json
& "$physics/BimRoboticsSim.exe" tests/fixtures/robotics/lidar-corner.ifc `
    "$out/lidar.json" lidar-corner tests/fixtures/robotics/lidar-corner-scan.json
ctest --test-dir $physics --output-on-failure `
    -R '^(robotics_module_boundaries|physics_world_tests|bim_collision_world_tests|bim_physics_determinism_tests|articulated_grasp_tests|bim_fixed_step_replay)$'
```

The reports carry scene and collision diagnostics, stable identities and
explicit units or provenance. See [Physics Integration](../physics-integration.md)
for cache and sensor options, the independent URDF demo, and replay tolerances.
