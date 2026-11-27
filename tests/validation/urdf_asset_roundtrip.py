#!/usr/bin/env python3
"""Verify URDF GLB motion and optionally render two native animation poses."""

import argparse
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


FIXTURES = Path(__file__).resolve().parents[1] / "fixtures/robotics"
# Column-major REP-103 to renderer rotation, with metres on both sides.
TO_RENDERER = ((0, 0, -1, 0), (-1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 0, 1))
IDENTITY = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def near(actual, wanted, label, tolerance=3e-5):
    require(len(actual) == len(wanted) and all(
        math.isclose(a, b, rel_tol=0., abs_tol=tolerance) for a, b in zip(actual, wanted)
    ), f"{label}: expected {wanted}, got {actual}")


def multiply(left, right):
    return tuple(tuple(sum(left[k][row] * column[k] for k in range(4)) for row in range(4))
                 for column in right)


def transformed_node(node, override):
    if "matrix" in node:
        require(not override, "The asset animates a node with a matrix transform")
        return tuple(tuple(node["matrix"][i:i + 4]) for i in range(0, 16, 4))
    x, y, z, w = override.get("rotation", node.get("rotation", (0, 0, 0, 1)))
    a, b, c = node.get("scale", (1, 1, 1))
    offset = override.get("translation", node.get("translation", (0, 0, 0)))
    return (
        (a * (1 - 2 * (y*y + z*z)), a * 2 * (x*y + z*w), a * 2 * (x*z - y*w), 0),
        (b * 2 * (x*y - z*w), b * (1 - 2 * (x*x + z*z)), b * 2 * (y*z + x*w), 0),
        (c * 2 * (x*z + y*w), c * 2 * (y*z - x*w), c * (1 - 2 * (x*x + y*y)), 0),
        (*offset, 1),
    )


def read_glb(path):
    raw = path.read_bytes()
    magic, version, length = struct.unpack_from("<4sII", raw)
    require((magic, version, length) == (b"glTF", 2, len(raw)), f"Invalid GLB header in {path}")
    json_length, json_kind = struct.unpack_from("<I4s", raw, 12)
    require(json_kind == b"JSON", "The first GLB chunk must contain JSON")
    binary_start = 20 + json_length
    binary_length, binary_kind = struct.unpack_from("<I4s", raw, binary_start)
    require(binary_kind == b"BIN\0" and binary_start + 8 + binary_length == length,
            "Expected a self-contained GLB binary chunk")
    return json.loads(raw[20:binary_start]), raw[binary_start + 8:]


def read_accessor(model, binary, index):
    accessor = model["accessors"][index]
    view = model["bufferViews"][accessor["bufferView"]]
    component = {5123: "H", 5125: "I", 5126: "f"}[accessor["componentType"]]
    count = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[accessor["type"]]
    layout = struct.Struct("<" + component * count)
    base = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    stride = view.get("byteStride", layout.size)
    return [layout.unpack_from(binary, base + i * stride) for i in range(accessor["count"])]


def animate_endpoints(model, binary, channel_count):
    require(len(model.get("animations", [])) == 1, "The animated robot needs one playback clip")
    animation = model["animations"][0]
    require(animation["name"] == "URDF joint motion", "The motion clip lost its name")
    require(len(animation["channels"]) == channel_count, "One scene lost its moving joint channels")
    first, last = {}, {}
    for channel in animation["channels"]:
        target = channel["target"]
        node, key = target["node"], target["path"]
        require(model["nodes"][node]["extras"]["urdf"]["role"] == "jointMotion",
                "An animation channel targets a visual or static origin")
        sampler = animation["samplers"][channel["sampler"]]
        times = [t[0] for t in read_accessor(model, binary, sampler["input"])]
        values = read_accessor(model, binary, sampler["output"])
        require(times[0] == 0. and len(times) == len(values) >= 2 and
                all(a < b for a, b in zip(times, times[1:])), "Invalid animation samples")
        near((times[-1],), (2.,), "URDF motion duration")
        if key == "rotation":
            for value in values:
                near((sum(x * x for x in value),), (1.,), "Unit motion quaternion")
            require(all(sum(x*y for x, y in zip(a, b)) >= 0. for a, b in zip(values, values[1:])),
                    "Quaternion samples change hemisphere between adjacent keys")
        first.setdefault(node, {})[key] = values[0]
        last.setdefault(node, {})[key] = values[-1]
    return first, last


