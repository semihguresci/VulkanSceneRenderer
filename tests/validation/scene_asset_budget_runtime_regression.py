"""Exercise live asset budget admission and retirement in the Vulkan viewport."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from display_capture_test_support import Asset, validation_log_error


OPT_IN = "CONTAINER_RUN_GPU_SCENE_ASSET_BUDGET"
CAP_MIB = 2
CAP_BYTES = CAP_MIB * 1024 * 1024
WIDTH, HEIGHT = 640, 400
GRID_SIZE = 200
CAMERA_POSITION = [3.3, 2.4, 6.0]
CAMERA_TARGET = [0.0, .8, 0.0]


@dataclass(frozen=True)
class Step:
    label: str
    active: Path | None
    incoming: Path | None = None
    rejected: bool = False
    gui_unload: bool = False
    empty: bool = False


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_json(path, document):
    path.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def panel_grid(size):
    # Independent unique positions avoid mistaking a large instanced source for
    # a large GPU vertex upload. 40k packed renderer vertices exceed 2 MiB.
    positions = [(3 * x / (size - 1) - 1.5, 2 * y / (size - 1), 0.)
                 for y in range(size) for x in range(size)]
    indices = []
    for y in range(size - 1):
        for x in range(size - 1):
            first = y * size + x
            indices.extend((first, first + 1, first + size + 1,
                            first, first + size + 1, first + size))
    return positions, indices


def gltf_panel(path, positions, indices, color, texture_name=None):
    asset = Asset()
    attributes = {
        "POSITION": asset.accessor("VEC3", positions),
        "NORMAL": asset.accessor("VEC3", [(0., 0., 1.)] * len(positions)),
    }
    material = dict(pbrMetallicRoughness=dict(baseColorFactor=[*color, 1.], metallicFactor=0.),
                    emissiveFactor=list(color))
    if texture_name is not None:
        attributes["TEXCOORD_0"] = asset.accessor("VEC2", [(0., 0.), (1., 0.), (0., 1.), (1., 1.)])
        material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
        material["emissiveTexture"] = {"index": 0}
        asset.model["images"] = [{"uri": texture_name}]
        asset.model["textures"] = [{"source": 0}]
    asset.model["meshes"] = [dict(primitives=[dict(
        attributes=attributes, material=0,
        indices=asset.accessor("SCALAR", indices, 5123, indices=True))])]
    asset.model["materials"] = [material]
    asset.write(path)


def oversized_fixtures(output):
    from PIL import Image, ImageDraw

    gltf, bim, texture = (output / name for name in
                          ("oversize-geometry.gltf", "oversize-geometry.bim", "oversize-texture.gltf"))
    positions, indices = panel_grid(GRID_SIZE)
    gltf_panel(gltf, positions, indices, (.18, .02, .55))
    # DotBIM coordinates are Z-up. Convert the same panel to its source axes.
    write_json(bim, {"schema_version": "1.1.0",
        "meshes": [{"mesh_id": 1, "coordinates": [c for x, y, z in positions for c in (x, -z, y)],
                    "indices": indices}],
        "elements": [{"mesh_id": 1, "guid": "budget-bim-panel", "type": "IfcBuildingElementProxy",
                       "color": {"r": 66, "g": 207, "b": 103, "a": 255}}]})
    pixels = Image.new("RGBA", (1024, 1024), (18, 178, 228, 255))
    ImageDraw.Draw(pixels).rectangle((0, 0, 511, 511), fill=(232, 83, 31, 255))
    pixels.save(output / "oversize-texture.png")
    positions, indices = panel_grid(2)
    gltf_panel(texture, positions, indices, (.7, .7, .7), "oversize-texture.png")
    return gltf, bim, texture


def capture_case(renderer, output, name, steps, *, flags=(), gui=False):
    from PIL import Image

    output.mkdir()
    base = output / name
    last_frame = 4 * len(steps)
    frames = list(range(4, last_frame + 1, 4))
    events = []
    for frame, step in zip(frames, steps, strict=True):
        if step.gui_unload:
            events.append({"frame": frame - 3, "guiUnload": True})
        elif step.incoming is not None:
            events.append({"frame": frame - 3, "reload": str(step.incoming)})
    sequence = base.with_suffix(".sequence.json")
    write_json(sequence, dict(schemaVersion=1, frames=last_frame, sampleFrames=frames, events=events,
        camera=[dict(frame=frame, position=CAMERA_POSITION, target=CAMERA_TARGET)
                for frame in (1, last_frame)]))
    settings = output / "vk_layer_settings.txt"
    settings.write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    command = [
        str(renderer), "--hidden", "--validation", "--screenshot-viewport",
        "--model", str(steps[0].active), "--import-scale", "1",
        "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1",
        "--render-technique", "deferred-raster", "--display-mode", "lit",
        "--no-taa", "--no-bloom", "--no-ddgi", "--ray-shadows", "hard", "--no-ray-shadow-denoise",
        "--camera-position", *map(str, CAMERA_POSITION), "--camera-target", *map(str, CAMERA_TARGET),
        "--camera-fov", "42", "--exposure", ".8", "--tone-map", "neutral",
        "--sky", "plain", "--sky-horizon", ".57", ".64", ".72", "--environment-intensity", ".15",
        "--directional-intensity", "2.5", "--fixed-dt", ".016666667",
        "--capture-sequence", str(sequence), "--screenshot", str(base.with_suffix(".png")), *flags,
    ]
    if not gui:
        command.append("--no-ui")
    log = base.with_suffix(".log")
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer")
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, env=env, stdout=stream,
            stderr=subprocess.STDOUT, timeout=300,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    messages = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(messages),
            f"Vulkan capture or synchronization validation failed. See {log}")
    loader_lines = [line for line in messages.splitlines()
                    if 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in line]
    require(loader_lines, f"Loader did not confirm insertion of the Khronos validation layer. See {log}")

    samples = {}
    for frame, step in zip(frames, steps, strict=True):
        stem = base if frame == last_frame else base.with_name(f"{base.name}.frame-{frame:04d}")
        png = Path(f"{stem}.png")
        telemetry_path = Path(f"{stem}.telemetry.json")
        sidecar = Path(f"{png}.capture.json")
        state = json.loads(telemetry_path.read_bytes())
        metadata = json.loads(sidecar.read_bytes())
        require(state["validationEnabled"] and not metadata["includesUi"] and metadata["source"] == "viewport",
                f"{png} is not a validated viewport capture")
        require(state["technique"] == "deferred-raster" and
                metadata["build"] == state["build"] and metadata["sceneRevisions"] == state["sceneRevisions"],
                f"{png} does not match the captured renderer state")
        with Image.open(png) as pixels:
            require(pixels.format == "PNG" and pixels.size == (WIDTH, HEIGHT), f"Invalid image {png}")
            rgb = pixels.convert("RGB")
            require(step.empty or max(upper - lower for lower, upper in rgb.getextrema()) > 20,
                    f"{png} has no visible model")
            pixel_digest = hashlib.sha256(rgb.tobytes()).hexdigest()
        samples[step.label] = dict(frame=frame, image=str(png), imageSha256=digest(png),
            pixelSha256=pixel_digest, telemetry=str(telemetry_path), sidecar=str(sidecar), state=state)
    return dict(command=command, log=str(log), layer=loader_lines, samples=samples), messages


def verify_case(case, steps, *, resident_cap=0, upload_cap=0, rays=True):
    limits = {"residentBytes": resident_cap, "uploadBytes": upload_cap}
    identities = set()
    previous, previous_generation = None, -1
    for step in steps:
        sample = case["samples"][step.label]
        state = sample["state"]
        budget, scene, ray = (state[key] for key in ("assetResidency", "scene", "rayShadows"))
        used, peak = budget["used"], budget["peak"]
        require(budget["limits"] == limits, f"{step.label} used the wrong configured limits")
        for category, limit in limits.items():
            require(0 <= used[category] <= peak[category] and (not limit or peak[category] <= limit),
                    f"{step.label} exceeded {category} or lost its usage accounting")
        require(used["uploadBytes"] == 0 and not budget["preparing"],
                f"{step.label} did not retire its completed preparation and staging")
        paths = [Path(path).resolve() for path in (scene["primary"], scene["auxiliary"]) if path]
        require(paths == ([step.active] if step.active is not None else []),
                f"{step.label} published an unexpected active model: {paths}")
        require(ray["supported"] is rays, f"{step.label} ran with the wrong ray capabilities")
        built = ray.get("build", {})
        if step.empty or not rays:
            require(not ray["active"] and built.get("instances", 0) == 0 and built.get("asAllocatedBytes", 0) == 0,
                    f"{step.label} kept an acceleration structure alive without usable geometry")
        else:
            require(ray["active"] and built["instances"] > 0 and built["asAllocatedBytes"] > 0,
                    f"{step.label} did not build a usable acceleration structure")
        textures = budget["textures"]
        texture_ids = {texture["id"] for texture in textures}
        require(len(texture_ids) == len(textures) and all(
            type(texture["id"]) is int and texture["id"] > 0 and
            texture["revision"] > 0 and 0 < texture["residentBytes"] <= used["residentBytes"] and
            texture["residency"] == "resident" for texture in textures),
            f"{step.label} reported invalid texture IDs or residency")
        if step.active is not None and step.active.name in ("taa_equivalent.gltf", "oversize-texture.gltf"):
            require(textures, f"{step.label} did not publish its authored glTF image")
        if step.empty:
            prior_budget = previous["state"]["assetResidency"]
            remaining = {provider["geometryAssetId"] for provider in budget["providers"]}
            retired = {provider["geometryAssetId"] for provider in prior_budget["providers"]}
            require(not (remaining & retired) and (not step.gui_unload or not remaining) and
                    used["residentBytes"] < prior_budget["used"]["residentBytes"],
                    f"{step.label} retained the unloaded provider or its owned allocations")
        else:
            require(len(budget["providers"]) == 1, f"{step.label} did not publish exactly one provider")
            provider = budget["providers"][0]
            identity = provider["geometryAssetId"]
            require(isinstance(provider["id"], str) and bool(provider["id"]) and
                    type(identity) is int and identity > 0 and
                    type(provider["geometryRevision"]) is int and provider["geometryRevision"] > 0 and
                    provider["residency"] == "resident" and identity not in texture_ids,
                    f"{step.label} has invalid scene asset identity metadata")
            if not step.rejected:
                require(identity not in identities, f"{step.label} reused a retired geometry asset ID")
                identities.add(identity)
                if rays:
                    require(ray["generation"] > previous_generation,
                            f"{step.label} reused the previous provider's ray generation")
        if previous is not None:
            prior = previous["state"]
            prior_budget = prior["assetResidency"]
            expected_rejections = prior_budget["rejectedReservations"] + int(step.rejected)
            require(budget["rejectedReservations"] == expected_rejections,
                    f"{step.label} did not record the expected admission decision")
            if step.rejected:
                require(budget["providers"] == prior_budget["providers"] and
                        textures == prior_budget["textures"] and
                        used == prior_budget["used"] and
                        state["sceneRevisions"] == prior["sceneRevisions"] and
                        ray["generation"] == prior["rayShadows"]["generation"] and
                        sample["pixelSha256"] == previous["pixelSha256"],
                        f"{step.label} changed a live renderer resource, revision, or viewport")
            require(state["build"] == prior["build"], f"{step.label} changed the renderer build")
        else:
            require(budget["rejectedReservations"] == 0, f"{step.label} rejected its initial scene")
        if not step.rejected:
            require(not (texture_ids & identities), f"{step.label} reused a retired image asset ID")
            identities.update(texture_ids)
        previous, previous_generation = sample, ray["generation"]


def capped_steps(gltf, bim, usd, oversized, *, empty=None):
    large_gltf, large_bim, texture = oversized
    return [
        Step("initial-gltf", gltf),
        Step("rejected-bim-geometry", gltf, large_bim, rejected=True),
        Step("accepted-bim", bim, bim),
        Step("rejected-gltf-geometry", bim, large_gltf, rejected=True),
        Step("rejected-gltf-texture", bim, texture, rejected=True),
        Step("accepted-usd", usd, usd),
        Step("accepted-gltf", gltf, gltf),
        Step("same-gltf-reloaded", gltf, gltf),
        Step("bim-before-retirement", bim, bim),
        Step("retired", empty, empty, gui_unload=empty is None, empty=True),
    ]


def comparison_image(path, rows):
    from PIL import Image, ImageDraw

    panel_width, panel_height, label_height = WIDTH // 2, HEIGHT // 2, 20
    canvas = Image.new("RGB", (3 * panel_width, len(rows) * (panel_height + label_height)), "#151b21")
    draw = ImageDraw.Draw(canvas)
    for row_index, row in enumerate(rows):
        for column, (title, sample) in enumerate(row):
            x, y = column * panel_width, row_index * (panel_height + label_height)
            draw.text((x + 5, y + 3), title, fill="white")
            with Image.open(sample["image"]) as picture:
                canvas.paste(picture.convert("RGB").resize((panel_width, panel_height), Image.Resampling.LANCZOS),
                             (x, y + label_height))
    canvas.save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--require-opt-in", action="store_true")
    args = parser.parse_args()
    if args.require_opt_in and os.environ.get(OPT_IN) != "1":
        print(f"SKIP: set {OPT_IN}=1 to run Vulkan asset budget captures")
        return 77

    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True, exist_ok=False)
    repo_models = Path(__file__).resolve().parents[2] / "models" / "validation"
    gltf, bim, usd = (repo_models / f"taa_equivalent.{ext}" for ext in ("gltf", "bim", "usda"))
    for path in (gltf, bim, usd):
        require(path.is_file(), f"Missing shared validation scene {path}")
    empty = output / "empty.gltf"
    positions, indices = panel_grid(2)
    gltf_panel(empty, positions, indices, (.2, .2, .2))
    empty_asset = json.loads(empty.read_bytes())
    empty_asset["scenes"][0]["nodes"] = []
    write_json(empty, empty_asset)
    oversized = oversized_fixtures(output)
    results = dict(passed=False, rendererSha256=digest(renderer), capBytes=CAP_BYTES, cases={})
    checksum = results["rendererSha256"]
    try:
        for name, empty_scene in (("memory", empty), ("upload", None)):
            steps = capped_steps(gltf, bim, usd, oversized, empty=empty_scene)
            case, messages = capture_case(renderer, output / name, name, steps,
                flags=(f"--asset-{name}-mb", str(CAP_MIB)), gui=empty_scene is None)
            results["cases"][name] = case
            verify_case(case, steps, resident_cap=CAP_BYTES if name == "memory" else 0,
                        upload_cap=CAP_BYTES if name == "upload" else 0)
            require(len(re.findall(fr"Scene asset {name} budget exceeded", messages)) >= 3,
                    f"{name} run did not diagnose all budget rejections in its native log")

        large_gltf, large_bim, texture = oversized
        controls = [Step("initial-gltf", gltf), Step("accepted-large-bim", large_bim, large_bim),
                    Step("accepted-large-gltf", large_gltf, large_gltf),
                    Step("accepted-large-texture", texture, texture)]
        control, _ = capture_case(renderer, output / "uncapped", "uncapped", controls,
                                  flags=("--no-ray-query",))
        results["cases"]["uncapped"] = control
        verify_case(control, controls, rays=False)
        for step in controls[1:]:
            sample = control["samples"][step.label]
            require(sample["state"]["assetResidency"]["used"]["residentBytes"] > CAP_BYTES and
                    sample["pixelSha256"] != control["samples"]["initial-gltf"]["pixelSha256"],
                    f"The uncapped renderer did not visibly load the oversized control {step.label}")
        require(control["samples"]["accepted-large-bim"]["state"]["assetResidency"]["peak"]["uploadBytes"] >
                CAP_BYTES, "Uncapped geometry did not exercise an oversized staging upload")
        large_gltf_state = control["samples"]["accepted-large-gltf"]["state"]
        require(large_gltf_state["scene"]["primaryVertexCount"] >= GRID_SIZE * GRID_SIZE,
                "Uncapped glTF did not upload the large source geometry")
        large_image_state = control["samples"]["accepted-large-texture"]["state"]
        require(any(texture["residentBytes"] > CAP_BYTES
                    for texture in large_image_state["assetResidency"]["textures"]),
                "Uncapped glTF did not allocate its oversized image")

        memory = results["cases"]["memory"]["samples"]
        upload = results["cases"]["upload"]["samples"]
        uncapped = control["samples"]
        comparison = output / "budget-comparison.png"
        comparison_image(comparison, [
            [("Before memory rejection", memory["initial-gltf"]),
             ("After memory rejection", memory["rejected-bim-geometry"]),
             ("Uncapped BIM load", uncapped["accepted-large-bim"])],
            [("Before texture rejection", upload["accepted-bim"]),
             ("After upload rejection", upload["rejected-gltf-texture"]),
             ("Uncapped textured glTF load", uncapped["accepted-large-texture"])],
        ])
        require(digest(renderer) == checksum, "Renderer binary changed during validation")
        results.update(passed=True, comparison=str(comparison),
                       captures=sum(len(case["samples"]) for case in results["cases"].values()))
    finally:
        write_json(output / "results.json", results)
    print(f"PASS {results['captures']} Vulkan scene asset budget captures: {output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
