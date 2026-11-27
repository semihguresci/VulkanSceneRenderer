"""Check calibrated BIM sensor-camera outputs against analytic native IFC geometry."""

import argparse
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from display_capture_test_support import validation_log_error


WIDTH, HEIGHT = 127, 95
FX, FY, CX, CY = 72.0, 64.0, 54.5, 38.5
INTRINSICS = dict(width=WIDTH, height=HEIGHT, fx=FX, fy=FY, cx=CX, cy=CY,
                  nearMeters=0.2, farMeters=6.0)
NAMESPACE = "camera-golden"
TICK_RATE = 12
SENSOR_TIMING = dict(ticksPerSecond=TICK_RATE, framesPerSecondMilliHz=5000,
                     firstTick=1, lastTick=9)
SENSOR_TICKS = (1, 4, 6, 9)
MOUNT = (0.08, -0.055, 0.03)
CHANNELS = {"B": 2, "G": 2, "InstanceID": 0, "R": 2, "SemanticID": 0, "Z": 2}
IDS = {"wall": "0000000000000000000027", "rear": "0000000000000000000037",
       "glass": "0000000000000000000057"}


@dataclass(frozen=True)
class Surface:
    key: str
    ifc_class: str
    lower: tuple
    upper: tuple
    rgb: tuple
    transparency: float = 0.0


# Bounds are authored independently in metric REP-103 (+X forward, +Z up).
# The rear face projects beyond the wall on all sides for the first frame.
WALL = Surface("wall", "IFCWALL", (2.0, -0.47, -0.23), (2.15, 0.34, 0.37), (.9, .015, .015))
REAR = Surface("rear", "IFCBUILDINGELEMENTPROXY", (4.0, -1.30, -0.72), (4.22, 1.55, 0.81),
               (.015, .025, .9))
GLASS = Surface("glass", "IFCBUILDINGELEMENTPROXY", (.9, -.9, -.6), (1.0, .9, .8),
                (.015, .9, .015), transparency=.5)


def fixture(path, reverse=False):
    def product(surface, n):
        # STEP coordinates are +Z-up millimetres. The importer maps
        # IFC (x,y,z) to REP-103 (y,-x,z).
        x0, y0, z0 = surface.lower
        x1, y1, z1 = surface.upper
        x, y, z = -(y0 + y1) * 500, (x0 + x1) * 500, z0 * 1000
        wide, thick, high = (y1 - y0) * 1000, (x1 - x0) * 1000, (z1 - z0) * 1000
        r, g, b = surface.rgb
        category = ".STANDARD." if surface is WALL else ".NOTDEFINED."
        return f"""
#{n}=IFCCARTESIANPOINT(({x:.6f},{y:.6f},{z:.6f}));
#{n+1}=IFCAXIS2PLACEMENT3D(#{n},#5,#6);
#{n+2}=IFCLOCALPLACEMENT(#8,#{n+1});
#{n+3}=IFCRECTANGLEPROFILEDEF(.AREA.,$,$,{wide:.6f},{thick:.6f});
#{n+4}=IFCEXTRUDEDAREASOLID(#{n+3},#7,#5,{high:.6f});
#{n+5}=IFCSHAPEREPRESENTATION(#9,'Body','SweptSolid',(#{n+4}));
#{n+6}=IFCPRODUCTDEFINITIONSHAPE($,$,(#{n+5}));
#{n+7}={surface.ifc_class}('{IDS[surface.key]}',$,'{surface.key}',$,$,#{n+2},#{n+6},$,{category});
#{n+8}=IFCCOLOURRGB($,{r:.6f},{g:.6f},{b:.6f});
#{n+9}=IFCSURFACESTYLESHADING(#{n+8},{surface.transparency:.6f});
#{n+40}=IFCSURFACESTYLE($,.BOTH.,(#{n+9}));
#{n+41}=IFCSTYLEDITEM(#{n+4},(#{n+40}),$);
"""

    order = (GLASS, REAR, WALL) if reverse else (WALL, REAR, GLASS)
    steps = (20, 30, 50)
    parts = "".join(product(surface, step) for surface, step in zip(order, steps))
    members = ",".join(f"#{step + 7}" for step in steps)
    path.write_text(f"""ISO-10303-21;
HEADER;
FILE_DESCRIPTION(('Metric camera targets'),'2;1');
FILE_NAME('camera-golden.ifc','2026-11-27T00:00:00',(''),(''),'','','');
FILE_SCHEMA(('IFC4'));
ENDSEC;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);
#2=IFCUNITASSIGNMENT((#1));
#3=IFCPROJECT('0000000000000000000003',$,'Camera test',$,$,$,$,(#9),#2);
#4=IFCCARTESIANPOINT((0.,0.,0.));
#5=IFCDIRECTION((0.,0.,1.));
#6=IFCDIRECTION((1.,0.,0.));
#7=IFCAXIS2PLACEMENT3D(#4,#5,#6);
#8=IFCLOCALPLACEMENT($,#7);
#9=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,0.001,#7,$);
{parts}
#40=IFCBUILDINGSTOREY('0000000000000000000040',$,'Storey',$,$,#8,$,$,.ELEMENT.,0.);
#41=IFCBUILDING('0000000000000000000041',$,'Building',$,$,#8,$,$,.ELEMENT.,$,$,$);
#42=IFCSITE('0000000000000000000042',$,'Site',$,$,#8,$,$,.ELEMENT.,$,$,$,$,$);
#43=IFCRELAGGREGATES('0000000000000000000043',$,$,$,#3,(#42));
#44=IFCRELAGGREGATES('0000000000000000000044',$,$,$,#42,(#41));
#45=IFCRELAGGREGATES('0000000000000000000045',$,$,$,#41,(#40));
#46=IFCRELCONTAINEDINSPATIALSTRUCTURE('0000000000000000000046',$,$,$,({members}),#40);
ENDSEC;
END-ISO-10303-21;
""", encoding="ascii")


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def base_pose(tick):
    return (tick * .07, tick * .014, tick * .018), tick * .012


