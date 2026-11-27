"""Check the real Gymnasium adapter with the headless renderer-independent simulator."""

import argparse
from pathlib import Path
import sys
import tempfile

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
    print("Gymnasium check, episode outcomes, recipe identity, held-out leakage, and reproducible evaluation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