def world_nodes(model, scene, overrides):
    def visit(index, parent):
        node = model["nodes"][index]
        world = multiply(parent, transformed_node(node, overrides.get(index, {})))
        yield index, node, world
        for child in node.get("children", []):
            yield from visit(child, world)

    return [row for root in model["scenes"][scene]["nodes"] for row in visit(root, IDENTITY)]


def verify_scene(model, scene, overrides, reference, role):
    require(model["scenes"][scene]["name"] == role.title(), "Wrong active robot view")
    links = {link["name"]: link for link in reference["links"]}
    joints = {joint["name"]: joint for joint in reference["joints"]}
    actual_links, actual_shapes = set(), set()
    for _, node, world in world_nodes(model, scene, overrides):
        metadata = node["extras"]["urdf"]
        name = metadata.get("name")
        if metadata["role"] == "link":
            actual_links.add(name)
            expected = multiply(TO_RENDERER, links[name]["worldFromLink"])
        elif metadata["role"] == role:
            owner, index = metadata["link"], metadata["index"]
            actual_shapes.add((owner, index))
            shape = links[owner][role + "s"][index]
            require(name == shape["name"] and "mesh" in node, f"{name} lost its URDF mesh node")
            expected = multiply(TO_RENDERER, shape["worldFrom" + role.title()])
            if shape["geometry"]["type"] == "mesh":
                expected = tuple(tuple(s * x for x in column)
                                 for s, column in zip((*shape["geometry"]["scale"], 1), expected))
        elif metadata["role"] in ("jointOrigin", "jointMotion"):
            near(metadata["axis"], joints[name]["axis"], f"Authored joint axis {name}")
            continue
        else:
            require(metadata["role"] == "frame", "Unexpected node role in the URDF scene")
            continue
        near(tuple(x for column in world for x in column), tuple(x for column in expected for x in column),
             f"{role} world matrix for {name}")
    require(actual_links == links.keys(), f"The {role} scene lost a link")
    expected_shapes = {(key, i) for key, link in links.items() for i in range(len(link[role + "s"]))}
    require(actual_shapes == expected_shapes, f"The {role} scene lost a shape")


def execute(command, *, cwd=None, env=None, timeout=120):
    return subprocess.run([str(x) for x in command], cwd=cwd, env=env, capture_output=True,
                          text=True, encoding="utf-8", errors="replace", timeout=timeout)


def export(exporter, source, output, *args):
    process = execute([exporter, source, output, *args])
    require(process.returncode == 0,
            f"URDF export failed ({process.returncode}) for {source}:\n{process.stdout}{process.stderr}")


def record_poses(exporter, source, scenario, folder, name, packages=()):
    initial_path = folder / f"{name}-initial.json"
    final_path = folder / f"{name}-final.json"
    asset = folder / f"{name}.glb"
    export(exporter, source, initial_path)
    export(exporter, source, final_path, scenario)
    export(exporter, source, asset, scenario, *packages)
    return asset, json.loads(initial_path.read_text()), json.loads(final_path.read_text())


def verify_exported_motion(asset, initial, final, channel_count):
    model, data = read_glb(asset)
    require(model["scene"] == 0 and len(model["scenes"]) == 2, "Expected visual and collision scenes")
    first, last = animate_endpoints(model, data, channel_count)
    for scene, role in enumerate(("visual", "collision")):
        verify_scene(model, scene, first, initial, role)
        verify_scene(model, scene, last, final, role)
    a = {i for i, _, _ in world_nodes(model, 0, {})}
    b = {i for i, _, _ in world_nodes(model, 1, {})}
    require(a.isdisjoint(b), "Visual and collision views share transform parents")
    return model, data, first, last