def camera_pose(tick):
    (x, y, z), yaw = base_pose(tick)
    sine, cosine = math.sin(yaw), math.cos(yaw)
    mx, my, mz = MOUNT
    return ((sine, -cosine, 0, 0), (0, 0, 1, 0), (-cosine, -sine, 0, 0),
            (x + cosine * mx - sine * my, y + sine * mx + cosine * my, z + mz, 1))


def read_exr(path, *, include_ground_truth=False):
    # OpenEXR v2 scanlines. Decode IDs directly as UINT; a float image loader
    # can silently change instance IDs above the exact binary32 integer range.
    def c_string(data, start):
        end = data.index(0, start)
        return data[start:end].decode("ascii"), end + 1

    payload = path.read_bytes()
    assert struct.unpack_from("<II", payload) == (20000630, 2), path
    attributes = {}
    position = 8
    while payload[position]:
        name, position = c_string(payload, position)
        kind, position = c_string(payload, position)
        size, = struct.unpack_from("<I", payload, position)
        position += 4
        assert name not in attributes and size <= len(payload) - position, (path, name)
        attributes[name] = kind, payload[position:position + size]
        position += size
    window = struct.pack("<iiii", 0, 0, WIDTH - 1, HEIGHT - 1)
    assert attributes["dataWindow"] == attributes["displayWindow"] == ("box2i", window), path
    assert attributes["compression"] == ("compression", b"\0"), path
    kind, channel_list = attributes["channels"]
    assert kind == "chlist", path
    channels = {}
    cursor = 0
    while channel_list[cursor]:
        name, cursor = c_string(channel_list, cursor)
        pixel_type, _, x_sample, y_sample = struct.unpack_from("<iB3xii", channel_list, cursor)
        cursor += 16
        assert name not in channels and x_sample == y_sample == 1, (path, name)
        channels[name] = pixel_type
    expected_channels = dict(CHANNELS, GroundTruthZ=2) if include_ground_truth else CHANNELS
    assert cursor == len(channel_list) - 1 and channels == expected_channels, (path, channels)

    planes = {name: [None] * HEIGHT for name in channels}
    offsets = struct.unpack_from(f"<{HEIGHT}Q", payload, position + 1)
    scanline_size = WIDTH * 4 * len(channels)
    first_offset = position + 1 + 8 * HEIGHT
    for offset in offsets:
        assert first_offset <= offset <= len(payload) - 8 - scanline_size, (path, offset)
        row, size = struct.unpack_from("<iI", payload, offset)
        assert 0 <= row < HEIGHT and size == scanline_size, (path, row, size)
        for index, (channel, pixel_type) in enumerate(channels.items()):
            assert planes[channel][row] is None, (path, row, channel)
            fmt = f"<{WIDTH}" + ("I" if pixel_type == 0 else "f")
            planes[channel][row] = struct.unpack_from(fmt, payload, offset + 8 + index * WIDTH * 4)
    return planes


