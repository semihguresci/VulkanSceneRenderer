# Compare BIM navigation policies

The benchmark uses the same exported floor, IFC source, door states, spawn,
goal, and capsule for every method. The two driven policies share velocity
limits and a time budget. The suite compares an eight-neighbor
shortest-path oracle, Jazzy NavFn with Regulated Pure Pursuit, and a tile-coded
Q-learning policy trained in the headless BIM simulator.
[Versioned suite configuration](../tests/fixtures/robotics/navigation/policy-benchmark-v1.json)

## Run the benchmark

Build the simulator, map exporter, and collision verifier on Ubuntu 24.04.
Install the optional bridge and Nav2 components using the Jazzy instructions,
and source the ROS environment and installed bridge in the same shell.
[ROS 2 Jazzy bridge setup](bim-ros2-bridge.md)

```bash
build=out/build/linux-release
cmake --build "$build" --target BimRoboticsSim BimRoboticsNavMap BimRoboticsCollision
python3 -B tests/robotics/bim_navigation_policy_benchmark.py \
  --config tests/fixtures/robotics/navigation/policy-benchmark-v1.json \
  --simulator "$build/BimRoboticsSim" --exporter "$build/BimRoboticsNavMap" \
  --collision "$build/BimRoboticsCollision" --fixtures tests/fixtures/robotics \
  --output out/navigation-policy-benchmark
```

Use a fresh output directory. The runner regenerates maps and rejects an
oracle path if any native capsule sweep fails. It preserves per-trial ROS
logs, the unmodified NavFn path, native drive traces, frozen checkpoints,
and the exact simulator and case fingerprints. `comparison.json` contains
per-trial scores, aggregate estimates, and paired intervals.

## How trials are scored

- The geometric oracle uses eight-neighbor grid edges without diagonal corner
  cutting. It applies NavFn's one-cell guard and independently sweeps its
  route against the original BIM collision geometry.
- The Nav2 baseline sends the returned path to the live Jazzy controller.
  The benchmark checks the published occupancy grid, waits for a stationary
  spawn, and records the controller's actual native motion.
- The learned policy uses 32 LiDAR beams and a body-relative goal derived from
  odometry. Three independent seeds train on the doorway family. The runner
  freezes each model at 0, 3,000, 10,000, and 24,000 action transitions and
  runs every checkpoint on the same holdouts without further learning.
- A driven trial reaches the goal on the first committed tick within 0.25 m
  horizontally. A native `blocked` event counts as a collision. An episode
  can use up to 1,800 native ticks at 60 Hz and policy decisions at 10 Hz.
  The command limits are 0.40 m/s and 0.65 rad/s.
- Success-weighted path efficiency divides the grid reference length by the
  greater of the reference and driven lengths for successful trials. A
  failed trial contributes zero. This keeps the score bounded when a
  continuous route or the goal tolerance makes the actual drive shorter.

The four scored routes come from three holdout families. The two room floors
share one family. One disconnected lower-floor case is reported separately
as a control. The estimates weight buildings equally and use paired bootstrap
intervals over families and whole method repeats or training seeds. Three
holdout families give descriptive intervals for this suite. The oracle's
geometry checks and controller outcomes are reported separately. Wall time
includes the middleware and real-time pacing for Nav2. The learned policy
steps the same native simulator faster than real time. The report also
records native simulation time and setup overhead for each trial.

## Reference run

On the recorded Jazzy Linux environment, Nav2 completed all 12 held-out
trials with zero collisions. The untrained policy completed 6 of 12 and
recorded six collisions. At 24,000 transitions per seed, it completed 4 of 12
and recorded one collision. The reference includes every checkpoint,
per-route outcomes, and confidence intervals. A full rerun produced the
same trial outcomes and all 12 checkpoint files matched byte for byte.
[Measured results](../tests/benchmarks/results/bim-navigation-policy-jazzy.json)
