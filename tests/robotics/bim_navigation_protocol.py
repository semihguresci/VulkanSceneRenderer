"""Exercise the persistent native BIM navigation protocol without Python SDKs."""

import argparse
import hashlib
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


def doorway_recipe(scene):
    recipe = json.loads(scene.read_text(encoding="utf-8"))
    recipe["source"] = str((scene.parent / recipe["source"]).resolve(strict=True))
    return recipe


def make_fixture(directory, name, recipe):
    fixture = directory / f"{name}.json"
    fixture.write_text(json.dumps(recipe), encoding="utf-8")
    return fixture


def verify_door_manifest(reply, fixture, recipe_hash, *, expected_open=None):
    require(reply["ok"], reply)
    manifest = reply["manifest"]
    door_id = fixture["randomizedDoors"][0]["id"]
    states = manifest["doorStates"]
    require(len(states) == 1 and states[0]["id"] == door_id
            and type(states[0]["open"]) is bool, states)
    door_open = states[0]["open"]
    if expected_open is not None:
        require(door_open == expected_open, manifest)
    require(manifest["openDoorIds"] == ([door_id] if door_open else []), manifest)
    witness = fixture.get("witnessWaypoints", []) if door_open else fixture["alternateWitnessRoutes"][0]
    require(manifest["witnessWaypoints"] == witness, manifest)
    require(manifest["sceneRecipeSha256"] == recipe_hash, manifest)
    return door_open


def sampled_door_states(executable, fixture):
    recipe = doorway_recipe(fixture)
    recipe_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
    seeds = range(32)
    description, nominal, *sampled, _ = exchange(
        executable, fixture,
        [{"op": "describe"}, {"op": "reset", "seed": 0, "randomize": False}]
        + [{"op": "reset", "seed": seed} for seed in seeds] + [{"op": "close"}],
    )
    require(description["ok"], description)
    require(description["scene"]["sceneRecipeSha256"] == recipe_hash, description)
    verify_door_manifest(nominal, recipe, recipe_hash, expected_open=True)
    states = {}
    for seed, reply in zip(seeds, sampled, strict=True):
        door_open = verify_door_manifest(reply, recipe, recipe_hash)
        states.setdefault(door_open, (seed, reply))
    require(states.keys() == {False, True}, f"Did not sample both door states in {fixture.name}")
    return description["scene"], states


def verify_sampled_doors(executable, randomized, training_identity):
    description, sampled = sampled_door_states(executable, randomized)
    require(description["sourceSha256"] == training_identity["sourceSha256"], description)
    require(description["geometryFingerprint"] == training_identity["geometryFingerprint"], description)
    require(description["family"] == training_identity["family"], description)

    opened, open_reset = sampled[True]
    closed, closed_reset = sampled[False]
    action = {"op": "step", "action": [0.6, 0]}
    replies = exchange(executable, randomized, [
        {"op": "reset", "seed": opened}, action,
        {"op": "reset", "seed": closed}, action,
        {"op": "reset", "seed": opened}, action,
        {"op": "reset", "seed": closed}, action, {"op": "close"},
    ])
    require(all(reply["ok"] for reply in replies), replies)
    require(replies[0] == replies[4] == open_reset and replies[1] == replies[5],
            "The open episode changed after resetting a closed episode")
    require(replies[2] == replies[6] == closed_reset and replies[3] == replies[7],
            "The closed episode changed after resetting an open episode")