def ray_box(origin, direction, surface):
    entry, exit = -math.inf, math.inf
    front_face = False
    for axis in range(3):
        lower, upper = surface.lower[axis], surface.upper[axis]
        if abs(direction[axis]) < 1e-12:
            if not lower <= origin[axis] <= upper:
                return None
            continue
        a, b = sorted(((lower - origin[axis]) / direction[axis], (upper - origin[axis]) / direction[axis]))
        if a > entry:
            entry, front_face = a, axis == 0
        exit = min(exit, b)
    return (entry, front_face) if exit >= entry > 0 else None


def expected_pixel(u, v, pose):
    right, up, back, origin = pose
    direction = tuple(-back[i] + (u - CX) * right[i] / FX + (CY - v) * up[i] / FY for i in range(3))
    candidates = []
    for surface in (WALL, REAR):
        hit = ray_box(origin, direction, surface)
        if hit and INTRINSICS["nearMeters"] <= hit[0] <= INTRINSICS["farMeters"]:
            candidates.append((hit[0], surface.key, hit[1]))
    return min(candidates, default=(0, None, False))


def verify_frame(directory, index, tick, labels):
    stem = directory / f"frame-{index:08}"
    metadata = json.loads(stem.with_suffix(".json").read_bytes())
    assert (metadata["schema"], metadata["version"]) == ("container.sensor-camera-frame", 1), stem
    assert (metadata["sampleIndex"], metadata["tick"]) == (index, tick), (stem, metadata)
    assert math.isclose(metadata["seconds"], tick / TICK_RATE, abs_tol=1e-12), (stem, metadata)
    assert metadata["intrinsics"] == INTRINSICS, (stem, metadata)
    assert metadata["worldFrame"] == "REP-103 metres" and metadata["matrixLayout"] == "column-major", stem
    assert metadata["pixelCenters"] == "i+0.5,j+0.5" and metadata["imageOrigin"] == "top-left", stem
    assert metadata["backgroundId"] == metadata["unmappedPixelCount"] == metadata["rangeRejectedPixelCount"] == 0, stem
    pose = camera_pose(tick)
    assert len(metadata["worldFromCamera"]) == 4, metadata
    for actual, expected in zip(metadata["worldFromCamera"], pose):
        assert len(actual) == 4 and all(abs(a - b) < 1e-10 for a, b in zip(actual, expected)), (stem, actual, expected)
    planes = read_exr(stem.with_suffix(".exr"))
    valid_labels = {labels["wall"], labels["rear"], (0, 0)}
    counts = {None: 0, "wall": 0, "rear": 0}
    colors = {key: [0, 0] for key in ("wall", "rear")}
    depth_summary = {}
    for row in range(HEIGHT):
        for column in range(WIDTH):
            instance, semantic, depth = (planes[name][row][column] for name in ("InstanceID", "SemanticID", "Z"))
            rgb = tuple(planes[name][row][column] for name in "RGB")
            location = (stem, column, row)
            assert all(map(math.isfinite, (*rgb, depth))), (location, depth, rgb)
            assert (instance, semantic) in valid_labels and bool(instance) == (depth > 0), (location, instance, semantic, depth)

            u, v = column + .5, row + .5
            expected_depth, name, front = expected_pixel(u, v, pose)
            # Exclude a narrow band around projected silhouettes. Subpixel
            # edge coverage can vary by device even at single-sample settings.
            if any(expected_pixel(u + dx, v + dy, pose)[1] != name
                   for dx, dy in ((-.75, 0), (.75, 0), (0, -.75), (0, .75))):
                continue
            counts[name] += 1
            assert (instance, semantic) == labels.get(name, (0, 0)), (location, name, instance, semantic)
            assert math.isclose(depth, expected_depth, rel_tol=0, abs_tol=3e-4), (location, name, depth, expected_depth)
            if name and front:
                dominant, other = (rgb[0], rgb[2]) if name == "wall" else (rgb[2], rgb[0])
                colors[name][0] += dominant > max(other, rgb[1]) * 1.25 and dominant > .005
                colors[name][1] += 1
                depth_summary.setdefault(name, depth)

    assert min(counts.values()) >= 100, (stem, counts)
    for name, (dominant, total) in colors.items():
        assert total >= 100 and dominant >= .9 * total, (stem, name, dominant, total)
    return planes, {"checkedPixels": {key or "background": value for key, value in counts.items()},
                    "witnessDepthMeters": depth_summary}


