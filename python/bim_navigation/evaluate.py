"""Evaluate the observation-only navigation baseline on disjoint BIM scenes."""

import argparse
import hashlib
import json
from pathlib import Path
import tempfile
from time import perf_counter

import numpy as np

from .environment import BimNavigationEnv, REWARD_VERSION, _positive_int
from .policy import BASELINE_POLICY_VERSION, baseline_action


EPISODE_SEED_VERSION = "sha256-scene-seed-v1"


def _scenes(executable, paths_by_split, max_episode_steps):
    scenes = []
    ids = set()
    holdouts = ("family", "sourceSha256", "geometryFingerprint")
    identities = {"train": {field: set() for field in holdouts}, "held-out": {field: set() for field in holdouts}}
    for split, paths in paths_by_split.items():
        if not paths:
            raise ValueError(f"Provide at least one {split} scene manifest")
        for path in paths:
            path = Path(path).resolve(strict=True)
            with BimNavigationEnv(executable, path, max_episode_steps=max_episode_steps) as environment:
                scene = environment.scene
            if scene["split"] != split:
                raise ValueError(f"Expected {split} scene at {path}, got {scene['split']}")
            if scene["id"] in ids:
                raise ValueError(f"Duplicate scene ID {scene['id']} in evaluation")
            ids.add(scene["id"])
            for field in holdouts:
                identities[split][field].add(scene[field].casefold())
            scenes.append((path, scene))
    for field in holdouts:
        shared = identities["train"][field] & identities["held-out"][field]
        if shared:
            raise ValueError(f"Train and held-out scenes share {field}: {', '.join(sorted(shared))}")
    return scenes


def _metrics(episodes, wall_seconds):
    summaries = [episode["summary"] for episode in episodes]
    total = len(summaries)
    steps = sum(summary["steps"] for summary in summaries)
    successes = sum(summary["success"] for summary in summaries)
    collisions = sum(summary["collision"] for summary in summaries)
    simulated_seconds = sum(summary["simulatedSeconds"] for summary in summaries)
    return {
        "episodes": total, "steps": steps, "successes": successes, "collisions": collisions,
        "truncated": sum(summary["truncated"] for summary in summaries),
        "successRate": successes / total, "collisionRate": collisions / total,
        "meanReturn": sum(summary["return"] for summary in summaries) / total,
        "wallSeconds": wall_seconds, "simulatedSeconds": simulated_seconds,
        "stepsPerSecond": steps / wall_seconds if wall_seconds > 0 else 0.0,
        "simulatedSecondsPerSecond": simulated_seconds / wall_seconds if wall_seconds > 0 else 0.0,
    }


def _run_scene(executable, path, scene, seeds, *, randomize, max_episode_steps):
    episodes = []
    with BimNavigationEnv(
        executable, path, randomize=randomize, max_episode_steps=max_episode_steps
    ) as environment:
        if environment.scene != scene:
            raise ValueError(f"Scene identity changed after validating splits: {path}")
        start = perf_counter()
        for seed in seeds:
            observation, _ = environment.reset(seed=seed)
            terminated = truncated = False
            while not (terminated or truncated):
                # The policy gets only the sensor and goal observation, not
                # the manifest, simulator state, scene identity or planned route.
                action = baseline_action(observation)
                observation, _, terminated, truncated, _ = environment.step(action)
            episodes.append(environment.episode_record())
        elapsed = perf_counter() - start
    return {"path": str(path), "scene": scene, "metrics": _metrics(episodes, elapsed), "episodes": episodes}


def _episode_seeds(seed, scene, count):
    identity = {field: scene[field] for field in ("split", "id", "geometryFingerprint")}
    canonical = json.dumps(identity, sort_keys=True, separators=(",", ":")).encode("utf-8")
    digest = hashlib.sha256(canonical).digest()
    # Use fixed little-endian 32-bit words for portable SeedSequence entropy.
    words = [int.from_bytes(digest[index:index + 4], "little") for index in range(0, len(digest), 4)]
    entropy = [seed & 0xFFFFFFFF, seed >> 32, *words]
    return [int(value) for value in np.random.SeedSequence(entropy).generate_state(count, dtype=np.uint64)]


def evaluate(executable, train, held_out, *, episodes=3, seed=0, max_episode_steps=200, randomize=True):
    """Evaluate the same controller on train and held-out manifests."""
    episodes = _positive_int(episodes, "episodes")
    max_episode_steps = _positive_int(max_episode_steps, "max_episode_steps")
    if isinstance(seed, (bool, np.bool_)) or not isinstance(seed, (int, np.integer)) or not 0 <= seed < 2 ** 64:
        raise ValueError("seed must be an unsigned 64-bit integer")
    if type(randomize) is not bool:
        raise ValueError("randomize must be a boolean")
    paths_by_split = {"train": tuple(train), "held-out": tuple(held_out)}
    scenes = _scenes(executable, paths_by_split, max_episode_steps)
    rows = []
    for path, scene in scenes:
        seeds = _episode_seeds(int(seed), scene, episodes)
        rows.append(_run_scene(executable, path, scene, seeds,
                               randomize=randomize, max_episode_steps=max_episode_steps))
    split_metrics = {}
    for split in paths_by_split:
        selected = [row for row in rows if row["scene"]["split"] == split]
        split_episodes = [episode for row in selected for episode in row["episodes"]]
        split_metrics[split] = _metrics(split_episodes, sum(row["metrics"]["wallSeconds"] for row in selected))
    return {"schemaVersion": 1, "policy": BASELINE_POLICY_VERSION, "rewardVersion": REWARD_VERSION,
            "seed": int(seed), "episodeSeedVersion": EPISODE_SEED_VERSION,
            "episodesPerScene": episodes, "maxEpisodeSteps": max_episode_steps,
            "randomize": randomize, "splits": split_metrics, "scenes": rows}


def _write_report(path, report):
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=path.parent, prefix=".navigation-", delete=False
    ) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(report, stream, indent=2, allow_nan=False)
            stream.write("\n")
        except BaseException:
            stream.close()
            temporary.unlink(missing_ok=True)
            raise
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True, help="Native BimRoboticsNavigation executable")
    parser.add_argument("--train", type=Path, action="append", required=True, help="Train scene manifest, repeatable")
    parser.add_argument("--held-out", type=Path, action="append", required=True,
                        help="Held-out scene manifest, repeatable")
    parser.add_argument("--episodes", type=int, default=3, help="Episodes per scene (default: 3)")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--max-episode-steps", type=int, default=200)
    parser.add_argument("--no-randomize", action="store_true", help="Use each manifest's nominal layout")
    parser.add_argument("--output", type=Path, required=True, help="Destination for the JSON report")
    args = parser.parse_args(argv)
    try:
        report = evaluate(args.executable, args.train, args.held_out, episodes=args.episodes, seed=args.seed,
                          max_episode_steps=args.max_episode_steps, randomize=not args.no_randomize)
        _write_report(args.output, report)
    except (OSError, RuntimeError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    for split, summary in report["splits"].items():
        print(f"{split}  {summary['successes']}/{summary['episodes']} successes  "
              f"{summary['collisions']} collisions  {summary['stepsPerSecond']:.1f} steps/s")
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
