#!/usr/bin/env python3
"""Train and evaluate a tile-coded Q policy on the headless BIM drive stream."""

import argparse
from bisect import bisect_right
from collections import defaultdict
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import random
import sys
import tempfile
import time

for parent in Path(__file__).resolve().parents:
    package_root = parent / "ros2" / "bim_ros_bridge"
    if package_root.is_dir():
        sys.path.insert(0, str(package_root))
        break
from bim_ros_bridge.transport import DriveStream, native_input_path


TICKS_PER_SECOND = 60
TICKS_PER_ACTION = 6
GOAL_TOLERANCE = 0.25
DRIVE_HORIZON = 1800
BLOCKED_TICKS = 18
LIDAR_BEAMS = 32
LIDAR_RANGE = 4.0
ACTIONS = ((0.4, 0.0), (0.25, 0.45), (0.25, -0.45),
           (0.0, 0.65), (0.0, -0.65), (0.0, 0.0))
GAMMA = 0.99
ANGLE_STEP = 2 * math.pi / LIDAR_BEAMS
BEAM_ANGLES = tuple(-math.pi + beam * ANGLE_STEP for beam in range(LIDAR_BEAMS))


def json_text(value):
    return json.dumps(value, allow_nan=False, sort_keys=True, separators=(",", ":"))


def write(path, value):
    path.write_text(json_text(value) + "\n", encoding="utf-8")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate_case_split(cases):
    train, test = cases["train"], cases["test"]
    require(train and test, "Both splits need at least one case")
    require({case.family for case in train}.isdisjoint(case.family for case in test),
            "Training and test families overlap")
    require({case.manifest["imageSha256"] for case in train}.isdisjoint(
                case.manifest["imageSha256"] for case in test),
            "Training and test occupancy maps overlap")

    def source_identities(group):
        return {tuple(layer["sha256"] for layer in case.manifest["sceneSource"]["layers"])
                for case in group}

    require(source_identities(train).isdisjoint(source_identities(test)),
            "Training and test BIM source fingerprints overlap")


def write_trace_row(trace, state, command):
    if trace is not None:
        trace.write(json_text({
            "schema": "container.bim-navigation-learner-trace", "version": 1,
            "tick": state["tick"], "timestampNanoseconds": state["timestampNanoseconds"],
            "command": command, "base": state["base"], "obstruction": state["obstruction"],
            "blockingEntityId": state["blockingEntityId"],
        }) + "\n")


