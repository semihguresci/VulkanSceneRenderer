"""Opt-in OpenVDB rendering, composition, sequence and sparse-mode captures.

Requires an enabled renderer/tool, a Vulkan display/runtime and Pillow. Every
run retains generated VDB/BIM fixtures, subprocess logs, commands and PNGs in a
new directory beneath --output-dir. No external model downloads are needed.
"""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import traceback
import uuid


WIDTH, HEIGHT = 320, 200
RUNTIME_BUDGET_SECONDS = 900
VALIDATION_ERROR = re.compile(r"VUID-|Validation Error|SYNC-HAZARD", re.IGNORECASE)


def write_occluder(path: Path) -> None:
    """Opaque box in front of the generated cloud, with a known center patch."""
    x, y, z = .2, .8, .08
    points = [(-x, -y, -z), (x, -y, -z), (x, y, -z), (-x, y, -z),
              (-x, -y, z), (x, -y, z), (x, y, z), (-x, y, z)]
    fixture = {
        "schema_version": "1.1.0",
        "meshes": [{"mesh_id": 0,
                    # dotbim is Z-up; inverse of its (x,z,-y) import conversion.
                    "coordinates": [component for px, py, pz in points
                                    for component in (px, -pz, py)],
                    "indices": [0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
                                0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
                                0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5]}],
        "elements": [{"mesh_id": 0, "guid": "vdb-depth-occluder", "type": "Box",
                      "vector": {"x": 0, "y": -.85, "z": 0},
                      "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
                      "color": {"r": 90, "g": 150, "b": 200, "a": 255}}],
    }
    path.write_text(json.dumps(fixture, indent=2) + "\n", encoding="utf-8")


