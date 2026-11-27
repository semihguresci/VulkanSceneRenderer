"""Measure one worker width for the headless BIM navigation evaluator."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import sys

from benchmark_bim_import import file_identity
from bim_robotics_benchmark_memory import ProcessMemory, host_identity


def episode_digest(scenes):
    digest = hashlib.sha256()
    for scene in sorted(scenes, key=lambda row: row["scene"]["id"]):
        for episode in scene["episodes"]:
            encoded = json.dumps(episode, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
            digest.update(encoded.encode("utf-8"))
            digest.update(b"\n")
    return digest.hexdigest()


def native_resident_upper_bound(scenes):
    totals = []
    for scene in scenes:
        peaks = [(worker["memory"] or {}).get("peakResidentBytes") for worker in scene["profile"]["workers"]]
        if not peaks or None in peaks:
            return None
        totals.append(sum(peaks))
    # Scenes run sequentially. Per-process peaks within a scene can occur at
    # different times, and resident shared pages can be counted more than once.
    return max(totals)


def benchmark_report(evaluation, executable, workers, coordinator_memory, gymnasium_version):
    scenes = evaluation["scenes"]
    splits = evaluation["splits"]
    totals = {key: sum(metrics[key] for metrics in splits.values())
              for key in ("episodes", "steps", "successes", "collisions", "truncated")}
    timing = evaluation["profile"]
    seconds = timing["wallSeconds"]
    return {
        "schemaVersion": 1,
        "configuration": {
            "executable": executable,
            "workersRequested": workers,
            "train": [row["path"] for row in scenes if row["scene"]["split"] == "train"],
            "heldOut": [row["path"] for row in scenes if row["scene"]["split"] == "held-out"],
            **{key: evaluation[key] for key in ("policy", "rewardVersion", "seed", "episodeSeedVersion",
                                              "episodesPerScene", "maxEpisodeSteps", "randomize")},
        },
        "host": {**host_identity(), "pythonVersion": platform.python_version(), "gymnasiumVersion": gymnasium_version},
        "summary": {
            **totals,
            "wallSeconds": seconds,
            "episodesPerSecond": totals["episodes"] / seconds if seconds > 0 else 0.0,
            "workerCount": max(row["profile"]["workerCount"] for row in scenes),
            "splitPreflightSeconds": timing["splitPreflightSeconds"],
        },
        "memory": {
            "coordinator": coordinator_memory,
            "nativePeakResidentBytesUpperBound": native_resident_upper_bound(scenes),
        },
        "episodeDigest": {"format": "sha256-scene-ordered-canonical-episode-jsonl-v1", "sha256": episode_digest(scenes)},
        "splits": splits,
        "scenes": [
            {"path": row["path"], "scene": row["scene"], "metrics": row["metrics"], "profile": row["profile"]}
            for row in scenes
        ],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True, help="Native BimRoboticsNavigation executable")
    parser.add_argument("--train", type=Path, action="append", required=True, help="Train scene recipe, repeatable")
    parser.add_argument("--held-out", type=Path, action="append", required=True, help="Held-out scene recipe, repeatable")
    parser.add_argument("--workers", type=int, required=True, help="Requested concurrent native simulators")
    parser.add_argument("--episodes", type=int, default=20, help="Episodes per scene (default: 20)")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--max-episode-steps", type=int, default=200)
    parser.add_argument("--no-randomize", action="store_true", help="Use the authored nominal layouts")
    parser.add_argument("--output", type=Path, required=True, help="Destination for the benchmark JSON")
    args = parser.parse_args(argv)
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
    try:
        from gymnasium import __version__ as gymnasium_version
        from bim_navigation.evaluate import _write_report, evaluate
    except ModuleNotFoundError as error:
        if error.name != "gymnasium":
            raise
        parser.exit(1, "Install the optional Gymnasium dependency to benchmark BIM navigation.\n")

    try:
        executable = args.executable.resolve(strict=True)
        executable_identity = file_identity(executable)
        with ProcessMemory(os.getpid()) as coordinator:
            evaluation = evaluate(
                executable, args.train, args.held_out, episodes=args.episodes, seed=args.seed,
                max_episode_steps=args.max_episode_steps, randomize=not args.no_randomize,
                workers=args.workers, profile=True, _worker_memory=ProcessMemory,
            )
            coordinator.sample()
            coordinator_memory = coordinator.metrics()
        report = benchmark_report(evaluation, executable_identity, args.workers, coordinator_memory, gymnasium_version)
        _write_report(args.output, report)
    except (OSError, RuntimeError, ValueError) as error:
        parser.exit(1, f"{error}\n")

    summary = report["summary"]
    print(f"{summary['workerCount']} workers, {summary['episodes']} episodes, "
          f"{summary['episodesPerSecond']:.2f} episodes/s")
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