@dataclass(frozen=True)
class Case:
    name: str
    family: str
    source: Path
    map_directory: Path
    manifest: dict
    start: tuple
    goal: tuple
    yaw: float

    @classmethod
    def load(cls, name, family, source, map_directory):
        manifest = json.loads((map_directory / "waypoints.json").read_bytes())
        require(manifest.get("schema") == "container.bim-navigation-map" and manifest.get("version") == 1,
                f"Invalid exported map for {name}")
        raw = source.read_bytes()
        identity = {"byteSize": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        require(manifest["sceneSource"]["layers"] == [identity], f"{name} source differs from the exported map")
        waypoints = {point["id"]: point for point in manifest["waypoints"]}
        start, goal = waypoints["start"], waypoints["goal"]
        require(len(start["positionMeters"]) == len(goal["positionMeters"]) == 2, f"Invalid {name} waypoints")
        return cls(name, family, source, map_directory, manifest, tuple(start["positionMeters"]),
                   tuple(goal["positionMeters"]), start["yawRadians"])

    def scenario(self, yaw):
        m = self.manifest
        return {"version": 1, "initialFeet": [*self.start, m["floorMeters"]], "initialYaw": yaw,
                "ticksPerSecond": TICKS_PER_SECOND, "ticks": DRIVE_HORIZON, "openDoorIds": m["openDoorIds"],
                "capsule": {"radius": m["radiusMeters"], "height": m["heightMeters"]},
                "lidar": {"scansPerSecondMilliHz": 10000, "azimuthSamples": LIDAR_BEAMS,
                          "minRangeMeters": 0.05, "maxRangeMeters": LIDAR_RANGE, "mount": {"xyz": [0, 0, 0.45]}}}


def observation(state, goal):
    """The policy sees body-relative goal and lidar, with pose inferred from odometry."""
    alignment = state["transforms"]["mapFromOdometry"]
    odom = state["odometry"]
    x, y, z = odom["position"]
    world_xy = tuple(alignment[3][i] + alignment[0][i] * x + alignment[1][i] * y + alignment[2][i] * z
                     for i in (0, 1))
    delta_x, delta_y = (b - a for a, b in zip(world_xy, goal, strict=True))
    yaw = math.atan2(alignment[0][1], alignment[0][0]) + odom["yaw"]
    bearing = math.remainder(math.atan2(delta_y, delta_x) - yaw, 2 * math.pi)
    ranges = tuple(LIDAR_RANGE if value == "no-hit" else 0.0 if isinstance(value, str) else value
                   for value in state["lidar"]["rangesMeters"])
    require(len(ranges) == LIDAR_BEAMS, "LiDAR observation is missing beams")
    return math.hypot(delta_x, delta_y), bearing, ranges


def features(obs):
    distance, angle, ranges = obs
    front = min(r for a, r in zip(BEAM_ANGLES, ranges) if abs(a) <= 0.42)
    left = min(r for a, r in zip(BEAM_ANGLES, ranges) if 0.42 < a <= 1.6)
    right = min(r for a, r in zip(BEAM_ANGLES, ranges) if -1.6 <= a < -0.42)
    tiles = []
    for offset in (0.0, 0.10):
        heading = bisect_right((-2, -1, -.55, -.24, -.10, .10, .24, .55, 1, 2), angle + offset * .5)
        depth = bisect_right((.4, .72, 1.2, 2.0, 3.5), distance + offset)
        obstacle = bisect_right((.29, .44, .7, 1.15, 2.0), front + offset * .35)
        side = int(left > right + .12) - int(right > left + .12)
        tiles.extend(((offset, "goal", heading), (offset, "approach", heading, depth),
                      (offset, "clearance", heading, obstacle, side)))
    return tiles


class TileQ:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.weights = defaultdict(lambda: [0.0] * len(ACTIONS))
        self.steps = 0

    def values(self, tiles):
        rows = [self.weights.get(tile) for tile in tiles]
        return [sum(row[i] for row in rows if row is not None) for i in range(len(ACTIONS))]

    def choose(self, tiles, epsilon=0.0):
        if epsilon and self.rng.random() < epsilon:
            return self.rng.randrange(len(ACTIONS))
        return max(enumerate(self.values(tiles)), key=lambda row: row[1])[0]

    def learn(self, tiles, action, reward, successor, terminal):
        target = reward if terminal else reward + GAMMA * max(self.values(successor))
        update = .16 * (target - self.values(tiles)[action]) / len(tiles)
        for tile in tiles:
            self.weights[tile][action] += update
        self.steps += 1

    def checkpoint(self):
        return {"schema": "container.bim-navigation-tile-q-prototype", "version": 1,
                "transitions": self.steps, "actions": ACTIONS,
                "weights": [[tile, values] for tile, values in sorted(self.weights.items())]}

    @classmethod
    def from_checkpoint(cls, document):
        require(document["schema"] == "container.bim-navigation-tile-q-prototype" and document["version"] == 1,
                "Unsupported navigation checkpoint")
        require(tuple(map(tuple, document["actions"])) == ACTIONS, "Checkpoint action space differs")
        policy = cls(0)
        policy.weights.update((tuple(key), values) for key, values in document["weights"])
        policy.steps = document["transitions"]
        return policy


def episode(executable, case, yaw, policy, *, epsilon=0.0, train=False, limit=None, save_checkpoint=None, trace=None):
    wall_start = time.perf_counter()
    travelled = 0.0
    elapsed = 0
    action_steps = 0
    outcome = "budget"
    reward_sum = 0.0
    obstruction_streak = 0
    with tempfile.TemporaryDirectory(prefix="bim-nav-287-") as tmp:
        scenario = Path(tmp) / "scenario.json"
        write(scenario, case.scenario(yaw))
        command = [str(executable), "--stream", native_input_path(executable, case.source),
                   case.manifest["sourceNamespace"], native_input_path(executable, scenario)]
        with DriveStream(command) as sim:
            desc = sim.request("describe", timeout=20)
            require(desc["sceneSource"] == case.manifest["sceneSource"] and desc["openDoorIds"] == case.manifest["openDoorIds"],
                    f"{case.name} native world identity does not match exported map")
            state = sim.request("observe")
            before = state["base"]["position"]
            require(math.dist(before, (*case.start, case.manifest["floorMeters"])) < .005,
                    f"{case.name} native spawn {before} differs from exported start {case.start}")
            obs = observation(state, case.goal)
            write_trace_row(trace, state, None)
            drive_start = time.perf_counter()
            while elapsed < DRIVE_HORIZON:
                tiles = features(obs)
                action = policy.choose(tiles, epsilon)
                speed, turn = ACTIONS[action]
                command = {"linearMetersPerSecond": speed, "angularRadiansPerSecond": turn}
                goal_distance = math.dist(before[:2], case.goal)
                for _ in range(TICKS_PER_ACTION):
                    elapsed += 1
                    state = sim.request("step", tick=elapsed, **command)
                    after = state["base"]["position"]
                    travelled += math.dist(before[:2], after[:2])
                    before = after
                    write_trace_row(trace, state, command)
                    obstruction = state["obstruction"]
                    if obstruction == "blocked":
                        outcome = "collision"
                        break
                    if math.dist(after[:2], case.goal) <= GOAL_TOLERANCE:
                        outcome = "success"
                        break
                    obstruction_streak = 0 if obstruction == "none" else obstruction_streak + 1
                    if obstruction_streak >= BLOCKED_TICKS:
                        outcome = "obstruction"
                        break
                terminal = outcome != "budget" or elapsed == DRIVE_HORIZON
                successor = None if terminal else observation(state, case.goal)
                reward = -0.01 + 10 * (goal_distance - math.dist(before[:2], case.goal))
                if successor is not None:
                    reward += .20 * (math.cos(successor[1]) - math.cos(obs[1]))
                reward += {"success": 10., "collision": -12., "obstruction": -10., "budget": 0.}[outcome]
                reward_sum += reward
                action_steps += 1
                if train:
                    policy.learn(tiles, action, reward, None if terminal else features(successor), terminal)
                    if save_checkpoint:
                        save_checkpoint(policy)
                if terminal:
                    break
                if limit is not None and action_steps == limit:
                    outcome = "training-budget"
                    break
                obs = successor
            drive_seconds = time.perf_counter() - drive_start
    return {"case": case.name, "family": case.family, "initialYaw": yaw, "outcome": outcome,
            "collision": outcome == "collision", "success": outcome == "success", "physicsTicks": elapsed,
            "transitions": action_steps, "traveledMeters": travelled, "goalErrorMeters": math.dist(before[:2], case.goal),
            "reward": reward_sum, "driveWallSeconds": drive_seconds, "setupWallSeconds": drive_start - wall_start}


def run(args):
    manifest = json.loads(args.cases.read_bytes())
    cases = {split: [Case.load(row["id"], row["family"], Path(row["source"]), Path(row["mapDirectory"]))
                     for row in manifest[split]] for split in ("train", "test")}
    validate_case_split(cases)
    executable = args.simulator.resolve(strict=True)
    args.output.mkdir(parents=True)
    policy = TileQ(args.seed)
    rng = random.Random(args.seed)
    checkpoints = {}

    def save_checkpoint(current):
        if current.steps in args.budgets:
            path = args.output / f"checkpoint-{current.steps:06d}.json"
            write(path, current.checkpoint())
            checkpoints[current.steps] = path

    save_checkpoint(policy)
    budget = max(args.budgets)
    started = time.perf_counter()
    with (args.output / "training.jsonl").open("x", encoding="utf-8", buffering=1) as training:
        index = 0
        while policy.steps < budget:
            case = cases["train"][index % len(cases["train"])]
            yaw = math.remainder(case.yaw + rng.uniform(-2.2, 2.2), 2 * math.pi)
            epsilon = .30 - .24 * policy.steps / budget
            result = episode(executable, case, yaw, policy, epsilon=epsilon, train=True,
                             limit=budget - policy.steps, save_checkpoint=save_checkpoint)
            training.write(json_text({"episode": index, "trainTransitions": policy.steps, **result}) + "\n")
            index += 1
            print(f"Episode {index} {case.name} {result['outcome']} {policy.steps} transitions", flush=True)
    training_seconds = time.perf_counter() - started
    require(set(checkpoints) == set(args.budgets), "Training failed to produce the declared sample checkpoints")
    results = []
    for transitions, path in sorted(checkpoints.items()):
        frozen = TileQ.from_checkpoint(json.loads(path.read_bytes()))
        for case in cases["test"]:
            trace_name = f"checkpoint-{transitions:06d}-{case.name}-trace.jsonl"
            with (args.output / trace_name).open("x", encoding="utf-8") as trace:
                result = episode(executable, case, case.yaw, frozen, trace=trace)
            results.append({**result, "trainTransitions": transitions, "trace": trace_name,
                            "sourceNamespace": case.manifest["sourceNamespace"],
                            "imageSha256": case.manifest["imageSha256"],
                            "sceneSource": case.manifest["sceneSource"], "openDoorIds": case.manifest["openDoorIds"]})
    write(args.output / "evaluation.json", {"trainingTransitions": policy.steps, "trainingWallSeconds": training_seconds,
                                            "seed": args.seed, "budgets": args.budgets, "episodes": results})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("simulator", "cases", "output"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=287)
    parser.add_argument("--budgets", type=int, nargs="+", default=[0, 3000, 10000, 24000])
    args = parser.parse_args()
    require(args.budgets == sorted(set(args.budgets)) and args.budgets[0] >= 0,
            "List checkpoint budgets as distinct nonnegative transition counts in increasing order")
    run(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
