"""Check the real Gymnasium adapter with the headless renderer-independent simulator."""

import argparse
import math
from pathlib import Path
import sys
import tempfile
from threading import Barrier, Lock
from unittest.mock import patch

from bim_navigation_protocol import doorway_recipe, doorway_wall_actions, make_fixture, unit_variant_recipes


def evaluate_against_splits(evaluate, executable, fixtures, training_scene):
    held_out = [fixtures / "corner-eval.json", fixtures / "offset-hall-eval.json"]
    configuration = {"episodes": 1, "seed": 20261127, "max_episode_steps": 200}
    first = evaluate(executable, [training_scene], held_out, **configuration)
    second = evaluate(executable, [training_scene], list(reversed(held_out)), **configuration)
    first_episodes = {row["scene"]["id"]: row["episodes"] for row in first["scenes"]}
    second_episodes = {row["scene"]["id"]: row["episodes"] for row in second["scenes"]}
    assert first_episodes == second_episodes, "Changing scene order changed the benchmark episodes"
    assert first["splits"]["train"]["successes"] == 1, first["splits"]
    assert first["splits"]["held-out"]["successes"] >= 1, first["splits"]
    assert all(metrics["stepsPerSecond"] > 0 for metrics in first["splits"].values()), first["splits"]

    with tempfile.TemporaryDirectory(prefix="bim-navigation-split-") as temporary:
        train_variant, holdout_variant = unit_variant_recipes(Path(temporary), training_scene)
        try:
            evaluate(executable, [train_variant], [holdout_variant], **configuration)
        except ValueError as error:
            assert "geometryFingerprint" in str(error), error
        else:
            raise AssertionError("A unit-equivalent BIM scene leaked into the held-out split")

        randomized = doorway_recipe(fixtures / "doorway-randomized.json")
        randomized.update(id="doorway-randomized-eval", family="doorway-randomized-eval", split="held-out")
        randomized_holdout = make_fixture(Path(temporary), "randomized-held-out", randomized)
        try:
            evaluate(executable, [training_scene], [randomized_holdout], **configuration)
        except ValueError as error:
            assert "sourceSha256" in str(error), error
        else:
            raise AssertionError("A randomized recipe leaked the fixed doorway's BIM source into the held-out split")


def verify_recipe_change(executable, fixtures):
    from bim_navigation.evaluate import _run_scene, _scenes

    max_episode_steps = 200
    recipe = doorway_recipe(fixtures / "doorway-randomized.json")
    with tempfile.TemporaryDirectory(prefix="bim-navigation-recipe-change-") as temporary:
        directory = Path(temporary)
        scene_path = make_fixture(directory, "editable-doorway", recipe)
        paths = {"train": [scene_path], "held-out": [fixtures / "corner-eval.json"]}
        (path, scene), _ = _scenes(executable, paths, max_episode_steps)
        recipe["randomizedDoors"][0]["openProbability"] = 0
        make_fixture(directory, "editable-doorway", recipe)
        try:
            _run_scene(executable, path, scene, [81], randomize=True, max_episode_steps=max_episode_steps)
        except ValueError as error:
            assert "Scene identity changed after validating splits" in str(error), error
        else:
            raise AssertionError("Changing a door probability after split validation went undetected")


def verify_parallel_results(evaluate, executable, train, held_out):
    settings = {"episodes": 5, "seed": 497, "max_episode_steps": 8}
    serial = evaluate(executable, [train], [held_out], **settings)
    assert serial["workers"] == 1, serial["workers"]
    timing_keys = {"wallSeconds", "episodesPerSecond", "stepsPerSecond", "simulatedSecondsPerSecond"}

    def stable_metrics(metrics):
        return {name: value for name, value in metrics.items() if name not in timing_keys}

    for width in (2, 4):
        parallel = evaluate(executable, [train], [held_out], workers=width, **settings)
        assert parallel["workers"] == width, parallel["workers"]
        assert len(parallel["scenes"]) == len(serial["scenes"])
        for actual, expected in zip(parallel["scenes"], serial["scenes"], strict=True):
            assert (actual["path"], actual["scene"]) == (expected["path"], expected["scene"])
            assert actual["episodes"] == expected["episodes"], (width, actual["scene"]["id"])
            assert stable_metrics(actual["metrics"]) == stable_metrics(expected["metrics"])
            assert "profile" not in actual, "Default evaluation unexpectedly profiled a worker"
        for split, metrics in parallel["splits"].items():
            assert stable_metrics(metrics) == stable_metrics(serial["splits"][split])
            assert metrics["episodesPerSecond"] > 0, metrics

    for invalid in (0, -1, True, 1.5, "2"):
        try:
            evaluate(executable, [train], [held_out], workers=invalid, **settings)
        except ValueError as error:
            assert "workers" in str(error), error
        else:
            raise AssertionError(f"Invalid worker width accepted: {invalid!r}")