def verify_continuous_turn(exporter, folder):
    # A binary header may start with "solid". The fast joint moves more than
    # pi between ticks, so two unsplit rotation keys would lose complete turns.
    binary_mesh = folder / "rotor.stl"
    points = (0., 0., 0., .2, 0., 0., 0., .3, 0.)
    binary_mesh.write_bytes(b"solid binary rotor".ljust(80, b" ") + struct.pack("<I12fH", 1, 0., 0., 0., *points, 0))
    source = folder / "rotor.urdf"
    source.write_text('''<robot name="rotor"><link name="base"/>
<link name="shaft"><visual name="rotor_mesh"><geometry><mesh filename="rotor.stl"/></geometry></visual></link>
<joint name="spin" type="continuous"><parent link="base"/><child link="shaft"/>
<axis xyz="0 0 1"/><limit effort="20" velocity="500"/></joint></robot>''', encoding="utf-8")
    scenario = folder / "spin-motion.json"
    scenario.write_text(json.dumps({"version": 1, "targets": {"spin": 4 * math.pi}, "ticks": 2}))
    asset = folder / "rotor.glb"
    export(exporter, source, asset, scenario)
    model, data = read_glb(asset)
    require(len(model["scenes"]) == 1 and len(model["animations"][0]["channels"]) == 1,
            "The visual-only rotor needs one animated scene")
    sampler = model["animations"][0]["samplers"][0]
    quaternions = read_accessor(model, data, sampler["output"])
    require(len(quaternions) > 3, "The export did not split long angular steps")
    angles = []
    for previous, current in zip(quaternions, quaternions[1:]):
        sine = previous[3] * current[2] - previous[2] * current[3]
        cosine = sum(a * b for a, b in zip(previous, current))
        angles.append(2 * math.atan2(sine, cosine))
    require(all(0 < angle <= math.pi / 2 + 1e-5 for angle in angles), "The rotor lost forward motion")
    near((sum(angles),), (4 * math.pi,), "Preserved two complete turns")
    geometry = model["meshes"][0]["primitives"][0]
    vertices = read_accessor(model, data, geometry["attributes"]["POSITION"])
    near(tuple(value for point in vertices for value in point), points, "Binary STL preserved local coordinates")

    source.write_text(source.read_text(encoding="utf-8").replace("visual", "collision"), encoding="utf-8")
    asset = folder / "rotor-collision-only.glb"
    export(exporter, source, asset, scenario)
    collision_model, _ = read_glb(asset)
    require(collision_model["scenes"][collision_model["scene"]]["name"] == "Collision",
            "A collision-only URDF should open with visible geometry")


def verify_assets(exporter, folder):
    arm = FIXTURES / "two-link-arm.urdf"
    motion = FIXTURES / "two-link-arm-pose.json"
    asset, initial, final = record_poses(exporter, arm, motion, folder, "arm")
    model, data, first, last = verify_exported_motion(asset, initial, final, 4)
    for keyframes, tip in ((first, (-.85, .25, -.05)), (last, (.25, .7, .5))):
        position = next(world[3][:3] for _, node, world in world_nodes(model, 0, keyframes)
                        if node["extras"]["urdf"].get("name") == "tip_marker")
        near(position, tip, "Independent two-link tip witness")
    repeated = folder / "arm-repeat.glb"
    export(exporter, arm, repeated, motion)
    require(asset.read_bytes() == repeated.read_bytes(), "Repeated URDF motion exports differ")

    slider = FIXTURES / "mesh-slider.urdf"
    slider_motion = folder / "slider-motion.json"
    slider_motion.write_text(json.dumps({"version": 1, "targets": {"slide": .35}, "ticks": 120}))
    slider_asset, slider_initial, slider_final = record_poses(
        exporter, slider, slider_motion, folder, "slider", ("--package-root", FIXTURES))
    slider_model, slider_data, _, slider_last = verify_exported_motion(slider_asset, slider_initial, slider_final, 2)
    _, wedge, world = next(row for row in world_nodes(slider_model, 0, slider_last)
                           if row[1]["extras"]["urdf"].get("name") == "mirrored_wedge")
    primitive = slider_model["meshes"][wedge["mesh"]]["primitives"][0]
    positions = read_accessor(slider_model, slider_data, primitive["attributes"]["POSITION"])
    for i, expected in enumerate((.35, .2, .1)):
        near((min(p[i] for p in positions), max(p[i] for p in positions)), (0., expected), "Authored STL bounds")
    near(world[3][:3], (-.75, .45, -.2), "Prismatic link and separate mesh origin")
    color = slider_model["materials"][primitive["material"]]["pbrMetallicRoughness"]["baseColorFactor"]
    near(color, (.15, .35, .8, 1.), "URDF material color")

    existing = slider_asset.read_bytes()
    missing = execute([exporter, slider, slider_asset, slider_motion])
    require(missing.returncode != 0 and "package" in missing.stderr.lower() and
            slider_asset.read_bytes() == existing, "Missing package did not preserve the previous export")
    verify_continuous_turn(exporter, folder)
    return asset, model, first, last