def verify_dataset(directory, ticks, scale):
    dataset = json.loads((directory / "dataset.json").read_bytes())
    assert (dataset["schema"], dataset["version"], dataset["frameCount"]) == (
        "container.sensor-camera-dataset", 1, len(ticks)), dataset
    assert (dataset["colorSpace"], dataset["depthUnits"], dataset["surfacePolicy"]) == (
        "linear-sRGB", "metres", "opaque-and-alpha-mask"), dataset
    assert (dataset["labels"], dataset["frameImagePattern"], dataset["frameMetadataPattern"]) == (
        "labels.json", "frame-%08d.exr", "frame-%08d.json"), dataset
    expected_files = {"dataset.json", "labels.json"}
    for index in range(len(ticks)):
        expected_files.update((f"frame-{index:08}.exr", f"frame-{index:08}.json"))
    assert {item.name for item in directory.iterdir()} == expected_files, directory

    manifest = json.loads((directory / dataset["labels"]).read_bytes())
    assert (manifest["schema"], manifest["version"], manifest["backgroundId"], manifest["sourceNamespace"]) == (
        "container.bim-sensor-labels", 1, 0, NAMESPACE), manifest
    assert math.isclose(manifest["metersPerRendererUnit"], 1 / scale, abs_tol=1e-6), manifest
    by_guid = {row["guid"]: row for row in manifest["instances"]}
    class_names = {row["semanticId"]: row["ifcClass"] for row in manifest["classes"]}
    labels = {}
    for surface in (WALL, REAR, GLASS):
        row = by_guid[IDS[surface.key]]
        assert 0 < row["instanceId"] <= 0xFFFFFFFF and row["representationIds"], row
        assert class_names[row["semanticId"]] == surface.ifc_class, row
        labels[surface.key] = row["instanceId"], row["semanticId"]
    assert len({pair[0] for pair in labels.values()}) == len(labels), manifest
    frames = [verify_frame(directory, index, tick, labels) for index, tick in enumerate(ticks)]
    return labels, frames


def capture(renderer, work, model, spec, output, *, log_name, scale=1, trajectory=None, failure=None):
    command = [str(renderer), "--model", str(model), "--import-scale", str(scale),
               "--hidden", "--no-ui", "--validation", "--render-technique", "deferred-raster",
               "--msaa", "1", "--no-taa", "--no-bloom", "--no-ray-query", "--no-ddgi",
               "--sky", "plain", "--sky-horizon", "0", "0", "0", "--environment-intensity", "0",
               "--directional-intensity", "3", "--directional-direction", "0", "0", "-1",
               "--bounce-intensity", "0", "--sensor-camera", str(spec), "--sensor-output", str(output),
               "--sensor-namespace", NAMESPACE]
    if trajectory is not None:
        command.extend(("--sensor-trajectory", str(trajectory)))
    log = work / log_name
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                timeout=150, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(work),
                                                      VK_LOADER_DEBUG="layer"),
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    contents = log.read_text(encoding="utf-8", errors="replace")
    assert not validation_log_error(contents), f"Vulkan validation failed in {log}"
    if failure:
        assert result.returncode != 0 and failure in contents, log
    else:
        assert result.returncode == 0, log
        assert 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in contents, log
        assert 'Inserted device layer "VK_LAYER_KHRONOS_validation"' in contents, log