def verify_worker_lifecycle(executable, train, held_out, *, episodes, width, inject_failure=False):
    import bim_navigation.evaluate as evaluation

    environment_type = evaluation.BimNavigationEnv
    requested_paths = (train.resolve(), held_out.resolve())
    active = min(episodes, width)
    lock = Lock()
    launched = []
    overlapped = set()
    leaders = {}
    failure_message = "test worker failure after overlapping native steps"

    def check_overlap(path):
        with lock:
            ready = [entry for entry in launched if entry["path"] == path and entry["steps"] == 1]
            assert len({entry["child"].pid for entry in ready}) == active, ready
            assert all(entry["child"].poll() is None for entry in ready), ready
            overlapped.add(path)

    barriers = {path: Barrier(active, action=lambda path=path: check_overlap(path), timeout=15)
                for path in requested_paths}

    class TracedEnvironment(environment_type):
        def __init__(self, executable, scene, **settings):
            super().__init__(executable, scene, **settings)
            self.trace = {"path": Path(scene).resolve(), "child": self._native._process,
                          "seeds": [], "steps": 0, "memory": None}
            assert self.trace["child"].pid == self.simulator_pid
            with lock:
                launched.append(self.trace)

        def reset(self, *, seed=None, options=None):
            observation = super().reset(seed=seed, options=options)
            self.trace["seeds"].append(seed)
            return observation

        def step(self, action):
            result = super().step(action)
            with lock:
                self.trace["steps"] += 1
                first_step = self.trace["steps"] == 1
                path = self.trace["path"]
                pid = self.trace["child"].pid
                leader = leaders.setdefault(path, pid)
            if first_step:
                barriers[path].wait()
                if inject_failure and path == requested_paths[0] and pid == leader:
                    raise RuntimeError(failure_message)
            return result

    class TracedMemory:
        def __init__(self, pid):
            self.pid = pid
            self.samples = 0
            self.closed = False
            with lock:
                owner = next(entry for entry in reversed(launched) if entry["child"].pid == pid)
                assert owner["memory"] is None and owner["child"].poll() is None, pid
                owner["memory"] = self

        def __enter__(self):
            return self

        def __exit__(self, *_):
            self.closed = True

        def sample(self):
            self.samples += 1

        def metrics(self):
            return {"source": "test monitor", "peakResidentBytes": 1_000_000 + self.pid, "peakCommitBytes": None}

    report = None
    try:
        with patch.object(evaluation, "BimNavigationEnv", TracedEnvironment):
            try:
                report = evaluation.evaluate(executable, [train], [held_out], episodes=episodes, seed=91,
                                             workers=width, max_episode_steps=6, profile=True,
                                             _worker_memory=TracedMemory)
            except RuntimeError as error:
                assert inject_failure and failure_message in str(error), error
            else:
                assert not inject_failure, "Injected worker failure was suppressed"
    finally:
        assert all(entry["child"].poll() is not None for entry in launched), "A native worker was left running"
        memories = [entry["memory"] for entry in launched if entry["memory"] is not None]
        assert all(memory.closed for memory in memories), "A native memory monitor was left open"

    if inject_failure:
        assert requested_paths[0] in overlapped, overlapped
        train_workers = [entry for entry in launched if entry["path"] == requested_paths[0] and entry["steps"]]
        assert len(train_workers) == active, train_workers
        return

    assert report["workers"] == width and overlapped == set(requested_paths)
    for row in report["scenes"]:
        path = Path(row["path"])
        workers = [entry for entry in launched if entry["path"] == path and entry["seeds"]]
        profile = row["profile"]
        assert len(workers) == len(profile["workers"]) == profile["workerCount"] == active, profile
        assert profile["wallSeconds"] >= row["metrics"]["wallSeconds"] > 0, profile
        assert math.isclose(row["metrics"]["episodesPerSecond"], episodes / row["metrics"]["wallSeconds"])
        expected_seeds = sorted(episode["seed"] for episode in row["episodes"])
        assert sorted(seed for worker in workers for seed in worker["seeds"]) == expected_seeds
        expected_counts = {}
        for worker in workers:
            memory = worker["memory"]
            assert memory is not None and memory.samples > 0, worker
            expected_counts[memory.metrics()["peakResidentBytes"]] = (len(worker["seeds"]), worker["steps"])
        reported_counts = {}
        for worker in profile["workers"]:
            memory = worker["memory"]
            assert memory["source"] == "test monitor", worker
            reported_counts[memory["peakResidentBytes"]] = (worker["episodes"], worker["steps"])
            for key in ("startupSeconds", "resetSeconds", "policySeconds", "stepSeconds", "recordSeconds"):
                assert math.isfinite(worker[key]) and worker[key] >= 0, worker
        assert reported_counts == expected_counts, profile
        assert sum(count for count, _ in reported_counts.values()) == episodes
        assert sum(steps for _, steps in reported_counts.values()) == row["metrics"]["steps"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    args = parser.parse_args()
    try:
        from gymnasium.utils.env_checker import check_env
    except ModuleNotFoundError as error:
        if error.name != "gymnasium":
            raise
        print("Install the optional Gymnasium dependency to exercise the Python adapter")
        return 77
    import numpy as np

    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
    from bim_navigation import BimNavigationEnv, baseline_action
    from bim_navigation.evaluate import evaluate

    executable = args.executable.resolve(strict=True)
    scene = (args.fixtures / "doorway-train.json").resolve(strict=True)
    env = BimNavigationEnv(executable, scene)
    try:
        check_env(env, skip_render_check=True)
        observation, _ = env.reset(seed=419)
        for _ in range(8):
            assert env.observation_space.contains(observation), observation
            action = baseline_action(observation)
            assert env.action_space.contains(action), action
            observation, reward, terminated, truncated, _ = env.step(action)
            assert np.isfinite(reward), reward
            if terminated or truncated:
                break
    finally:
        env.close()
    env.close()

    timed_env = BimNavigationEnv(executable, scene, max_episode_steps=2, randomize=False)
    try:
        start, _ = timed_env.reset(seed=6)
        _, _, terminated, truncated, _ = timed_env.step(np.zeros(2, dtype=np.float32))
        assert not terminated and not truncated
        _, _, terminated, truncated, info = timed_env.step(np.zeros(2, dtype=np.float32))
        assert not terminated and truncated and "episode" in info, info
        record = timed_env.episode_record()
        assert isinstance(record["manifest"], dict) and record["manifest"], record
        reset, _ = timed_env.reset(seed=6)
        for key in start:
            np.testing.assert_array_equal(reset[key], start[key], err_msg=key)
    finally:
        timed_env.close()

    collision_env = BimNavigationEnv(executable, scene, randomize=False)
    try:
        collision_env.reset(seed=81)
        for action in doorway_wall_actions():
            _, _, terminated, truncated, info = collision_env.step(action)
            if terminated or truncated:
                break
        assert terminated and not truncated and info["episode"]["collision"], info
        assert info["rewardTerms"]["collision"] == -5 and info["blockingEntityId"].endswith("wall-guid"), info
    finally:
        collision_env.close()

    evaluate_against_splits(evaluate, executable, args.fixtures, scene)
    verify_recipe_change(executable, args.fixtures)
    held_out = args.fixtures / "corner-eval.json"
    verify_parallel_results(evaluate, executable, scene, held_out)
    verify_worker_lifecycle(executable, scene, held_out, episodes=6, width=2)
    verify_worker_lifecycle(executable, scene, held_out, episodes=2, width=64)
    verify_worker_lifecycle(executable, scene, held_out, episodes=2, width=64, inject_failure=True)
    print("Gymnasium check, held-out leakage, deterministic parallel evaluation, and worker cleanup passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