def verify_door_geometry(executable, randomized):
    recipe = doorway_recipe(randomized)
    for endpoint in ("start", "goal"):
        recipe[endpoint]["jitterMeters"] = 0
    recipe["yawJitterRadians"] = 0
    recipe["rangeNoiseMeters"] = [0, 0]
    recipe["encoderNoiseRadians"] = [0, 0]
    recipe["furniture"] = []

    with tempfile.TemporaryDirectory(prefix="bim-navigation-door-geometry-") as temporary:
        fixture = make_fixture(Path(temporary), "door-states", recipe)
        _, sampled = sampled_door_states(executable, fixture)
        opened, open_reset = sampled[True]
        closed, closed_reset = sampled[False]
        # Fix the observer and exclude sensor noise so ray zero witnesses
        # insertion and removal of the actual door leaf at the same aperture.
        open_state, closed_state = open_reset["state"], closed_reset["state"]
        require(open_state["pose"] == closed_state["pose"], (open_state, closed_state))
        require(open_state["ranges"][0] > 3.0 and 1.2 < closed_state["ranges"][0] < 1.6,
                (open_state["ranges"][0], closed_state["ranges"][0]))

        def drive(seed, actions):
            reset, *movement, _ = exchange(
                executable, fixture, [{"op": "reset", "seed": seed}]
                + [{"op": "step", "action": action} for action in actions] + [{"op": "close"}],
            )
            require(reset["ok"] and all(reply["ok"] for reply in movement), (reset, movement))
            return [reply["state"] for reply in movement]

        straight = [[1, 0]] * 38
        open_move = drive(opened, straight)
        require(open_move[-1]["pose"][0] > 1.3 and not any(state["collision"] for state in open_move),
                "The robot could not drive through the sampled open door")
        closed_move = drive(closed, straight)
        hits = [state for state in closed_move if state["collision"]]
        door = recipe["randomizedDoors"][0]["id"]
        require(closed_move[-1]["pose"][0] < -0.20 and hits
                and all(hit["obstruction"] == "blocked" and hit["blockingEntityId"] == door for hit in hits),
                "The sampled closed door did not physically block direct travel at its authored identity")

        # At 10 actions/s the following quarter turns and straight drives
        # take the capsule around the partition at y=2.48 m.
        left = [[0, math.pi / 4]] * 10
        right = [[0, -math.pi / 4]] * 10
        detour = drive(closed, left + straight[:31] + right + straight + right + straight[:31])
        require(not any(state["collision"] or state["obstruction"] != "none" for state in detour)
                and math.dist(detour[-1]["pose"][:3], closed_state["goal"]) < 0.1,
                "The wall-end detour could not reach the original goal with the door closed")


def verify_failed_door_reset(executable, randomized):
    recipe = doorway_recipe(randomized)
    recipe["randomizedDoors"][0]["openProbability"] = 0
    del recipe["alternateWitnessRoutes"]
    nominal = {"op": "reset", "seed": 0, "randomize": False}
    action = {"op": "step", "action": [0.65, 0.1]}
    with tempfile.TemporaryDirectory(prefix="bim-navigation-door-retry-") as temporary:
        fixture = make_fixture(Path(temporary), "closed-unreachable", recipe)
        replies = exchange(executable, fixture, [
            nominal, action, action, nominal, action,
            {"op": "reset", "seed": 0}, action, {"op": "close"},
        ])
        failure = replies[5]
        require(not failure["ok"] and "No collision-valid layout" in failure["error"], failure)
        require(replies[0]["ok"] and replies[1]["ok"] and replies[2]["ok"]
                and replies[:3] == [replies[3], replies[4], replies[6]],
                "Rejecting an unreachable closed layout changed the running episode")


def verify_invalid_door_recipes(executable, randomized):
    recipe = doorway_recipe(randomized)
    door = recipe["randomizedDoors"][0]
    cases = (
        ("unknown", [{**door, "id": "unknown-door", "openProbability": 0}], "unknown-door"),
        ("duplicate", [door, door], "door-guid"),
        ("probability", [{**door, "openProbability": 1.2}], "openProbability"),
    )
    with tempfile.TemporaryDirectory(prefix="bim-navigation-door-recipe-") as temporary:
        for name, doors, diagnostic in cases:
            fixture = make_fixture(Path(temporary), name, {**recipe, "randomizedDoors": doors})
            result = subprocess.run(
                [str(executable), str(fixture)], input='{"op":"close"}\n',
                capture_output=True, encoding="utf-8", timeout=45, check=False,
            )
            require(result.returncode == 1 and not result.stdout and diagnostic in result.stderr,
                    f"{name}: invalid door recipe accepted or not diagnosed: {result.stdout} {result.stderr}")


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
    randomized = args.fixtures / "doorway-randomized.json"
    verify_sampled_doors(executable, randomized, training)
    verify_door_geometry(executable, randomized)
    verify_failed_door_reset(executable, randomized)
    verify_invalid_door_recipes(executable, randomized)
    print("Seeded door variation, actual door collisions, detour and reset preservation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