def hashes(directory):
    return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in directory.iterdir() if path.is_file()}


def verify_randomization(renderer, work, model, spec, baseline, clean):
    seed = 7295
    lighting = {"seed": seed, "lightingVariation": 0.7}
    material = {"seed": seed, "materialVariation": 0.8}
    cases = {
        "lighting-only": {"appearance": lighting},
        "material-only": {"appearance": material},
        "rgb-noise-only": {"rgbNoise": {"stdDevLinear": 0.01, "seed": seed + 2}},
    }
    for case, profile_seed in (("randomized", seed), ("randomized-repeat", seed),
                               ("randomized-alternate-seed", seed + 1)):
        cases[case] = {
            "appearance": {**lighting, **material, "seed": profile_seed},
            "rgbNoise": {"stdDevLinear": 0.01, "seed": profile_seed + 2},
            "depthNoise": {"dropoutProbability": 0.3, "seed": profile_seed + 4},
        }

    image_name, metadata_name = "frame-00000000.exr", "frame-00000000.json"
    calibration = json.loads((baseline / metadata_name).read_bytes())
    labels = (baseline / "labels.json").read_bytes()
    foreground = [(row, column)
                  for row, scanline in enumerate(clean["InstanceID"])
                  for column, instance in enumerate(scanline) if instance]
    observations, statistics, appearance_gains = {}, {}, {}
    for case, profile in cases.items():
        settings, directory = work / f"{case}.json", work / case
        write_json(settings, {**spec, **profile})
        capture(renderer, work, model, settings, directory, log_name=f"{case}.log")
        planes = read_exr(directory / image_name, include_ground_truth=True)
        metadata = json.loads((directory / metadata_name).read_bytes())
        for key in ("worldFromCamera", "intrinsics", "sampleIndex", "tick", "seconds"):
            assert metadata[key] == calibration[key], (case, key, metadata[key])
        assert (directory / "labels.json").read_bytes() == labels, case
        assert planes["InstanceID"] == clean["InstanceID"] and planes["SemanticID"] == clean["SemanticID"], case
        assert planes["GroundTruthZ"] == clean["Z"], case
        assert "GroundTruthZ" in metadata["channels"], case
        if "appearance" in profile:
            appearance = json.loads((directory / "dataset.json").read_bytes())["appearance"]
            assert appearance["algorithm"] == "splitmix64-v1", appearance
            for key, value in profile["appearance"].items():
                assert appearance[key] == metadata["appearance"][key] == value, (case, key, appearance)
            gains = appearance["directionalGain"], appearance["environmentGain"]
            variation = profile["appearance"].get("lightingVariation", 0)
            assert all(1 - variation <= gain <= 1 + variation for gain in gains), (case, gains)
            appearance_gains[case] = gains

        changed = sum(any(abs(planes[channel][row][column] - clean[channel][row][column]) > 1e-4
                          for channel in "RGB") for row, column in foreground)
        assert changed >= 100, (case, changed)
        dropped = 0
        for observed_row, truth_row in zip(planes["Z"], clean["Z"], strict=True):
            for observed, truth in zip(observed_row, truth_row, strict=True):
                assert observed in (0, truth), (case, observed, truth)
                dropped += truth > 0 and observed == 0
        if "depthNoise" in profile:
            assert 0.2 * len(foreground) < dropped < 0.4 * len(foreground), (case, dropped)
            assert metadata["depthDroppedPixelCount"] == dropped, (case, dropped)
        else:
            assert dropped == 0, (case, dropped)
        for effect in ("rgbNoise", "depthNoise"):
            for key, value in profile.get(effect, {}).items():
                assert metadata[effect][key] == value, (case, key, metadata[effect])
        noisy_rgb = "rgbNoise" in profile
        assert all(math.isfinite(value) and (not noisy_rgb or value >= 0)
                   for channel in "RGB" for row in planes[channel] for value in row), case
        observations[case] = planes
        statistics[case] = {"changedForegroundRgbPixels": changed, "droppedDepthPixels": dropped}

    assert hashes(work / "randomized") == hashes(work / "randomized-repeat"), "seeded capture changed on repeat"
    assert appearance_gains["lighting-only"] == appearance_gains["randomized"]
    assert appearance_gains["randomized"] != appearance_gains["randomized-alternate-seed"]
    first, alternate = observations["randomized"], observations["randomized-alternate-seed"]
    assert first["Z"] != alternate["Z"], "seed change left depth dropout unchanged"
    assert any(first[channel] != alternate[channel] for channel in "RGB"), "seed change left RGB unchanged"
    return statistics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_SENSOR_CAMERA") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_SENSOR_CAMERA=1 to run the BIM camera capture")
        return 77
    renderer, work = args.renderer.resolve(), args.output.resolve()
    work.mkdir(parents=True, exist_ok=args.managed_output)
    if args.managed_output:
        work = Path(tempfile.mkdtemp(prefix="run-", dir=work))
    print(f"BIM camera regression artifacts: {work}", flush=True)
    model, reordered = work / "camera-golden.ifc", work / "camera-reordered.ifc"
    fixture(model)
    fixture(reordered, reverse=True)
    static, moving, trajectory = (work / name for name in ("static.json", "moving.json", "trajectory.json"))
    spec = {"schema": "container.sensor-camera", "version": 1, "intrinsics": INTRINSICS,
            "baseFromCamera": {"xyz": MOUNT}}
    write_json(static, spec)
    write_json(moving, dict(spec, timing=SENSOR_TIMING))
    write_json(trajectory, {"schema": "container.bim-robotics-simulation", "version": 1,
                            "coordinateConvention": "ROS REP-103", "ticksPerSecond": TICK_RATE,
                            "history": [{"tick": tick, "seconds": tick / TICK_RATE,
                                         "base": {"position": base_pose(tick)[0], "yaw": base_pose(tick)[1]}}
                                        for tick in range(SENSOR_TIMING["lastTick"] + 1)]})
    if args.generate_only:
        return 0
    (work / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="ascii")
    initial, animated, repeat = (work / name for name in ("static", "moving", "reordered-and-scaled"))
    capture(renderer, work, model, static, initial, log_name="static.log")
    static_labels, initial_frames = verify_dataset(initial, (0,), 1)
    capture(renderer, work, model, moving, animated, trajectory=trajectory, log_name="moving.log")
    moving_labels, first = verify_dataset(animated, SENSOR_TICKS, 1)
    capture(renderer, work, reordered, moving, f"{repeat}{os.sep}", scale=2.5,
            trajectory=trajectory, log_name="reordered.log")
    reordered_labels, second = verify_dataset(repeat, SENSOR_TICKS, 2.5)
    assert static_labels == moving_labels == reordered_labels, (static_labels, moving_labels, reordered_labels)
    assert first[0][0]["InstanceID"] != first[-1][0]["InstanceID"], "moving camera did not change coverage"
    assert second[0][0]["InstanceID"] != second[-1][0]["InstanceID"], "scaled camera did not change coverage"

    before = hashes(animated)
    capture(renderer, work, reordered, moving, animated, scale=2.5, trajectory=trajectory,
            log_name="existing-output.log", failure="Sensor camera output directory already exists")
    assert hashes(animated) == before, "rejected capture wrote into an existing dataset"
    randomization = verify_randomization(renderer, work, model, spec, initial, initial_frames[0][0])
    write_json(work / "results.json", {"ticks": SENSOR_TICKS, "labels": moving_labels,
                                      "moving": [stats for _, stats in first],
                                      "reorderedAndScaled": [stats for _, stats in second],
                                      "randomization": randomization})
    print(f"PASS calibrated RGB/depth/labels, playback, and seeded randomization ({work})", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
