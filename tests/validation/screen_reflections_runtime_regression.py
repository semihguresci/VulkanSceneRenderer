"""Compare a known reflected target against the deferred raster IBL fallback."""

import argparse
import json
import math
import os
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset
from gltf_extended_materials_runtime_regression import validation_log_error


WIDTH, HEIGHT = 480, 360
EYE = np.array([0., 2.8, 5.])
TARGET = np.array([0., .65, 0.])
CARD_CENTER = np.array([.45, 1.05, -1.1])
FOV = 50.


def fixture(path, roughness, card_color=(1, .015, .008, 1), *, card_center=CARD_CENTER,
            card_size=(.63, .55), lit_card=False, blocker=None):
    asset = Asset()

    def quad(positions, normal, material):
        return {"attributes": {"POSITION": asset.accessor("VEC3", positions),
                               "NORMAL": asset.accessor("VEC3", [normal] * 4)},
                "indices": asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True),
                "material": material}

    def panel(center, half_size, material):
        x, y, z = center
        w, h = half_size
        return quad([[x - w, y - h, z], [x + w, y - h, z],
                     [x + w, y + h, z], [x - w, y + h, z]], [0, 0, 1], material)

    floor = quad([[-4, 0, 3], [4, 0, 3], [4, 0, -5], [-4, 0, -5]], [0, 1, 0], 0)
    card = panel(card_center, card_size, 1)
    asset.model.update(
        extensionsUsed=["KHR_materials_unlit"],
        meshes=[{"primitives": [floor]}, {"primitives": [card]}],
        nodes=[{"mesh": 0, "name": "Mirror"}, {"mesh": 1, "name": "Red card"}],
        scenes=[{"nodes": [0, 1]}],
        materials=[{"pbrMetallicRoughness": {"baseColorFactor": [.95, .95, .95, 1],
                                             "metallicFactor": 1, "roughnessFactor": roughness}},
                   {"pbrMetallicRoughness": {"baseColorFactor": list(card_color),
                                             "metallicFactor": 0, "roughnessFactor": 1},
                    "extensions": {"KHR_materials_unlit": {}}}])
    if lit_card:
        del asset.model["materials"][1]["extensions"]
    if blocker:
        center, half_size, alpha = blocker
        asset.model["meshes"].append({"primitives": [panel(center, half_size, 2)]})
        asset.model["nodes"].append({"mesh": 2, "name": "Black blocker"})
        asset.model["scenes"][0]["nodes"].append(2)
        material = {"pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, alpha]},
                    "doubleSided": True, "extensions": {"KHR_materials_unlit": {}}}
        if alpha < 1:
            material["alphaMode"] = "MASK"
            material["alphaCutoff"] = .5
        asset.model["materials"].append(material)
    asset.write(path)


