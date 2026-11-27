"""Exercise the persistent native BIM navigation protocol without Python SDKs."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def exchange(executable, scene, requests):
    def encode(request):
        return request if isinstance(request, str) else json.dumps(request, allow_nan=False)

    request_text = "".join(encode(request) + "\n" for request in requests)
    result = subprocess.run(
        [str(executable), str(scene)], input=request_text, capture_output=True,
        encoding="utf-8", timeout=45, check=False,
    )
    require(result.returncode == 0, f"{scene.name}: {result.stderr}")
    replies = [json.loads(line) for line in result.stdout.splitlines()]
    require(len(replies) == len(requests), f"{scene.name}: reply count {len(replies)}")
    require(all(isinstance(reply.get("ok"), bool) for reply in replies), replies)
    require(replies[-1]["ok"], replies[-1])
    return replies


def finite_array(values, length, lower=-math.inf, upper=math.inf):
    return (
        isinstance(values, list) and len(values) == length
        and all(type(value) in (int, float) and math.isfinite(value) and lower <= value <= upper
                for value in values)
    )


def verify_state(state, hello, tick):
    require(state["tick"] == tick, state)
    require(finite_array(state["pose"], 4) and finite_array(state["odometry"], 4), state)
    require(finite_array(state["goal"], 3), state)
    require(finite_array(state["ranges"], hello["rayCount"], 0, hello["rangeMeters"]), state)
    require(finite_array(state["intensity"], hello["rayCount"], 0, 1), state)
    require(isinstance(state["collision"], bool) and isinstance(state["obstruction"], str), state)
    require(isinstance(state["blockingEntityId"], str), state)
    require(math.isfinite(state["travelledMeters"]) and state["travelledMeters"] >= 0, state)


def verify_episode(executable, fixture):
    reset = {"op": "reset", "seed": 71825}
    action = {"op": "step", "action": [0.65, 0.1]}
    requests = [
        {"op": "describe"},
        action,
        reset,
        "not valid JSON",
        {"op": "step", "action": [1.1, 0]},
        {"op": "reset", "seed": -1},
        action,
        action,
        reset,
        action,
        action,
        {"op": "reset", "seed": 71826},
        "x" * 5000,
        action,
        {"op": "close"},
    ]
    replies = exchange(executable, fixture, requests)
    hello = replies[0]
    require(hello["ok"] and hello["version"] == 1, hello)
    require(hello["rayCount"] == 36 and hello["ticksPerStep"] == 6, hello)
    require(hello["ticksPerSecond"] == 60 and hello["rangeMeters"] == 6, hello)
    require(hello["linearLimit"] == 0.8 and hello["angularLimit"] == 2, hello)
    for index in (1, 3, 4, 5, 12):
        require(not replies[index]["ok"] and replies[index]["error"], replies[index])
    for index, tick in ((2, 0), (6, 6), (7, 12), (8, 0), (9, 6), (10, 12), (11, 0), (13, 6)):
        require(replies[index]["ok"], replies[index])
        verify_state(replies[index]["state"], hello, tick)
    require(replies[2] == replies[8], "Reset did not restore the same episode")
    require(replies[6:8] == replies[9:11], "Identical actions did not replay exactly")
    require(replies[7]["state"]["travelledMeters"] > 0.0, "No committed robot motion")
    first, second = replies[2], replies[11]
    require(first["manifest"] != second["manifest"], "Different episode seeds have identical manifests")
    require(first["state"]["pose"] != second["state"]["pose"] or
            first["state"]["goal"] != second["state"]["goal"], "Different seeds changed neither start nor goal")
    require(first["state"]["ranges"] != second["state"]["ranges"], "The scan did not change")
    require(first["state"]["intensity"] != second["state"]["intensity"], "The optical scan did not change")
    if first["manifest"]["furniture"]:
        positions = [episode["manifest"]["furniture"][0]["position"] for episode in (first, second)]
        require(positions[0] != positions[1], "Seeded furniture was placed at the same position")
    require(first["manifest"]["rangeNoiseMeters"] != second["manifest"]["rangeNoiseMeters"],
            "Sensor noise did not vary with the seed")
    return hello["scene"]


def doorway_wall_actions():
    # The IFC has an open doorway at y=0 and a solid partition at y=0.9.
    # Shift sideways clear of the doorway, then drive into the partition.
    return [[0, 1]] * 8 + [[1, 0]] * 12 + [[0, -1]] * 8 + [[1, 0]] * 24


def verify_collision(executable, doorway):
    replies = exchange(
        executable, doorway,
        [{"op": "reset", "seed": 0, "randomize": False}]
        + [{"op": "step", "action": action} for action in doorway_wall_actions()] + [{"op": "close"}],
    )
    require(all(reply["ok"] for reply in replies), replies)
    states = [reply["state"] for reply in replies[1:-1]]
    hits = [state for state in states if state["collision"]]
    require(hits and all(hit["obstruction"] == "blocked" and hit["blockingEntityId"].endswith("wall-guid")
                         for hit in hits), hits)
    require(states[-1]["pose"][0] < -0.25 and states[-1]["travelledMeters"] > 1.2,
            "The robot failed to reach the wall or crossed the partition")


def verify_lighting(executable, doorway):
    reference = json.loads(doorway.read_text(encoding="utf-8"))
    reference["source"] = str((doorway.parent / reference["source"]).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="bim-navigation-lighting-") as temporary:
        states = []
        for label, ambient in (("dark", 0.10), ("bright", 0.90)):
            fixture = Path(temporary) / f"{label}.json"
            recipe = {**reference, "ambientLight": [ambient, ambient], "directionalLight": [0, 0]}
            fixture.write_text(json.dumps(recipe), encoding="utf-8")
            reset, _ = exchange(executable, fixture, [
                {"op": "reset", "seed": 0, "randomize": False}, {"op": "close"},
            ])
            require(reset["ok"], reset)
            states.append(reset["state"])
        dark, bright = states
        require(dark["pose"] == bright["pose"] and dark["ranges"] == bright["ranges"],
                "Changing illumination changed the physical scenario")
        require(any(b - a > 0.5 for a, b in zip(dark["intensity"], bright["intensity"])),
                "Changing illumination had no optical effect")


def verify_encoder_noise_limit(executable, doorway):
    recipe = json.loads(doorway.read_text(encoding="utf-8"))
    recipe["source"] = str((doorway.parent / recipe["source"]).resolve(strict=True))
    recipe["encoderNoiseRadians"] = [1.1, 1.2]
    with tempfile.TemporaryDirectory(prefix="bim-navigation-encoder-") as temporary:
        fixture = Path(temporary) / "invalid-encoder-noise.json"
        fixture.write_text(json.dumps(recipe), encoding="utf-8")
        result = subprocess.run(
            [str(executable), str(fixture)], input='{"op":"describe"}\n{"op":"close"}\n',
            capture_output=True, encoding="utf-8", timeout=45, check=False,
        )
    require(result.returncode == 1 and not result.stdout and "encoderNoiseRadians" in result.stderr,
            f"Invalid encoder noise was accepted: {result.stdout} {result.stderr}")


def unit_variant_recipes(directory, doorway):
    reference = json.loads(doorway.read_text(encoding="utf-8"))
    models = ("room-connectivity.ifc", "room-connectivity-metres.ifc")
    fixtures = []
    for index, model in enumerate(models):
        name = f"unit-variant-{index}"
        fixture = directory / f"{name}.json"
        recipe = {
            **reference, "source": str((doorway.parent.parent / model).resolve(strict=True)),
            "id": name, "family": name, "sourceNamespace": name, "openDoorIds": [], "furniture": [],
            "split": "held-out" if index else "train",
        }
        fixture.write_text(json.dumps(recipe), encoding="utf-8")
        fixtures.append(fixture)
    return fixtures


def verify_unit_variant_identity(executable, doorway):
    with tempfile.TemporaryDirectory(prefix="bim-navigation-units-") as temporary:
        identities = []
        for fixture in unit_variant_recipes(Path(temporary), doorway):
            description, _ = exchange(executable, fixture, [{"op": "describe"}, {"op": "close"}])
            require(description["ok"], description)
            identities.append(description["scene"])
        first, second = identities
        require(first["sourceSha256"] != second["sourceSha256"], "Unit evidence used the same input bytes")
        require(first["geometryFingerprint"] == second["geometryFingerprint"],
                "Equivalent BIM geometry got different digests after a unit conversion")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    names = {"doorway-train.json": "train", "corner-eval.json": "held-out", "offset-hall-eval.json": "held-out"}
    identities = {}
    for name, split in names.items():
        scene = verify_episode(executable, args.fixtures / name)
        require(scene["split"] == split, scene)
        require(all(isinstance(scene[key], str) and scene[key]
                    for key in ("id", "family", "sourceSha256", "geometryFingerprint")), scene)
        identities[name] = scene
        print(f"{scene['id']} ({split}) passed deterministic reset, step, and observation checks")
    training = identities["doorway-train.json"]
    for name, identity in identities.items():
        if names[name] == "held-out":
            require(all(training[key] != identity[key] for key in ("id", "family", "sourceSha256", "geometryFingerprint")),
                    f"Training scene identity overlaps {name}")
    doorway = args.fixtures / "doorway-train.json"
    verify_collision(executable, doorway)
    verify_lighting(executable, doorway)
    verify_encoder_noise_limit(executable, doorway)
    verify_unit_variant_identity(executable, doorway)
    print("Wall contact, optical lighting, and unit-independent scene identity passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
