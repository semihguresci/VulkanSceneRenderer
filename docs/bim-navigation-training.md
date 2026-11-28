# Headless BIM navigation training

The policy benchmark compares the grid oracle, live Jazzy Nav2, and a learned
policy on common BIM maps. [Run the comparison](bim-navigation-policy-benchmark.md)

The optional Gymnasium environment runs a differential-drive robot in a BIM
collision world. Its native process uses the fixed-step Jolt simulator. The
Python package owns the observation space, navigation reward, episode
termination and baseline policy. Training and evaluation run without the
editor or a graphics device.

## Build and run

From a Visual Studio Developer PowerShell, follow the standard
[build setup](build-and-test.md), then build the headless tool with physics
enabled. Install the Python dependency into a separate directory and load the
adapter from the source tree. [Gymnasium API](https://gymnasium.farama.org/api/env/)

```powershell
cmake --preset windows-release -DENABLE_PHYSICS=ON
cmake --build out/build/windows-release --target BimRoboticsNavigation
python -m pip install --target out/bim-navigation-deps gymnasium==1.2.3
$env:PYTHONPATH = "$(Join-Path $PWD 'python');$(Join-Path $PWD 'out/bim-navigation-deps')"
$tool = 'out/build/windows-release/BimRoboticsNavigation.exe'
$scenes = 'tests/fixtures/robotics/navigation'
python -B -m bim_navigation.evaluate --executable $tool --train "$scenes/doorway-train.json" --held-out "$scenes/corner-eval.json" --held-out "$scenes/offset-hall-eval.json" --episodes 12 --seed 20261127 --output out/bim-navigation-evaluation.json
```

The three manifests describe separate model families. The training fixture
has a doorway and an authored movable prop. The held-out fixtures use a
corner and an offset hall. The evaluator checks model source hashes,
normalized geometry fingerprints, family IDs and split membership before
running episodes. Equivalent copies and unit-converted copies belong to the
same split. Episode seeds are derived from the root seed and each scene's
identity, so changes to the training list leave the held-out seeds intact.

## Baseline results

The command above measured a Windows Release build on a Ryzen 9 3900X with
Python 3.13 and Gymnasium 1.2.3. Each scene used one simulator process. Rates
include episode resets and actions after process startup.

| Scene | Split | Goals reached | Collisions | Time limits | Actions/s |
| --- | --- | ---: | ---: | ---: | ---: |
| Doorway | Train | 12/12 | 0 | 0 | 1,784 |
| Corner | Held out | 12/12 | 0 | 0 | 1,833 |
| Offset hall | Held out | 9/12 | 0 | 3 | 1,943 |

The reactive controller completed 21 of 24 held-out episodes. The three
remaining episodes reached the action limit in the offset hall.

## Parallel rollouts and profiling

Add `--workers 4 --profile` to the evaluator command to run episodes for
each scene concurrently. The effective worker count is capped at the
number of episodes per scene. Each worker owns one headless simulator process
and reuses it across its assigned episodes. Episode seeds and the order of
the result records are independent of worker count and completion order.
The workers use CPU physics and require no Vulkan context or GPU.

`metrics.wallSeconds` measures the rollout window from the first worker's
first reset to the last worker's completed episode. The reported episodes
and actions per second use this interval. The report-level
`profile.wallSeconds` covers split preflight, worker startup, all rollouts,
and worker shutdown. Each scene's worker profile lists startup, reset,
policy, step and episode record times. Adding each phase across workers
measures their combined work, which can exceed the elapsed wall time.
Step time includes the native simulation, subprocess communication and
processing the resulting observation in Python.

Use the benchmark runner to measure end-to-end throughput and peak resident
memory with the same fixtures and seed. Run the command once per worker count
with a separate output path so each width starts in a fresh Python process.
The benchmark's `summary.episodesPerSecond` uses the report-level wall time.
Matching `episodeDigest` values confirm that runs produced the same episode
records.

```powershell
python -B tools/bim_navigation_rollout_benchmark.py --executable $tool `
    --train "$scenes/doorway-train.json" --held-out "$scenes/corner-eval.json" `
    --held-out "$scenes/offset-hall-eval.json" --episodes 20 --seed 20261127 `
    --workers 4 --output out/bim-navigation-rollout-workers-4.json
```

On Windows and Linux the benchmark records resident memory high-water marks
for the Python coordinator and each rollout worker's native simulator. The
benchmark sums the native peaks within each scene and reports the largest
scene sum as `nativePeakResidentBytesUpperBound`. Individual worker peaks can
occur at different times, and shared pages can be counted in multiple
processes. Missing memory counters appear as `null`.

### Measured throughput and memory

Three fresh runs per worker count used a Ryzen 9 3900X on Windows 11 with
Python 3.13 and Gymnasium 1.2.3. The runs used a deliberately unavailable
Vulkan driver manifest. Each run used seed `20261127` and 20 episodes per
scene in `doorway-train`, `corner-eval`, and `offset-hall-eval`.

| Workers | Median episodes/s | Run range (episodes/s) | Peak Python resident MiB | Native resident upper bound MiB |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 26.48 | 26.38–27.22 | 58.84 | 9.56 |
| 2 | 33.72 | 32.93–34.25 | 58.94 | 19.05 |
| 4 | 33.08 | 32.47–34.30 | 59.99 | 37.79 |

Memory columns give the maximum across the three runs. All nine runs returned
the same episode digest, with 3,566 actions, 57 goals reached, zero collisions
and three time limits across 60 episodes per run.

With one worker, the step phase took the most time at 1.266 s across the run.
Median summed policy time rose from 0.455 s with one worker to 3.309 s with
four. For local tuning on this fixed workload, target at least 1.20× the
serial median throughput with two workers. The measured speedup was 1.27×.

## Observations and episodes

An action is a pair of signed values in `[-1, 1]`. Forward speed is scaled
to 0.8 m/s and yaw rate to 2 rad/s. Each action advances six 60 Hz physics
ticks. The robot uses metres in the building's REP-103 frame. The observation
contains the body-relative goal in forward and left metres, 36 range values
in metres, and 36 optical intensities in `[0, 1]`. Ray zero faces forward
and positive angles turn left. The optical channel uses the actual collision
hit normal with ambient and directional light. It supplies a lightweight
geometry-based cue for lighting randomization.

`reset(seed=...)` resets the simulated world, clock, action queue and sensor
noise. Each sampled episode records the source identity, seed, sampled
start and goal, furniture placement, lighting and noise settings. Scene
recipes restrict furniture to declared placement bounds. A capsule sweep
checks clearance and support through the route witness before an episode
starts. The evaluator's baseline uses the body-relative goal and local
observations. It selects no routes from the scene manifests.

Episode records store normalized actions, committed poses, collision and
success outcomes, termination or time-limit truncation, and the versioned
reward. This supports action replay from the same manifest and seed.
The `bim-navigation-progress-v1` reward adds the decrease in planar goal
distance in metres and subtracts 0.01 per action. Reaching within 0.35 m of
the goal adds 5. A physical collision subtracts 5. Goal arrival and
collision terminate an episode. The default 200-action limit truncates it.

To use the wrapper in Python:

```python
from bim_navigation import BimNavigationEnv, baseline_action

env = BimNavigationEnv(
    "out/build/windows-release/BimRoboticsNavigation.exe",
    "tests/fixtures/robotics/navigation/doorway-train.json",
)
try:
    observation, info = env.reset(seed=81)
    while True:
        observation, reward, terminated, truncated, info = env.step(
            baseline_action(observation)
        )
        if terminated or truncated:
            break
    record = env.episode_record()
finally:
    env.close()
```

## Randomized doors

To train or evaluate with doors that vary between episodes, use
`--train "$scenes/doorway-randomized.json"` in the evaluator command above. This
optional fixture belongs to the same doorway training family. The baseline
results above use `doorway-train.json`.

A recipe can list doors in `randomizedDoors` using the canonical collider ID
and an open probability between 0 and 1. For example, the doorway fixture can
use `[{"id":"bim/7:doorway/guid/9:door-guid","openProbability":0.5}]`.
`alternateWitnessRoutes` is an ordered array of routes, each an array of
`[x, y, z]` points in REP-103 metres. The sampler checks the primary
`witnessWaypoints` route and then the alternates in order.

Nominal resets (`"randomize":false` in the native protocol or `--no-randomize`
in the evaluator) use the door defaults from `openDoorIds`. Randomized resets
draw the door states from the episode seed independently of the other sampled
settings. Those states stay fixed across the bounded layout attempts. Collision
queries and sensors use the resulting door geometry. Every accepted layout has
a reachable route from the sampled start to the goal. Reset returns an error if
the attempts yield no reachable route.

The episode manifest records the effective `openDoorIds`, the configured
randomized doors as `doorStates` records with an `id` and a boolean `open`, and
the accepted route's `witnessWaypoints`. It identifies the recipe with
`sceneRecipeSha256` and the original BIM source and geometry with `sourceSha256`
and `geometryFingerprint`.

The observation-only baseline has no route planner and can time out when a
closed door blocks the direct approach. The native regression drives the robot
around the partition to verify the alternate route reaches the same goal.

## Native protocol

`BimRoboticsNavigation <scene.json>` reads one UTF-8 JSON request per line.
`describe` returns the protocol version, scene identity, sensor limits and
tick rates. `reset` returns a new manifest and committed initial observation.
`step` returns the next committed state and a collision summary for the
action's ticks. `close` ends the session. Each reply reports `ok`, and
invalid requests return an `error` string. Diagnostics use stderr.

```jsonl
{"op":"describe"}
{"op":"reset","seed":81,"randomize":true}
{"op":"step","action":[0.5,-0.1]}
{"op":"close"}
```

Run the native protocol regression and Gymnasium checker from the configured
build. The standard physics validation profiles include the native test.

```powershell
ctest --test-dir out/build/windows-release -C Release -R '^bim_navigation_(protocol|gym)$' --output-on-failure
```