def project(world, target=TARGET):
    forward = target - EYE
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, [0, 1, 0])
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    relative = world - EYE
    pixels_per_unit = HEIGHT * .5 / (math.tan(math.radians(FOV) * .5) * (forward @ relative))
    return int(WIDTH * .5 + (right @ relative) * pixels_per_unit), int(HEIGHT * .5 - (up @ relative) * pixels_per_unit)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--ray-fallback", action="store_true", help="Also verify off-screen ray hits on a ray-query device")
    parser.add_argument("--temporal", action="store_true", help="Also verify reflection history with motion and cuts")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    fixture(output / "mirror.gltf", .045)
    fixture(output / "rough.gltf", .85)
    fixture(output / "black-card.gltf", .045, card_color=(0, 0, 0, 1))
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))
    evidence = {}

    def capture(label, mode, *, model="mirror.gltf", forward=False, no_query=False, environment=0,
                hybrid=False, target=TARGET, directional=0, sequence=None, taa=False):
        image = output / f"{label}.png"
        command = [str(renderer), "--hidden", "--no-ui", "--validation", "--import-scale", "1",
                   "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1", "--taa" if taa else "--no-taa",
                   "--no-bloom", "--no-ddgi", "--ray-shadows", "raster", "--sky", "plain",
                   "--sky-horizon", "1", "1", "1", "--display-mode", "lit",
                   "--environment-intensity", str(environment), "--directional-intensity", str(directional),
                   "--directional-direction", "0", "0", "-1", "--exposure", "1",
                   "--camera-position", *map(str, EYE), "--camera-target", *map(str, target),
                   "--camera-fov", str(FOV), "--render-technique", "forward-raster" if forward else "deferred-raster",
                   "--screen-reflections", mode,
                   "--screenshot-viewport", "--screenshot", str(image), "--model",
                   str(output / model)]
        if sequence is None:
            command.extend(("--warmup-frames", "4", "--capture-frame", "5"))
        else:
            plan = output / f"{label}.sequence.json"
            plan.write_text(json.dumps(sequence), encoding="utf-8")
            command.extend(("--capture-sequence", str(plan), "--fixed-dt", ".016666667"))
        if no_query:
            command.append("--no-ray-query")
        if hybrid:
            command.append("--screen-reflections-ray-query")
        run = subprocess.run(command, cwd=renderer.parent, env=env, capture_output=True,
                             text=True, errors="replace", timeout=240)
        log = run.stdout + run.stderr
        (output / f"{label}.log").write_text(log, encoding="utf-8")
        assert run.returncode == 0 and not validation_log_error(log), log
        if sequence is not None:
            images, routes = {}, {}
            for frame in sequence["sampleFrames"]:
                path = image if frame == sequence["frames"] else output / f"{label}.frame-{frame:04d}.png"
                receipt = json.loads(path.with_suffix(".telemetry.json").read_text(encoding="utf-8"))
                route = receipt["screenReflections"]
                pass_state = next((p for p in receipt["passes"] if p["name"] == "ScreenReflections"), None)
                routes[frame] = {"route": route, "pass": pass_state}
                with Image.open(path) as result:
                    images[frame] = np.array(result.convert("RGB"))
            evidence[label] = routes
            return images, routes
        receipt = json.loads(image.with_suffix(".telemetry.json").read_text(encoding="utf-8"))
        active = mode != "off" and not forward
        pass_state = next((entry for entry in receipt["passes"] if entry["name"] == "ScreenReflections"), None)
        assert bool(pass_state and pass_state["active"]) == active, (label, pass_state)
        if active:
            assert pass_state["gpuTimed"] and pass_state["gpuMs"] > 0, pass_state
            assert receipt["screenReflections"]["allocationBytes"] >= WIDTH * HEIGHT * 8, receipt["screenReflections"]
        if hybrid:
            route = receipt["screenReflections"]
            assert route["rayFallbackRequested"]
            assert route["rayFallbackAvailable"] == (not no_query), route
            assert route["rayFallbackActive"] == (not no_query and active), route
        evidence[label] = {"pass": pass_state, "resources": receipt.get("screenReflections")}
        with Image.open(image) as result:
            return np.array(result.convert("RGB"))

    base = capture("mirror-off", "off")
    mirror = capture("mirror-high", "high")
    raster_only = capture("mirror-no-query", "high", no_query=True)
    confidence = capture("mirror-confidence", "confidence")
    distance = capture("mirror-distance", "distance")
    rough_off = capture("rough-off", "off", model="rough.gltf")
    rough_on = capture("rough-high", "high", model="rough.gltf")
    forward_off = capture("forward-off", "off", forward=True)
    forward_on = capture("forward-high", "high", forward=True)
    ibl = capture("ibl-off", "off", model="black-card.gltf", environment=1)
    occluded_ibl = capture("ibl-high", "high", model="black-card.gltf", environment=1)

    reflected = CARD_CENTER * [1, -1, 1]
    x, y = project(reflected)
    roi = np.s_[y - 8:y + 9, x - 12:x + 13]
    assert np.array_equal(raster_only, mirror)
    assert np.array_equal(rough_off, rough_on)
    assert np.array_equal(forward_off, forward_on)
    assert mirror[roi][..., 0].mean() > base[roi][..., 0].mean() + 60, (x, y, mirror[roi].mean(axis=(0, 1)))
    assert mirror[roi][..., 0].mean() > mirror[roi][..., 1].mean() + 60
    assert confidence[roi][..., 0].mean() > 65 and distance[roi][..., 0].max() > 0
    assert ibl[roi].mean() > occluded_ibl[roi].mean() + 15, (ibl[roi].mean(), occluded_ibl[roi].mean())
    assert np.array_equal(ibl[:32], occluded_ibl[:32])
    results = {"passed": True, "expectedPixel": [x, y],
        "roiMeanRGB": mirror[roi].mean(axis=(0, 1)).tolist(),
        "iblRoiMeans": [ibl[roi].mean(), occluded_ibl[roi].mean()], "captures": evidence}

    if args.ray_fallback:
        center = np.array([.35, 2.8, -1.5])
        target = np.array([0., -.5, 0.])
        card_size = (.45, .3)
        virtual = center * [1, -1, 1]
        mirror_point = EYE + (virtual - EYE) * (EYE[1] / (EYE[1] + center[1]))
        occluder = mirror_point + (center - mirror_point) * .86
        shadow = center + [0., 0., .7]
        for point, size in ((center, card_size), (occluder, (.50, .34)), (shadow, (.26, .19))):
            assert all(project(point + [dx, dy, 0.], target)[1] < 0
                       for dx in (-size[0], size[0]) for dy in (-size[1], size[1])), point
        fixtures = {
            "offscreen": {},
            "offscreen-blocked": {"blocker": (occluder, (.50, .34), 1)},
            "offscreen-mask": {"blocker": (occluder, (.50, .34), 0)},
            "offscreen-lit": {"lit_card": True},
            "offscreen-shadow": {"lit_card": True, "blocker": (shadow, (.26, .19), 1)},
        }
        for name, options in fixtures.items():
            fixture(output / f"{name}.gltf", .045, card_center=center, card_size=card_size, **options)
        screen = capture("offscreen-screen", "high", model="offscreen.gltf", target=target)
        hybrid = capture("offscreen-hybrid", "high", model="offscreen.gltf", target=target, hybrid=True)
        source = capture("offscreen-source", "source", model="offscreen.gltf", target=target, hybrid=True)
        unsupported = capture("offscreen-unsupported", "high", model="offscreen.gltf", target=target,
                              hybrid=True, no_query=True)
        blocked = capture("offscreen-nearest-blocker", "high", model="offscreen-blocked.gltf", target=target, hybrid=True)
        cutout = capture("offscreen-alpha-mask", "high", model="offscreen-mask.gltf", target=target, hybrid=True)
        lit = capture("offscreen-lit-card", "high", model="offscreen-lit.gltf", target=target, hybrid=True, directional=3)
        shaded = capture("offscreen-shadowed-card", "high", model="offscreen-shadow.gltf", target=target,
                         hybrid=True, directional=3)
        x, y = project(virtual, target)
        region = np.s_[y - 4:y + 5, x - 5:x + 6]
        red = lambda capture: float(capture[region][..., 0].mean())
        assert np.array_equal(screen, unsupported)
        assert red(hybrid) > red(screen) + 70 and source[region][..., 2].mean() > 90
        assert source[region][..., 1].mean() < 20
        assert red(hybrid) > red(blocked) + 70
        np.testing.assert_array_equal(hybrid[region], cutout[region])
        assert red(lit) > red(shaded) + 30, (red(lit), red(shaded))
        results["rayFallback"] = {"expectedPixel": [x, y], "screenRed": red(screen), "rayRed": red(hybrid),
            "nearestBlockerRed": red(blocked), "alphaCutoutRed": red(cutout), "litRed": red(lit),
            "shadowedRed": red(shaded), "unsupportedPixelExact": True}

    if args.temporal:
        # A thin target moves clear of the analytically projected reflector.
        # Camera motion is small enough to reproject the stationary receiver.
        thin = (.18, .44)
        fixture(output / "thin-card.gltf", .045, card_size=thin)
        moving = {"schemaVersion": 1, "frames": 8, "sampleFrames": [1, 3, 4, 5, 6, 8],
                  "objectName": "Red card",
                  "objectTranslation": [{"frame": 1, "translation": [0, 0, 0]},
                                        {"frame": 4, "translation": [0, 0, 0]},
                                        {"frame": 5, "translation": [2.5, 0, 0]}],
                  "camera": [{"frame": 1, "position": EYE.tolist(), "target": TARGET.tolist()},
                             {"frame": 8, "position": (EYE + [.035, 0, 0]).tolist(), "target": TARGET.tolist()}]}
        filtered, motion = capture("moving-thin-card", "high", model="thin-card.gltf", sequence=moving)
        debug, history = capture("moving-thin-history", "history", model="thin-card.gltf", sequence=moving)
        x, y = project(reflected)
        center = np.s_[y - 4:y + 5, x - 4:x + 5]
        red = {frame: float(image[center][..., 0].mean()) for frame, image in filtered.items()}
        accepted = {frame: float(image[center][..., 0].mean()) for frame, image in debug.items()}
        assert motion[3]["route"]["historyReprojected"] and history[3]["route"]["historyReprojected"], history
        assert accepted[1] == 0 and accepted[3] > 150 and accepted[5] < 5, accepted
        assert red[3] > red[6] + 65 and abs(red[8] - red[6]) < 5, red
        taa_images, _ = capture("moving-thin-card-taa", "high", model="thin-card.gltf", sequence=moving, taa=True)
        taa_red = {frame: float(image[center][..., 0].mean()) for frame, image in taa_images.items()}
        assert taa_red[3] > taa_red[8] + 65 and abs(taa_red[8] - red[8]) < 8, taa_red

        lifecycle = {"schemaVersion": 1, "frames": 17, "sampleFrames": [1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17],
                     "camera": [{"frame": 1, "position": EYE.tolist(), "target": TARGET.tolist()},
                                {"frame": 6, "position": EYE.tolist(), "target": TARGET.tolist()},
                                {"frame": 7, "position": (EYE + [6, 0, 0]).tolist(), "target": TARGET.tolist()}],
                     "events": [{"frame": 5, "reset": True}, {"frame": 8, "technique": "forward-raster"},
                                {"frame": 9, "technique": "deferred-raster"},
                                {"frame": 11, "materialEdit": {"sourceMaterial": 0, "values": [
                                    {"property": "pbrMetallicRoughness/roughnessFactor", "value": .22}]}},
                                {"frame": 13, "resize": [400, 300]},
                                {"frame": 16, "lightAdd": {"type": "point"}}]}
        _, routes = capture("history-lifecycle", "history", sequence=lifecycle)
        expected = {1: False, 3: True, 5: False, 6: True, 7: False, 8: False, 9: False,
                    10: True, 11: False, 12: True, 13: False, 15: True, 16: False, 17: True}
        assert {frame: state["route"]["historyReprojected"] for frame, state in routes.items()} == expected, routes
        assert not routes[8]["pass"] or not routes[8]["pass"]["active"], routes[8]

        roughness = {}
        for level in (.16, .30, .44, .58):
            label = f"roughness-{level:.2f}"
            fixture(output / f"{label}.gltf", level)
            image = capture(label, "high", model=f"{label}.gltf")
            roughness[str(level)] = float(image[roi][..., 0].mean())
        assert roughness["0.16"] > roughness["0.58"] + 70, roughness
        assert roughness["0.44"] < roughness["0.16"], roughness
        results["temporal"] = {"movingRed": red, "movingTaaRed": taa_red, "acceptedHistory": accepted,
                               "historyAfterEvents": expected, "roughnessRed": roughness}
        if args.ray_fallback:
            stationary = {"schemaVersion": 1, "frames": 3, "sampleFrames": [1, 3]}
            ray_history, routes = capture("offscreen-history", "history", model="offscreen.gltf",
                                          target=target, hybrid=True, sequence=stationary)
            accepted = {frame: float(image[region][..., 0].mean()) for frame, image in ray_history.items()}
            assert routes[3]["route"]["rayFallbackActive"] and routes[3]["route"]["historyReprojected"], routes
            assert accepted[1] == 0 and accepted[3] > 150, accepted
            results["temporal"]["rayHistory"] = accepted

    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print("PASS mirror reflection, IBL replacement, diagnostics, and requested fallback/history coverage")


if __name__ == "__main__":
    main()