def verify_native(renderer, folder, asset, model, first, last):
    from PIL import Image, ImageChops

    (folder / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    images = []
    for label, time, keyframes in (("before", 0., first), ("after", 1.95, last)):
        image = folder / f"native-{label}.png"
        command = [renderer, "--model", asset, "--hidden", "--no-ui", "--validation", "--width", "640",
                   "--height", "360", "--msaa", "1", "--no-taa", "--no-bloom", "--warmup-frames", "3",
                   "--capture-frame", "4", "--camera-position", "1.9", "1.55", "2.25", "--camera-target",
                   "0", ".36", "0", "--camera-fov", "39", "--render-technique", "forward-raster",
                   "--animation-clip", "0", "--animation-paused", "--animation-time", str(time),
                   "--display-mode", "lit", "--directional-intensity", "3.5", "--screenshot-viewport",
                   "--screenshot", image]
        result = execute(command, cwd=renderer.parent,
                         env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(folder)), timeout=180)
        log = folder / f"native-{label}.log"
        log.write_text(result.stdout + result.stderr, encoding="utf-8")
        require(result.returncode == 0 and not re.search(r"VUID-|SYNC-HAZARD|Validation Error",
                                                         result.stdout + result.stderr, re.I),
                f"Renderer failed or reported Vulkan validation errors: {log}")
        telemetry = json.loads(image.with_suffix(".telemetry.json").read_text())
        animation = telemetry["gltfAnimation"]
        require(animation["available"] and animation["selectedClip"] == 0 and
                not animation["diagnostics"] and not animation["playing"], "Renderer rejected the URDF animation")
        near((animation["time"],), (time,), "Renderer paused animation time")
        by_index = {node["sourceIndex"]: node for node in animation["nodes"]}
        for index, node, world in world_nodes(model, 0, keyframes):
            if node["extras"]["urdf"]["role"] == "jointMotion" and index in keyframes:
                require(index in by_index, f"Renderer did not evaluate URDF node {node['name']}")
                near(by_index[index]["worldMatrix"], tuple(x for column in world for x in column),
                     f"Native matrix for {node['name']}")
        require(telemetry["scene"]["primaryObjectCount"] >= 4, "Expected all four rendered URDF visuals")
        with Image.open(image) as opened:
            images.append(opened.convert("RGB"))
    difference = ImageChops.difference(*images)
    red, green, blue = difference.split()
    histogram = ImageChops.lighter(ImageChops.lighter(red, green), blue).histogram()
    changed = sum(histogram[10:]) / (640 * 360)
    require(changed > .005, f"Robot animation did not produce visibly different frames: {changed:.2%}")
    print(f"Renderer verified URDF poses and {changed:.2%} changed pixels")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exporter", required=True, type=Path)
    parser.add_argument("--renderer", type=Path, help="Also run the Vulkan animation capture")
    parser.add_argument("--output-dir", type=Path, help="Retain evidence in a new directory")
    args = parser.parse_args()
    if args.renderer:
        try:
            import PIL  # noqa: F401
        except ImportError:
            print("SKIP: Native URDF captures require Pillow", file=sys.stderr)
            return 77

    def check(folder):
        evidence = verify_assets(args.exporter.resolve(strict=True), folder)
        if args.renderer:
            verify_native(args.renderer.resolve(strict=True), folder, *evidence)
        print("URDF asset round-trip passed")

    if args.output_dir:
        folder = args.output_dir.resolve()
        folder.mkdir(parents=True, exist_ok=False)
        check(folder)
    else:
        with tempfile.TemporaryDirectory(prefix="urdf-asset-roundtrip-") as folder:
            check(Path(folder))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.SubprocessError, ValueError) as error:
        print(f"URDF asset round-trip: {error}", file=sys.stderr)
        sys.exit(1)