def write_local_light(path: Path, kind: str, intensity: float, *, away=False) -> None:
    """A minimal glTF needs geometry; keep its triangle outside the camera."""
    vertices = (50.0, 50.0, 50.0, 50.01, 50.0, 50.0, 50.0, 50.01, 50.0)
    data = struct.pack("<9f3H", *vertices, 0, 1, 2)
    light = {"type": kind, "intensity": intensity, "color": [1, .8, .6], "range": 4}
    if kind == "spot":
        light["spot"] = {"innerConeAngle": .25, "outerConeAngle": .65}
    light_node = {"translation": [0, 0, 1.2],
                  "extensions": {"KHR_lights_punctual": {"light": 0}}}
    if away:
        light_node["rotation"] = [0, 1, 0, 0]  # local -Z -> world +Z
    fixture = {
        "asset": {"version": "2.0", "generator": "Container OpenVDB regression"},
        "extensionsUsed": ["KHR_lights_punctual"],
        "extensions": {"KHR_lights_punctual": {"lights": [light]}},
        "buffers": [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," +
                     base64.b64encode(data).decode("ascii")}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
                        {"buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [50, 50, 50], "max": [50.01, 50.01, 50]},
                      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "nodes": [{"mesh": 0}, light_node], "scenes": [{"nodes": [0, 1]}], "scene": 0,
    }
    path.write_text(json.dumps(fixture, indent=2) + "\n", encoding="utf-8")


def write_area_lights(path: Path, intensity: float, *, shape="rect", away=False,
                      count=1, position=(0.0, 0.0, 1.2), rotation=None, light_range=4.0) -> None:
    """Reuse the imported-light route with a mesh outside the camera view."""
    write_local_light(path, "point", intensity)
    fixture = json.loads(path.read_text(encoding="utf-8"))
    lights, nodes = [], [fixture["nodes"][0]]
    for index in range(count):
        lights.append({
            "name": f"OpenVDB {shape} emitter {index}", "type": "point",
            "intensity": intensity, "color": [1.0, .8, .6], "range": light_range,
            "extras": {"areaLight": {"shape": shape, "width": .8, "height": .8,
                                      "radius": .4, "range": light_range}},
        })
        node = {"translation": list(position),
                "extensions": {"KHR_lights_punctual": {"light": index}}}
        if rotation is not None:
            node["rotation"] = list(rotation)
        elif away:
            node["rotation"] = [0, 1, 0, 0]  # local -Z -> world +Z
        nodes.append(node)
    fixture["extensions"]["KHR_lights_punctual"]["lights"] = lights
    fixture["nodes"] = nodes
    fixture["scenes"][0]["nodes"] = list(range(len(nodes)))
    path.write_text(json.dumps(fixture, indent=2) + "\n", encoding="utf-8")


def write_shadow_caster(path: Path) -> None:
    """An off-camera +X caster shadows fog; the front box still tests depth."""
    write_occluder(path)
    fixture = json.loads(path.read_text(encoding="utf-8"))
    x, y, z = .08, .8, .8
    points = [(-x, -y, -z), (x, -y, -z), (x, y, -z), (-x, y, -z),
              (-x, -y, z), (x, -y, z), (x, y, z), (-x, y, z)]
    fixture["meshes"].append({
        "mesh_id": 1,
        "coordinates": [component for px, py, pz in points for component in (px, -pz, py)],
        "indices": fixture["meshes"][0]["indices"],
    })
    fixture["elements"].append({
        "mesh_id": 1, "guid": "vdb-off-camera-shadow-caster", "type": "ShadowCaster",
        # At aspect 1.6 and vertical FOV45, even its far corners are offscreen.
        "vector": {"x": 3, "y": 0, "z": 0},
        "rotation": {"qx": 0, "qy": 0, "qz": 0, "qw": 1},
        "color": {"r": 100, "g": 100, "b": 100, "a": 255},
    })
    path.write_text(json.dumps(fixture, indent=2) + "\n", encoding="utf-8")


def write_transparent_planes(path: Path, layers: list[tuple[float, float, tuple]], *, reverse=False) -> None:
    """Unlit alpha planes with equal projected footprints at each depth.

    A pure absorbing medium makes front/inside/behind order observable without
    relying on an unrelated PBR or refraction approximation.
    """
    data = bytearray()
    views, accessors, materials, meshes, nodes = [], [], [], [], []
    for z, alpha, color in (reversed(layers) if reverse else layers):
        half = .9 * (3.0 - z) / 3.0  # camera is fixed at (0,0,3)
        offset = len(data)
        vertices = (-half, -half, z, half, -half, z, half, half, z, -half, half, z)
        data += struct.pack("<12f6H", *vertices, 0, 1, 2, 0, 2, 3)
        view = len(views)
        views += [{"buffer": 0, "byteOffset": offset, "byteLength": 48, "target": 34962},
                  {"buffer": 0, "byteOffset": offset + 48, "byteLength": 12, "target": 34963}]
        accessor = len(accessors)
        accessors += [{"bufferView": view, "componentType": 5126, "count": 4, "type": "VEC3",
                       "min": [-half, -half, z], "max": [half, half, z]},
                      {"bufferView": view + 1, "componentType": 5123, "count": 6, "type": "SCALAR"}]
        material = len(materials)
        materials.append({"alphaMode": "BLEND", "doubleSided": True,
                          "pbrMetallicRoughness": {"baseColorFactor": [*color, alpha],
                                                   "metallicFactor": 0, "roughnessFactor": 1},
                          "extensions": {"KHR_materials_unlit": {}}})
        meshes.append({"primitives": [{"attributes": {"POSITION": accessor}, "indices": accessor + 1,
                                        "material": material}]})
        nodes.append({"mesh": material})
    scene_nodes = list(range(len(nodes)))
    fixture = {"asset": {"version": "2.0", "generator": "Container OpenVDB transparency regression"},
               "extensionsUsed": ["KHR_materials_unlit"],
               "buffers": [{"byteLength": len(data), "uri": "data:application/octet-stream;base64," +
                            base64.b64encode(data).decode("ascii")}],
               "bufferViews": views, "accessors": accessors, "materials": materials,
               "meshes": meshes, "nodes": nodes, "scenes": [{"nodes": scene_nodes}], "scene": 0}
    path.write_text(json.dumps(fixture, indent=2) + "\n", encoding="utf-8")


def image_metrics(reference: Path, candidate: Path) -> dict:
    from PIL import Image, ImageChops, ImageStat

    with Image.open(reference) as source:
        expected = source.convert("RGB")
    with Image.open(candidate) as source:
        actual = source.convert("RGB")
    if expected.size != (WIDTH, HEIGHT) or actual.size != expected.size:
        raise AssertionError(f"unexpected capture dimensions: {expected.size}, {actual.size}")
    difference = ImageChops.difference(expected, actual)
    def pixels(image):
        getter = getattr(image, "get_flattened_data", None)
        return getter() if getter else image.getdata()
    changed = sum(max(pixel) >= 8 for pixel in pixels(difference))
    center = (WIDTH // 2 - 6, HEIGHT // 2 - 6,
              WIDTH // 2 + 6, HEIGHT // 2 + 6)
    patch_mean = sum(ImageStat.Stat(difference.crop(center)).mean) / 3.0
    cloud = (WIDTH // 2 - 70, HEIGHT // 2 - 65, WIDTH // 2 + 70, HEIGHT // 2 + 65)
    cloud_expected = ImageStat.Stat(expected.crop(cloud)).mean
    cloud_actual = ImageStat.Stat(actual.crop(cloud)).mean
    left = (WIDTH // 2 - 80, 0, WIDTH // 2 - 24, HEIGHT)
    right = (WIDTH // 2 + 24, 0, WIDTH // 2 + 80, HEIGHT)
    return {
        "changedPixels": changed,
        "changedFraction": changed / (WIDTH * HEIGHT),
        "rgbDifferenceThreshold": 8,
        "meanAbsoluteError": sum(ImageStat.Stat(difference).mean) / 3.0,
        "maxChannelAbsoluteError": max(high for low, high in difference.getextrema()),
        "cloudRegion": list(cloud),
        "cloudMeanAbsoluteError": sum(ImageStat.Stat(difference.crop(cloud)).mean) / 3.0,
        "cloudMeanLuminanceDelta": sum(
            (actual - expected) * weight for actual, expected, weight in
            zip(cloud_actual, cloud_expected, (.2126, .7152, .0722))),
        "centerPatch": list(center),
        "centerPatchMeanAbsoluteError": patch_mean,
        "leftCloudChangedPixels": sum(max(pixel) >= 8 for pixel in pixels(difference.crop(left))),
        "rightCloudChangedPixels": sum(max(pixel) >= 8 for pixel in pixels(difference.crop(right))),
    }


def reactive_metrics(path: Path) -> dict:
    """The temporal reactive diagnostic is linear grayscale, zero or one."""
    from PIL import Image, ImageStat

    with Image.open(path) as source:
        image = source.convert("RGB")
    def mean(box):
        return sum(ImageStat.Stat(image.crop(box)).mean) / 3.0
    return {
        "cornerMean": mean((0, 0, 24, 24)),
        "occludedCenterMean": mean((WIDTH // 2 - 6, HEIGHT // 2 - 6,
                                     WIDTH // 2 + 6, HEIGHT // 2 + 6)),
        "visibleCloudMean": mean((WIDTH // 2 + 28, HEIGHT // 2 - 5,
                                   WIDTH // 2 + 38, HEIGHT // 2 + 5)),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--surface-smoke", action="store_true",
                        help="also capture the generated SDF through the surface importer")
    parser.add_argument("--native-sparse", action="store_true",
                        help="also compare native NanoVDB rendering (requires NanoVDB headers in the build)")
    args = parser.parse_args()
    if os.environ.get("CONTAINER_RUN_OPENVDB_REGRESSION") != "1":
        print("SKIP: set CONTAINER_RUN_OPENVDB_REGRESSION=1")
        return 77
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print("SKIP: OpenVDB runtime image comparisons require Pillow")
        return 77

    root = args.output_dir.resolve()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = root / f"run-{stamp}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    output.mkdir(parents=True, exist_ok=False)
    exe, tool = args.exe.resolve(), args.tool.resolve()
    report = {
        "schemaVersion": 2, "passed": False, "outputDirectory": str(output),
        "nativeSparseRequested": args.native_sparse,
        "commands": [], "captures": {}, "comparisons": [], "errors": [],
        "thresholds": {"minChangedFraction": .01, "rgbDifference": 8,
                       "maxOccludedPatchMeanError": 2,
                       "minChangedPixelsPerCloudSide": int(WIDTH * HEIGHT * .001),
                       "maxEquivalentMeanError": .35, "maxEquivalentCloudMeanError": 1,
                       "maxNativeMeanError": 3, "maxNativeCloudMeanError": 6,
                       "minAreaChangedPixels": 100, "minAreaCloudLuminanceDelta": .25},
    }
    started = time.monotonic()
    deadline = started + RUNTIME_BUDGET_SECONDS
    (output / "vk_layer_settings.txt").write_text(
        "khronos_validation.validate_sync = true\n", encoding="utf-8")
    environment = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))

    def save_report() -> None:
        report["elapsedSeconds"] = round(time.monotonic() - started, 3)
        (output / "results.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8")

    def run(name: str, command: list[str], *, expected_failure=()) -> None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"OpenVDB runtime regression exceeded its {RUNTIME_BUDGET_SECONDS}-second budget")
        log = output / (name + ".log")
        record = {"name": name, "argv": command, "cwd": str(exe.parent),
                  "log": str(log), "timeoutSeconds": min(90, remaining)}
        if expected_failure:
            record["expectedFailure"] = True
            record["expectedDiagnosticFragments"] = list(expected_failure)
        report["commands"].append(record)
        save_report()
        subprocess_started = time.monotonic()
        try:
            with log.open("w", encoding="utf-8") as stream:
                process = subprocess.run(
                    command, cwd=exe.parent, env=environment,
                    stdout=stream, stderr=subprocess.STDOUT,
                    timeout=min(90, remaining),
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            record["returnCode"] = process.returncode
        except subprocess.TimeoutExpired as error:
            record["timedOut"] = True
            raise TimeoutError(f"{name} timed out; see {log}") from error
        finally:
            record["elapsedSeconds"] = round(time.monotonic() - subprocess_started, 3)
            save_report()
        messages = log.read_text(encoding="utf-8", errors="replace")
        validation_errors = sorted(set(VALIDATION_ERROR.findall(messages)))
        record["validationErrors"] = validation_errors
        if validation_errors:
            raise AssertionError(f"{name} reported Vulkan validation errors; see {log}")
        if expected_failure:
            if process.returncode == 0 or any(fragment not in messages for fragment in expected_failure):
                raise AssertionError(f"{name} did not fail with its requested frame/path diagnostic; see {log}")
        elif process.returncode != 0:
            raise AssertionError(f"{name} exited with {process.returncode}; see {log}")

    def capture(name: str, technique: str, msaa: int, *, volume=False,
                occluder=False, surface=False, events=None, taa=False,
                display="lit", extra=(), frames=4, expected_failure=()) -> Path:
        screenshot = output / (name + ".png")
        # An explicit empty string suppresses the default model. Keeping argv as
        # a list also preserves it on Windows, where shell strings often lose it.
        command = [str(exe), "--model", str(sdf) if surface else "",
                   "--hidden", "--no-ui", "--validation", "--display-mode", display,
                   "--width", str(WIDTH), "--height", str(HEIGHT),
                   "--camera-position", "0", "0", "3", "--camera-target", "0", "0", "0",
                   "--camera-fov", "45", "--exposure", "1",
                   "--directional-intensity", "2", "--environment-intensity", ".5",
                   "--no-bloom", "--taa" if taa else "--no-taa", "--no-ray-query",
                   "--fixed-dt", "0.016666667", "--warmup-frames", "3", "--capture-frame", str(frames),
                   "--render-technique", technique, "--msaa", str(msaa),
                   "--screenshot", str(screenshot)]
        if volume:
            command += ["--vdb-volume", str(fog), "--vdb-grid", "density"]
        if occluder:
            command += ["--bim-model", str(box)]
        if events:
            sequence = output / (name + ".capture.json")
            sequence.write_text(json.dumps({"schemaVersion": 1, "frames": frames,
                                            "sampleFrames": [frames], "events": events},
                                           indent=2) + "\n", encoding="utf-8")
            command += ["--capture-sequence", str(sequence)]
        command += list(extra)
        run(name, command, expected_failure=expected_failure)
        if expected_failure:
            return screenshot  # Deliberately no image is required or counted.
        if not screenshot.is_file():
            raise AssertionError(f"{name} did not create its screenshot")
        report["captures"][name] = str(screenshot)
        save_report()
        return screenshot

    def equivalent(name: str, reference: Path, candidate: Path, *, native=False, exact=False) -> None:
        metrics = image_metrics(reference, candidate)
        report["comparisons"].append({"name": name, **metrics})
        global_limit = report["thresholds"]["maxNativeMeanError" if native else "maxEquivalentMeanError"]
        cloud_limit = report["thresholds"]["maxNativeCloudMeanError" if native else "maxEquivalentCloudMeanError"]
        if exact:
            global_limit = cloud_limit = 0
        if metrics["meanAbsoluteError"] > global_limit or metrics["cloudMeanAbsoluteError"] > cloud_limit:
            raise AssertionError(
                f"{name}: mean RGB error {metrics['meanAbsoluteError']:.3f}, "
                f"cloud error {metrics['cloudMeanAbsoluteError']:.3f} exceed "
                f"{global_limit}/{cloud_limit}")
        save_report()

    def require_async_frame(image: Path, expected_frame: int, *, uploaded=False) -> None:
        telemetry = json.loads(image.with_suffix(".telemetry.json").read_text(encoding="utf-8"))
        state = telemetry.get("openvdbSequence", {})
        if (state.get("asynchronous") is not True or state.get("pending") is not False or
                state.get("requestedFrame") != expected_frame or state.get("displayedFrame") != expected_frame):
            raise AssertionError(f"{image.stem}: expected settled async frame {expected_frame}, got {state}")
        submitted = state.get("gpuUploadSubmitted")
        published = state.get("gpuUploadPublished")
        discarded = state.get("gpuUploadDiscarded")
        if (state.get("gpuUploadPending") is not False or
                any(type(value) is not int or value < 0 for value in (submitted, published, discarded)) or
                submitted != published + discarded):
            raise AssertionError(f"{image.stem}: upload ownership did not settle, got {state}")
        if uploaded and (submitted == 0 or published == 0):
            raise AssertionError(f"{image.stem}: no staged GPU frame was submitted and published")
        if not uploaded and submitted != 0:
            raise AssertionError(f"{image.stem}: paused startup unexpectedly replaced a GPU frame")
        report["comparisons"].append({"name": image.stem + "-settled-frame", **state})
        save_report()

    try:
        if not exe.is_file() or not tool.is_file():
            raise FileNotFoundError(f"renderer and tool must exist: {exe}, {tool}")
        sdf, fog, box = output / "sphere-sdf.vdb", output / "density.vdb", output / "occluder.bim"
        run("generate-sdf", [str(tool), "--sphere", str(sdf), "--radius", ".6",
                             "--voxel-size", ".05"])
        run("generate-fog", [str(tool), "--fog", str(sdf), str(fog), "--grid-name", "density"])
        if not sdf.is_file() or not fog.is_file():
            raise AssertionError("the OpenVDB tool did not create both fixtures")
        write_occluder(box)
        for label, technique, msaa in (("deferred", "deferred-raster", 1),
                                       ("forward-msaa4", "forward-raster", 4)):
            baseline = capture(label + "-empty", technique, msaa)
            cloud = capture(label + "-cloud", technique, msaa, volume=True)
            visibility = image_metrics(baseline, cloud)
            report["comparisons"].append({"name": label + "-volume-visible", **visibility})
            if visibility["changedFraction"] <= .01:
                raise AssertionError(f"{label}: volume changes <=1% of pixels at RGB difference 8")
            opaque = capture(label + "-occluder", technique, msaa, occluder=True)
            opaque_cloud = capture(label + "-occluded-cloud", technique, msaa,
                                   volume=True, occluder=True)
            occlusion = image_metrics(opaque, opaque_cloud)
            report["comparisons"].append({"name": label + "-opaque-depth", **occlusion})
            if occlusion["centerPatchMeanAbsoluteError"] > 2:
                raise AssertionError(f"{label}: fog overwrites the opaque center patch")
            minimum_side = report["thresholds"]["minChangedPixelsPerCloudSide"]
            if (occlusion["changedFraction"] <= .01 or
                    occlusion["leftCloudChangedPixels"] <= minimum_side or
                    occlusion["rightCloudChangedPixels"] <= minimum_side):
                raise AssertionError(f"{label}: cloud is not visible on both sides of the occluder")
            save_report()
        clipped = capture("deferred-section-clipped", "deferred-raster", 1,
                          volume=True, events=[{"frame": 1, "sectionPlane": [1, 0, 0, 0]}])
        clipping = image_metrics(Path(report["captures"]["deferred-empty"]), clipped)
        report["comparisons"].append({"name": "section-plane-half-space", **clipping})
        if clipping["leftCloudChangedPixels"] > 10 or clipping["rightCloudChangedPixels"] <= 64:
            raise AssertionError("section clipping did not remove only the negative-X half of the cloud")

        ortho_event = [{"frame": 1, "orthographic": True}]
        ortho_empty = capture("orthographic-empty", "deferred-raster", 1, events=ortho_event)
        ortho_cloud = capture("orthographic-cloud", "deferred-raster", 1, volume=True,
                              events=ortho_event, extra=("--vdb-volume-scale", "2"))
        ortho = image_metrics(ortho_empty, ortho_cloud)
        report["comparisons"].append({"name": "orthographic-volume-visible", **ortho})
        if ortho["changedPixels"] <= 64:
            raise AssertionError("orthographic view did not display the cloud")

        reactive_capture = capture("deferred-volume-reactive", "deferred-raster", 1,
                                   volume=True, occluder=True, taa=True, display="taa-reactive")
        reactive = reactive_metrics(reactive_capture)
        report["comparisons"].append({"name": "selective-temporal-reactivity", **reactive})
        if (reactive["cornerMean"] > 2 or reactive["occludedCenterMean"] > 2 or
                reactive["visibleCloudMean"] < 240):
            raise AssertionError("volume reactivity did not preserve clear/occluded pixels while rejecting visible fog")

        emission_options = ("--vdb-density", "0", "--vdb-emission-scale", "2")
        emission = capture("deferred-colored-emission", "deferred-raster", 1,
                           volume=True, extra=emission_options + ("--vdb-emission", "1", ".05", ".01"))
        thermal = capture("deferred-temperature-emission", "deferred-raster", 1,
                          volume=True, extra=emission_options + ("--vdb-temperature-grid", "density",
                                                                 "--vdb-temperature-scale", "4000"))
        emission_difference = image_metrics(emission, thermal)
        report["comparisons"].append({"name": "fixed-versus-temperature-emission", **emission_difference})
        if emission_difference["changedFraction"] <= .01:
            raise AssertionError("fixed and temperature emission did not produce distinct visible colors")
        hidden = capture("deferred-disabled-volume", "deferred-raster", 1,
                         volume=True, extra=("--no-vdb-volume",))
        disabled = image_metrics(Path(report["captures"]["deferred-empty"]), hidden)
        report["comparisons"].append({"name": "disabled-volume", **disabled})
        if disabled["changedPixels"] > 10:
            raise AssertionError("disabled volume changed the scene color")
        local_captures = {}
        for name, kind, intensity, away in (("point-off", "point", 0, False),
                                             ("point-on", "point", 12, False),
                                             ("spot-away", "spot", 12, True),
                                             ("spot-toward", "spot", 12, False)):
            fixture = output / (name + ".gltf")
            write_local_light(fixture, kind, intensity, away=away)
            local_captures[name] = capture(
                "deferred-" + name, "deferred-raster", 1, volume=True,
                extra=("--model", str(fixture), "--directional-intensity", "0",
                       "--environment-intensity", "0"))
        for label, first, second in (("point-light-scattering", "point-off", "point-on"),
                                     ("spot-cone-scattering", "spot-away", "spot-toward")):
            local = image_metrics(local_captures[first], local_captures[second])
            report["comparisons"].append({"name": label, **local})
            if local["changedFraction"] <= .01:
                raise AssertionError(label + " did not change a meaningful cloud area")

        area_captures = {}
        for name, intensity, away, count, shape in (
                ("off", 0, False, 1, "rect"), ("on", 12, False, 1, "rect"),
                ("bright", 24, False, 1, "rect"), ("backface", 12, True, 1, "rect"),
                ("two-emitters", 12, False, 2, "rect"), ("disk", 12, False, 1, "disk")):
            fixture = output / ("area-" + name + ".gltf")
            write_area_lights(fixture, intensity, away=away, count=count, shape=shape)
            area_captures[name] = capture(
                "deferred-area-" + name, "deferred-raster", 1, volume=True,
                extra=("--model", str(fixture), "--directional-intensity", "0",
                       "--environment-intensity", "0"))
        for label, first, second in (("area-light-scattering", "off", "on"),
                                     ("area-intensity-response", "on", "bright"),
                                     ("disk-light-scattering", "off", "disk")):
            metrics = image_metrics(area_captures[first], area_captures[second])
            report["comparisons"].append({"name": label, **metrics})
            if (metrics["changedPixels"] < report["thresholds"]["minAreaChangedPixels"] or
                    metrics["cloudMeanLuminanceDelta"] < report["thresholds"]["minAreaCloudLuminanceDelta"]):
                raise AssertionError(label + " did not increase visible cloud illumination")
        equivalent("area-backface-emits-no-radiance", area_captures["off"], area_captures["backface"])
        equivalent("two-area-emitters-equal-double-radiance", area_captures["bright"],
                   area_captures["two-emitters"])

        # Keep only fog frames in the sequence directory. Its SDF construction
        # inputs stay outside, so directory discovery cannot select an SDF.
        sequence_directory = output / "sequence"
        sequence_directory.mkdir()
        sequence_frames = []
        for index, radius, center in ((1, ".6", ("0", "0", "0")),
                                      (2, ".5", (".25", "0", "0")),
                                      (3, ".45", ("0", ".2", "0"))):
            sequence_sdf = sdf if index == 1 else output / f"sequence-source-{index}.vdb"
            if index != 1:
                run(f"generate-sequence-sdf-{index}", [str(tool), "--sphere", str(sequence_sdf),
                    "--radius", radius, "--center", *center, "--voxel-size", ".05"])
            frame = sequence_directory / f"smoke{index:03d}.vdb"
            run(f"generate-sequence-fog-{index}", [str(tool), "--fog", str(sequence_sdf),
                str(frame), "--grid-name", "density"])
            sequence_frames.append(frame)

        # Overlap must integrate the sum of extinction/emission, irrespective
        # of descriptor slot order; rendering A twice equals extinction scale2.
        medium_a = ("--vdb-volume", str(fog))
        medium_b = ("--vdb-volume", str(sequence_frames[1]))
        selection = ("--vdb-grid", "density")
        overlap_ab = capture("overlap-ab", "deferred-raster", 1,
                             extra=medium_a + medium_b + selection)
        overlap_ba = capture("overlap-ba", "deferred-raster", 1,
                             extra=medium_b + medium_a + selection)
        equivalent("overlap-order-independent", overlap_ab, overlap_ba)
        overlap_aa = capture("overlap-duplicate-a", "deferred-raster", 1,
                             extra=medium_a + medium_a + selection)
        doubled = capture("overlap-density-two", "deferred-raster", 1, volume=True,
                          extra=("--vdb-density", "2"))
        equivalent("overlap-equals-doubled-extinction", doubled, overlap_aa)
        if image_metrics(Path(report["captures"]["deferred-cloud"]), overlap_ab)["changedPixels"] < 64:
            raise AssertionError("adding the second overlapping volume made no visible contribution")
        overlap_four = capture("overlap-four-media", "deferred-raster", 1,
                               extra=medium_a + medium_b + medium_a + medium_b + selection)
        overlap_ab_doubled = capture("overlap-ab-density-two", "deferred-raster", 1,
                                    extra=medium_a + medium_b + selection + ("--vdb-density", "2"))
        equivalent("four-media-equals-doubled-two-media", overlap_ab_doubled, overlap_four)

        sequence_options = ("--vdb-sequence", str(sequence_directory), "--vdb-grid", "density",
                            "--vdb-fps", "60", "--vdb-no-loop")
        paused_first = capture("sequence-paused-first", "deferred-raster", 1,
                               extra=sequence_options + ("--vdb-paused",))
        equivalent("sequence-pause-holds-first", Path(report["captures"]["deferred-cloud"]), paused_first)
        zero_speed = capture("sequence-zero-speed", "deferred-raster", 1,
                             extra=sequence_options + ("--vdb-speed", "0"))
        equivalent("sequence-zero-speed-holds-first", paused_first, zero_speed)
        static_last = capture("sequence-static-last", "deferred-raster", 1,
                              extra=("--vdb-volume", str(sequence_frames[-1]), *selection))
        held_last = capture("sequence-advanced-held-last", "deferred-raster", 1,
                            extra=sequence_options)
        equivalent("sequence-advance-and-hold-last", static_last, held_last)
        start_last = capture("sequence-paused-start-last", "deferred-raster", 1,
                             extra=sequence_options + ("--vdb-start-frame", "2", "--vdb-paused"))
        equivalent("sequence-explicit-start-frame", static_last, start_last)
        if image_metrics(paused_first, held_last)["changedFraction"] <= .01:
            raise AssertionError("sequence playback never changed the visible density frame")
        async_options = sequence_options + ("--vdb-async",)
        async_first = capture("sequence-async-paused-first", "deferred-raster", 1,
                              extra=async_options + ("--vdb-paused",))
        equivalent("async-sequence-paused-first", paused_first, async_first)
        require_async_frame(async_first, 0)
        async_start_last = capture("sequence-async-start-last", "deferred-raster", 1,
            extra=async_options + ("--vdb-start-frame", "2", "--vdb-paused"))
        equivalent("async-sequence-explicit-start-frame", static_last, async_start_last)
        require_async_frame(async_start_last, 2)
        async_last = capture("sequence-async-advanced-held-last", "deferred-raster", 1,
                             extra=async_options)
        equivalent("async-sequence-advance-and-hold-last", static_last, async_last)
        require_async_frame(async_last, 2, uploaded=True)
        thermal_options = emission_options + ("--vdb-temperature-grid", "density",
                                               "--vdb-temperature-scale", "4000")
        static_last_thermal = capture("sequence-static-last-temperature", "deferred-raster", 1,
            extra=("--vdb-volume", str(sequence_frames[-1]), *selection, *thermal_options))
        async_last_thermal = capture("sequence-async-last-temperature", "deferred-raster", 1,
                                    extra=async_options + thermal_options)
        equivalent("async-gpu-density-and-temperature-upload", static_last_thermal, async_last_thermal)
        require_async_frame(async_last_thermal, 2, uploaded=True)
        static_three = capture("sequence-static-last-with-two-media", "deferred-raster", 1,
            extra=("--vdb-volume", str(sequence_frames[-1])) + medium_a + medium_b + selection)
        async_three = capture("sequence-async-last-with-two-media", "deferred-raster", 1,
                              extra=async_options + medium_a + medium_b)
        equivalent("async-gpu-replacement-retains-other-media", static_three, async_three)
        require_async_frame(async_three, 2, uploaded=True)
        # Capture settling deliberately retires pending uploads before each
        # deterministic tick. These events prove descriptor recreation and
        # subsequent sequence publication remain coherent; the separate gated
        # Vulkan API tests exercise resize/shutdown while a fence is unfinished.
        async_recreated = capture("sequence-async-resource-round-trip", "deferred-raster", 1,
            extra=async_options, frames=6, events=[
                {"frame": 1, "resize": [352, 220]},
                {"frame": 2, "acquireOutOfDate": True},
                {"frame": 3, "resize": [WIDTH, HEIGHT], "samples": 4, "technique": "forward-raster"},
                {"frame": 4, "presentSuboptimal": True},
                {"frame": 5, "samples": 1, "technique": "deferred-raster"}])
        equivalent("async-gpu-resize-msaa-technique-round-trip", static_last, async_recreated)
        require_async_frame(async_recreated, 2, uploaded=True)
        invalid_sequence = output / "invalid-async-sequence"
        invalid_sequence.mkdir()
        (invalid_sequence / "smoke001.vdb").write_bytes(sequence_frames[0].read_bytes())
        (invalid_sequence / "smoke002.vdb").write_text("invalid VDB payload\n", encoding="utf-8")
        capture("sequence-async-invalid-requested-frame", "deferred-raster", 1,
            extra=("--vdb-sequence", str(invalid_sequence), "--vdb-fps", "60", "--vdb-no-loop",
                   "--vdb-async", "--vdb-grid", "density"),
            expected_failure=("OpenVDB sequence frame 1", "smoke002.vdb"))

        caster = output / "volume-shadow-caster.bim"
        write_shadow_caster(caster)
        shadow_options = ("--bim-model", str(caster), "--directional-direction", "-1", "0", "0",
                          "--directional-intensity", "8", "--environment-intensity", "0")
        shadows_on = capture("geometry-shadow-on", "deferred-raster", 1, volume=True,
                             extra=shadow_options)
        shadows_off = capture("geometry-shadow-off", "deferred-raster", 1, volume=True,
                              extra=shadow_options + ("--no-vdb-shadows",))
        geometry_shadow = image_metrics(shadows_on, shadows_off)
        report["comparisons"].append({"name": "raster-geometry-shadows-on-fog", **geometry_shadow})
        if geometry_shadow["changedPixels"] < 100 or geometry_shadow["cloudMeanLuminanceDelta"] < .25:
            raise AssertionError("off-camera geometry did not shadow the fog through raster shadow maps")
        if geometry_shadow["centerPatchMeanAbsoluteError"] > 2:
            raise AssertionError("volume geometry-shadow toggle changed the opaque depth-occluded patch")

        area_shadow_fixture = output / "area-shadow-light.gltf"
        # RotationY(+90deg) turns the default -Z normal toward the fog at -X.
        # The existing x3 offscreen box lies between emitter x5 and the cloud.
        write_area_lights(area_shadow_fixture, 160, position=(5, 0, 0),
                          rotation=(0, 2 ** -.5, 0, 2 ** -.5), light_range=8)
        area_shadow_options = ("--model", str(area_shadow_fixture), "--bim-model", str(caster),
                               "--directional-intensity", "0", "--environment-intensity", "0")
        area_shadow_on = capture("area-geometry-shadow-on", "deferred-raster", 1,
                                 volume=True, extra=area_shadow_options)
        area_shadow_off = capture("area-geometry-shadow-off", "deferred-raster", 1,
            volume=True, extra=area_shadow_options + ("--no-vdb-shadows",))
        area_geometry_shadow = image_metrics(area_shadow_on, area_shadow_off)
        report["comparisons"].append({"name": "area-raster-geometry-shadows-on-fog", **area_geometry_shadow})
        if (area_geometry_shadow["changedPixels"] < report["thresholds"]["minAreaChangedPixels"] or
                area_geometry_shadow["cloudMeanLuminanceDelta"] < report["thresholds"]["minAreaCloudLuminanceDelta"]):
            raise AssertionError("off-camera geometry did not shadow area-lit fog through local shadow maps")
        if area_geometry_shadow["centerPatchMeanAbsoluteError"] > 2:
            raise AssertionError("area-light volume shadow toggle changed the opaque depth-occluded patch")

        # The optional closure changes higher-order light scattering only.
        # Check its physical zeros and media-mixture invariants against actual
        # rendered references, rather than reproducing shader arithmetic here.
        multiple_max = ("--vdb-multiple-scattering", "1", "--vdb-scattering-orders", "4")
        multiple_zero = capture("multiple-strength-zero", "deferred-raster", 1, volume=True,
                                extra=("--vdb-multiple-scattering", "0", "--vdb-scattering-orders", "4"))
        equivalent("multiple-zero-preserves-single-scattering",
                   Path(report["captures"]["deferred-cloud"]), multiple_zero, exact=True)
        multiple_vacuum = capture("multiple-vacuum", "deferred-raster", 1, volume=True,
                                  extra=multiple_max + ("--vdb-density", "0"))
        equivalent("multiple-vacuum-preserves-background",
                   Path(report["captures"]["deferred-empty"]), multiple_vacuum, exact=True)
        absorbing_options = ("--vdb-density", "4", "--vdb-scattering", "0", "0", "0")
        absorbing = capture("multiple-absorbing-reference", "deferred-raster", 1,
                            volume=True, extra=absorbing_options)
        absorbing_multiple = capture("multiple-zero-albedo", "deferred-raster", 1,
                                     volume=True, extra=absorbing_options + multiple_max)
        equivalent("multiple-zero-albedo-adds-no-light", absorbing, absorbing_multiple, exact=True)
        emission_multiple = capture("multiple-emission-only", "deferred-raster", 1, volume=True,
            extra=emission_options + ("--vdb-emission", "1", ".05", ".01") + multiple_max)
        equivalent("multiple-preserves-emission-only", emission, emission_multiple, exact=True)
        scattering_options = ("--vdb-density", "4", "--directional-intensity", "4",
                              "--environment-intensity", "0", "--vdb-anisotropy", ".35",
                              "--vdb-scattering", ".8", ".6", ".4")
        scattering_single = capture("multiple-single-reference", "deferred-raster", 1,
                                    volume=True, extra=scattering_options)
        scattering_low = capture("multiple-one-order", "deferred-raster", 1, volume=True,
            extra=scattering_options + ("--vdb-multiple-scattering", ".5", "--vdb-scattering-orders", "1"))
        scattering_full = capture("multiple-four-orders", "deferred-raster", 1, volume=True,
                                  extra=scattering_options + multiple_max)
        for label, reference, candidate in (
                ("multiple-light-contribution", scattering_single, scattering_low),
                ("multiple-strength-and-order-response", scattering_low, scattering_full)):
            metrics = image_metrics(reference, candidate)
            report["comparisons"].append({"name": label, **metrics})
            if metrics["changedPixels"] < 25 or metrics["cloudMeanLuminanceDelta"] < .1:
                raise AssertionError(label + " did not increase cloud illumination")
        multiple_occluded = capture("multiple-opaque-depth", "deferred-raster", 1,
                                    volume=True, occluder=True, extra=multiple_max)
        metrics = image_metrics(Path(report["captures"]["deferred-occluder"]), multiple_occluded)
        report["comparisons"].append({"name": "multiple-preserves-opaque-patch", **metrics})
        if metrics["centerPatchMeanAbsoluteError"] > 2:
            raise AssertionError("multiple scattering overwrote opaque scene depth")
        multiple_ab = capture("multiple-media-ab", "deferred-raster", 1,
                              extra=medium_a + medium_b + selection + multiple_max)
        multiple_ba = capture("multiple-media-ba", "deferred-raster", 1,
                              extra=medium_b + medium_a + selection + multiple_max)
        equivalent("multiple-media-order-independence", multiple_ab, multiple_ba, exact=True)
        multiple_four = capture("multiple-four-media", "deferred-raster", 1,
            extra=medium_a + medium_b + medium_a + medium_b + selection + multiple_max)
        multiple_double = capture("multiple-doubled-density", "deferred-raster", 1,
            extra=medium_a + medium_b + selection + multiple_max + ("--vdb-density", "2"))
        equivalent("multiple-mixture-density-equivalence", multiple_four, multiple_double, exact=True)
        multiple_forward = capture("multiple-forward-msaa4", "forward-raster", 4,
                                   volume=True, extra=multiple_max)
        metrics = image_metrics(Path(report["captures"]["forward-msaa4-cloud"]), multiple_forward)
        report["comparisons"].append({"name": "multiple-forward-visible", **metrics})
        if metrics["changedPixels"] < 25 or metrics["cloudMeanLuminanceDelta"] < .1:
            raise AssertionError("multiple scattering did not affect the forward/MSAA cloud")
        for label, fixture, reference in (
                ("point", output / "point-on.gltf", local_captures["point-on"]),
                ("area", output / "area-on.gltf", area_captures["on"])):
            multiple_local = capture("multiple-" + label + "-light", "deferred-raster", 1,
                volume=True, extra=("--model", str(fixture), "--directional-intensity", "0",
                                    "--environment-intensity", "0") + multiple_max)
            metrics = image_metrics(reference, multiple_local)
            report["comparisons"].append({"name": "multiple-" + label + "-response", **metrics})
            if metrics["changedPixels"] < 25 or metrics["cloudMeanLuminanceDelta"] < .1:
                raise AssertionError("multiple scattering did not affect " + label + " lighting")
        multiple_spot_away = capture("multiple-spot-away", "deferred-raster", 1, volume=True,
            extra=("--model", str(output / "spot-away.gltf"), "--directional-intensity", "0",
                   "--environment-intensity", "0") + multiple_max)
        equivalent("multiple-respects-spot-cone", local_captures["spot-away"],
                   multiple_spot_away, exact=True)
        for label, options, diagnostic in (
                ("negative-strength", ("--vdb-multiple-scattering", "-.1"), "--vdb-multiple-scattering"),
                ("large-strength", ("--vdb-multiple-scattering", "1.1"), "--vdb-multiple-scattering"),
                ("zero-orders", ("--vdb-scattering-orders", "0"), "--vdb-scattering-orders"),
                ("large-orders", ("--vdb-scattering-orders", "5"), "--vdb-scattering-orders")):
            run("multiple-invalid-" + label, [str(exe), *options], expected_failure=(diagnostic,))

        # Transparent fragments must be inserted at their actual depth along
        # the medium ray. With pure absorption, moving an identical alpha plane
        # toward the camera strictly increases its visible unlit contribution.
        glass_color = (.9, .35, .1)
        glass_planes = {}
        for label, z in (("front", .9), ("inside", 0.0), ("behind", -.9)):
            fixture = output / ("glass-" + label + ".gltf")
            write_transparent_planes(fixture, [(z, .6, glass_color)])
            glass_planes[label] = fixture
        glass_layers = [( .9, .35, (.9, .1, .05)),
                        (0.0, .45, (.05, .8, .1)),
                        (-.9, .55, (.1, .2, .9))]
        glass_stack, glass_stack_reverse = output / "glass-stack.gltf", output / "glass-stack-reverse.gltf"
        write_transparent_planes(glass_stack, glass_layers)
        write_transparent_planes(glass_stack_reverse, glass_layers, reverse=True)
        glass_opaque_front = output / "glass-opaque-front.gltf"
        write_transparent_planes(glass_opaque_front, [(.9, 1.0, glass_color)])
        glass_absorption = ("--vdb-density", "4", "--vdb-scattering", "0", "0", "0")
        glass_captures = {}
        for label, technique, msaa in (("deferred", "deferred-raster", 1),
                                       ("forward-msaa4", "forward-raster", 4)):
            for depth_label, fixture in glass_planes.items():
                glass_captures[(label, depth_label)] = capture(
                    label + "-glass-" + depth_label, technique, msaa, volume=True,
                    extra=("--model", str(fixture)) + glass_absorption)
            for name, first, second in (("inside-versus-behind", "behind", "inside"),
                                         ("front-versus-inside", "inside", "front")):
                metrics = image_metrics(glass_captures[(label, first)], glass_captures[(label, second)])
                report["comparisons"].append({"name": label + "-glass-" + name, **metrics})
                if metrics["changedPixels"] < 100 or metrics["cloudMeanLuminanceDelta"] < .25:
                    raise AssertionError(label + ": transparent surface depth did not change fog attenuation")
            stack_reference = capture(label + "-glass-stack-reference", technique, msaa,
                                      extra=("--model", str(glass_stack)))
            stack_vacuum = capture(label + "-glass-stack-vacuum", technique, msaa,
                volume=True, extra=("--model", str(glass_stack), "--vdb-density", "0"))
            equivalent(label + "-glass-vacuum-preserves-existing-resolve", stack_reference, stack_vacuum)
            stack = capture(label + "-glass-stack-medium", technique, msaa, volume=True,
                extra=("--model", str(glass_stack)) + glass_absorption + multiple_max)
            stack_reverse = capture(label + "-glass-stack-reversed", technique, msaa, volume=True,
                extra=("--model", str(glass_stack_reverse)) + glass_absorption + multiple_max)
            equivalent(label + "-glass-submission-order-independence", stack, stack_reverse)
            front_reference = capture(label + "-glass-opaque-front-reference", technique, msaa,
                                      extra=("--model", str(glass_opaque_front)))
            front_medium = capture(label + "-glass-opaque-front-medium", technique, msaa,
                volume=True, extra=("--model", str(glass_opaque_front)) + multiple_max)
            equivalent(label + "-glass-front-alpha-one-occludes-medium", front_reference, front_medium)
            # The renderer supports TAA only at one sample. The composition
            # cases above retain forward 4x MSAA coverage independently.
            glass_reactive = capture(technique + "-glass-reactive", technique, 1, volume=True,
                taa=True, display="taa-reactive", extra=("--model", str(glass_opaque_front)))
            reactive = reactive_metrics(glass_reactive)
            report["comparisons"].append({"name": label + "-glass-reactivity", **reactive})
            if reactive["cornerMean"] > 2 or reactive["occludedCenterMean"] < 240:
                raise AssertionError(label + ": consumed transparent fragments lost temporal reactivity")
        glass_multiple = capture("glass-multiple-scattering", "forward-raster", 4, volume=True,
            extra=("--model", str(glass_stack)) + scattering_options + multiple_max)
        glass_recreated = capture("glass-resource-round-trip", "deferred-raster", 1, volume=True,
            extra=("--model", str(glass_stack)) + scattering_options + multiple_max,
            frames=6, events=[{"frame": 1, "resize": [352, 220]},
                              {"frame": 2, "samples": 4, "technique": "forward-raster"},
                              {"frame": 3, "acquireOutOfDate": True},
                              {"frame": 4, "resize": [WIDTH, HEIGHT]},
                              {"frame": 5, "presentSuboptimal": True}])
        equivalent("glass-resize-msaa-technique-and-acquire-recovery", glass_multiple, glass_recreated)

        if args.native_sparse:
            native = capture("native-sparse-cloud", "deferred-raster", 1, volume=True,
                             extra=("--vdb-sparse",))
            equivalent("native-versus-dense-density", Path(report["captures"]["deferred-cloud"]), native,
                       native=True)
            if image_metrics(Path(report["captures"]["deferred-empty"]), native)["changedFraction"] <= .01:
                raise AssertionError("native sparse traversal did not display visible density")
            native_thermal = capture("native-sparse-temperature-emission", "deferred-raster", 1,
                volume=True, extra=emission_options + ("--vdb-temperature-grid", "density",
                    "--vdb-temperature-scale", "4000", "--vdb-sparse"))
            equivalent("native-versus-dense-temperature", thermal, native_thermal, native=True)
            native_forward = capture("native-sparse-forward-msaa4", "forward-raster", 4,
                                     volume=True, extra=("--vdb-sparse",))
            equivalent("native-versus-dense-forward-msaa4",
                       Path(report["captures"]["forward-msaa4-cloud"]), native_forward, native=True)
            native_four = capture("native-sparse-four-media", "deferred-raster", 1,
                                  extra=medium_a + medium_b + medium_a + medium_b + selection +
                                        ("--vdb-sparse",))
            equivalent("native-versus-dense-four-media", overlap_four, native_four, native=True)
            native_async = capture("native-sparse-async-held-last", "deferred-raster", 1,
                                   extra=async_options + ("--vdb-sparse",))
            equivalent("native-async-sequence-advance-and-hold-last", static_last, native_async, native=True)
            require_async_frame(native_async, 2, uploaded=True)
            native_async_thermal = capture("native-sparse-async-last-temperature", "deferred-raster", 1,
                extra=async_options + thermal_options + ("--vdb-sparse",))
            equivalent("native-async-gpu-density-and-temperature-upload", static_last_thermal,
                       native_async_thermal, native=True)
            require_async_frame(native_async_thermal, 2, uploaded=True)
            native_multiple = capture("native-sparse-multiple-scattering", "deferred-raster", 1,
                volume=True, extra=scattering_options + multiple_max + ("--vdb-sparse",))
            equivalent("native-versus-dense-multiple-scattering", scattering_full,
                       native_multiple, native=True)
            native_multiple_forward = capture("native-sparse-multiple-forward-msaa4", "forward-raster", 4,
                volume=True, extra=multiple_max + ("--vdb-sparse",))
            equivalent("native-versus-dense-multiple-forward-msaa4", multiple_forward,
                       native_multiple_forward, native=True)
            native_glass = capture("native-sparse-glass-inside", "deferred-raster", 1, volume=True,
                extra=("--model", str(glass_planes["inside"]), "--vdb-sparse") + glass_absorption)
            equivalent("native-versus-dense-glass-depth", glass_captures[("deferred", "inside")],
                       native_glass, native=True)
            native_glass_stack = capture("native-sparse-glass-stack", "forward-raster", 4, volume=True,
                extra=("--model", str(glass_stack), "--vdb-sparse") + scattering_options + multiple_max)
            equivalent("native-versus-dense-glass-stack-forward-msaa4",
                       glass_multiple, native_glass_stack, native=True)
        save_report()
        if args.surface_smoke:
            surface = capture("deferred-surface", "deferred-raster", 1, surface=True)
            metrics = image_metrics(Path(report["captures"]["deferred-empty"]), surface)
            report["comparisons"].append({"name": "surface-import", **metrics})
            if metrics["changedFraction"] <= .01:
                raise AssertionError("surface import did not change a meaningful image area")
        report["passed"] = True
    except Exception as error:
        report["errors"].append({"type": type(error).__name__, "message": str(error),
                                 "traceback": traceback.format_exc()})
        print(f"FAIL: {error}", file=sys.stderr)
    finally:
        save_report()
        print(f"OpenVDB runtime results: {output / 'results.json'}", flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
