"""Exercise authored mip residency, texture eviction and typed sampling in Vulkan."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import struct
import subprocess
import tempfile

from display_capture_test_support import Asset, validation_log_error


OPT_IN = "CONTAINER_RUN_GPU_TEXTURE_STREAMING"
TECHNIQUES = ("deferred-raster", "forward-raster")
WIDTH, HEIGHT, FOV = 1280, 720, 42
TEXTURE_SIZE = 1024
FALLBACK_SIZE = 128
RESIDENT_MIB = 7
UPLOAD_KIB = 256
BYTES_PER_MIB = 1024 * 1024
PALETTE = (((194, 52, 32), (38, 186, 214)), ((130, 55, 216), (55, 207, 31)))
BACK = (.02, .06, .55)
NEAR, FAR = .90, 32.


@dataclass(frozen=True)
class Panel:
    name: str
    texture: int
    x: float
    y: float
    width: float
    height: float
    z: float = 0.


MAIN = (Panel("large-a", 0, -8.22, 0., 1.02, .98), Panel("large-b", 1, 7.78, 0., 1.02, .98))
WITNESS = (Panel("witness-b", 1, -7.54, .058, .105, .11), Panel("witness-a", 0, 8.46, .058, .105, .11))


@dataclass(frozen=True)
class Checkpoint:
    phase: str
    frame: int
    probe: Panel
    panels: tuple[Panel, ...]
    eye: tuple[float, float, float]
    probe_u: float = .5
    expected_pick: str | None = None

    def pixel(self, panel, u=.5):
        x, y, distance = self.eye
        focal = HEIGHT / (2 * math.tan(math.radians(FOV / 2)))
        scale = focal / (distance - panel.z)
        return (int(WIDTH / 2 + scale * (panel.x + (u - .5) * panel.width - x)),
                int(HEIGHT / 2 - scale * (panel.y - y)))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def color_material(name, *, texture=None, color=(1., 1., 1.), mask=False):
    pbr = dict(baseColorFactor=[*color, 1.], metallicFactor=0., roughnessFactor=1.)
    if texture is not None:
        pbr["baseColorTexture"] = dict(index=texture)
    result = dict(name=name, pbrMetallicRoughness=pbr, extensions=dict(KHR_materials_unlit={}))
    if mask:
        result.update(alphaMode="MASK", alphaCutoff=.30)
    return result


def make_asset(panels, materials, images=(), *, dds=False, uv=(.25, .75)):
    asset = Asset()
    asset.model["nodes"] = []
    asset.model["scenes"] = [dict(nodes=list(range(len(panels))))]
    asset.model["materials"] = materials
    unlit = any("KHR_materials_unlit" in material.get("extensions", {}) for material in materials)
    if unlit:
        asset.model["extensionsUsed"] = ["KHR_materials_unlit"]
    if images:
        asset.model["images"] = list(images)
        asset.model["samplers"] = [dict(minFilter=9984, magFilter=9728, wrapS=33071, wrapT=33071)]
        if dds:
            asset.model.setdefault("extensionsUsed", []).append("MSFT_texture_dds")
            asset.model["textures"] = [dict(source=0, sampler=0, extensions=dict(MSFT_texture_dds=dict(source=1)))]
        else:
            asset.model["textures"] = [dict(source=i, sampler=0) for i in range(len(images))]
    indices = asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True)
    normal = asset.accessor("VEC3", [(0., 0., 1.)] * 4)
    lo, hi = uv
    texcoords = asset.accessor("VEC2", [(lo, hi), (hi, hi), (hi, lo), (lo, lo)])
    for index, panel in enumerate(panels):
        left, right = panel.x - panel.width / 2, panel.x + panel.width / 2
        bottom, top = panel.y - panel.height / 2, panel.y + panel.height / 2
        points = [(left, bottom, panel.z), (right, bottom, panel.z),
                  (right, top, panel.z), (left, top, panel.z)]
        attributes = dict(POSITION=asset.accessor("VEC3", points), NORMAL=normal, TEXCOORD_0=texcoords)
        asset.model["nodes"].append(dict(name=panel.name, mesh=index))
        asset.model["meshes"].append(dict(primitives=[dict(attributes=attributes, indices=indices,
                                                           material=panel.texture)]))
    return asset


def rgba_levels(size, colors):
    import numpy as np

    levels = []
    for level in range(size.bit_length()):
        width = max(1, size >> level)
        color = colors[0] if level < 2 else colors[1]
        levels.append(np.tile([*color, 255], (width, width, 1)).astype(np.uint8))
    return levels


def write_pressure_fixture(directory):
    from gltf_texture_formats_runtime_regression import ktx2_bytes

    images = []
    for index, colors in enumerate(PALETTE):
        path = directory / f"stream-{'a' if index == 0 else 'b'}.ktx2"
        path.write_bytes(ktx2_bytes(rgba_levels(TEXTURE_SIZE, colors)))
        images.append(dict(name=path.stem, uri=path.name, mimeType="image/ktx2"))
    scene = directory / "texture-pressure.gltf"
    make_asset((*MAIN, *WITNESS), [color_material(f"mip panel {index}", texture=index) for index in range(2)],
               images).write(scene)
    return scene


def write_sequence(path, checkpoints, cuts):
    cameras = []
    for first, last, eye in cuts:
        cameras.extend(dict(frame=frame, position=eye, target=[eye[0], eye[1], 0.]) for frame in (first, last))
    events = [dict(frame=2, viewportPointer=dict(action="select", pixel=[WIDTH - 2, 2]))]
    for sample in checkpoints:
        events.append(dict(frame=sample.frame, depthProbe=list(sample.pixel(sample.probe, sample.probe_u))))
    write_json(path, dict(schemaVersion=1, frames=checkpoints[-1].frame, sampleFrames=[item.frame for item in checkpoints],
                          camera=cameras, events=events))


def pressure_sweep(path):
    far_eye = (0., 0., FAR)
    eye_a, eye_b = (-8., 0., NEAR), (8., 0., NEAR)
    close_a, close_b = (MAIN[0], WITNESS[0]), (MAIN[1], WITNESS[1])
    # Two close frames can submit a copy but cannot finish a 1024² image at
    # 256 KiB per tick. Returning immediately also exercises fence retirement.
    cuts = [(1, 30, far_eye), (31, 32, eye_a), (33, 34, far_eye), (35, 104, eye_a),
            (105, 178, eye_b), (179, 252, eye_a), (253, 288, far_eye)]
    groups = (("far", MAIN), ("early-close-a", close_a), ("interrupted-far", MAIN), ("close-a", close_a),
              ("close-b", close_b), ("return-a", close_a), ("return-far", MAIN))
    checkpoints = []
    for (phase, panels), (first, last, eye) in zip(groups, cuts, strict=True):
        offsets = (last - first - 1, last - first)
        if panels != MAIN and last - first >= 50:
            offsets = (0, 1, 2, 4, 8, 16, 32, 48, *offsets)
        for index, offset in enumerate(offsets):
            checkpoints.append(Checkpoint(phase, first + offset, panels[index % 2], panels, eye))
    write_sequence(path, checkpoints, cuts)
    return checkpoints


def rejected_reload_sequence(path):
    from scene_asset_budget_runtime_regression import panel_grid

    invalid_gltf = path.parent / "rejected.gltf"
    invalid_bim = path.parent / "rejected.bim"
    for candidate in (invalid_gltf, invalid_bim):
        candidate.write_text("{ invalid JSON", encoding="utf-8")
    # Force full-resident images on both candidates. Two images cannot fit even
    # after retirement. One image fits only after the live detail can retire.
    images = [dict(uri=f"stream-{color}.ktx2", mimeType="image/ktx2") for color in ("a", "b")]
    over_budget = path.parent / "oversized.gltf"
    oversized = make_asset(MAIN, [color_material("a", texture=0), color_material("b", texture=1)], images)
    oversized.model["samplers"][0]["minFilter"] = 9728
    oversized.write(over_budget)
    accepted = path.parent / "admitted.gltf"
    panel = MAIN[0]
    replacement = Panel("admitted panel", 0, panel.x, panel.y, panel.width, panel.height)
    fitting = make_asset((replacement,), [color_material("replacement", texture=0)], images[1:])
    fitting.model["samplers"][0]["minFilter"] = 9728
    fitting.write(accepted)
    rejected_upload = path.parent / "oversized-upload.bim"
    points, indices = panel_grid(160)
    write_json(rejected_upload, dict(schema_version="1.1.0",
                                   meshes=[dict(mesh_id=1, coordinates=[component for x, y, z in points
                                                                        for component in (x, -z, y)], indices=indices)],
                                   elements=[dict(mesh_id=1, guid="oversized-upload-panel",
                                                  type="IfcBuildingElementProxy",
                                                  color=dict(r=66, g=207, b=103, a=255))]))
    eye = (-8., 0., NEAR)
    checkpoints = [Checkpoint(phase, frame, panel, (panel,), eye) for phase, frame in
                   (("resident", 88), ("rejected-gltf", 89), ("rejected-bim", 90),
                    ("rejected-budget", 91), ("rejected-upload", 93), ("after-rejections", 94))]
    checkpoints.append(Checkpoint("admitted", 98, replacement, (replacement,), eye))
    write_sequence(path, checkpoints, [(1, checkpoints[-1].frame, eye)])
    sequence = json.loads(path.read_bytes())
    rejected = {89: invalid_gltf, 90: invalid_bim, 91: over_budget}
    reloads = {**rejected, 98: accepted}
    for event in sequence["events"]:
        if event["frame"] in reloads:
            event["reload"] = str(reloads[event["frame"]])
    sequence["events"].append(dict(frame=92, guiReload=str(rejected_upload), guiWaitForLoad=True))
    sequence["events"].sort(key=lambda event: event["frame"])
    write_json(path, sequence)
    return checkpoints, (invalid_gltf, invalid_bim, rejected_upload), accepted


def verify_rejected_reload(run, rejected, accepted):
    from PIL import Image, ImageChops

    log = Path(run["log"]).read_text(encoding="utf-8", errors="replace")
    for candidate in rejected:
        require(candidate.name in log, f"The renderer did not report the rejected reload: {candidate}")
    require("glTF load failed: Scene asset memory budget exceeded" in log,
            "The oversized glTF was not rejected by scene memory admission")
    before, *following, after_commit = run["captures"].values()
    row = texture_row(before["state"], 0)
    require(row["residentFirstMip"] == 0, f"The initial texture did not promote before reload: {row}")
    counters = before["state"]["textureStreaming"]
    before_provider = before["state"]["assetResidency"]["providers"]
    checkpoints = {sample.phase: entry["state"]["assetResidency"]["rejectedReservations"]
                   for sample, entry in run["captures"].items()}
    require(checkpoints["rejected-upload"] > checkpoints["rejected-budget"] > checkpoints["rejected-bim"],
            f"The synchronous and interactive replacements did not exercise budget rejection: {checkpoints}")
    with Image.open(before["image"]) as baseline:
        for record in following:
            current = texture_row(record["state"], 0)
            require(current["imageId"] == row["imageId"] and current["residentFirstMip"] == 0,
                    f"A rejected reload changed the resident texture: {row} -> {current}")
            require(record["state"]["textureStreaming"]["evictions"] == counters["evictions"] and
                    record["state"]["assetResidency"]["providers"] == before_provider,
                    "A rejected reload evicted detail or replaced the current provider")
            with Image.open(record["image"]) as pixels:
                require(ImageChops.difference(baseline.convert("RGB"), pixels.convert("RGB")).getbbox() is None,
                        f"A rejected reload changed the viewport pixels: {record['image']}")
    committed = after_commit["state"]
    committed_memory = committed["assetResidency"]
    require(Path(committed["scene"]["primary"]) == accepted and committed_memory["providers"] != before_provider,
            "The fitting replacement did not publish after the rejections")
    resident_cap = RESIDENT_MIB * BYTES_PER_MIB
    largest_image = max(texture["residentBytes"] for texture in committed_memory["textures"])
    require(largest_image + before["state"]["assetResidency"]["used"]["residentBytes"] > resident_cap and
            committed_memory["peak"]["residentBytes"] <= resident_cap,
            "The fitting replacement did not prove admission under the live mip's budget credit")
    assert_pick(committed, next(reversed(run["captures"])))
    return dict(imageId=row["imageId"], residentFirstMip=row["residentFirstMip"],
                images={sample.phase: record["image"] for sample, record in run["captures"].items()}, log=run["log"])


def capture(renderer, directory, fixture, technique, sequence, checkpoints, flags=(), *, display="lit", lit=False,
            force_uncompressed=False, ui=False):
    from PIL import Image

    directory.mkdir()
    image = directory / "texture.png"
    log = directory / "renderer.log"
    (directory / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    command = [str(renderer), "--hidden", "--screenshot-viewport", "--validation", "--import-scale", "1",
               "--model", str(fixture), "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1",
               "--render-technique", technique, "--display-mode", display, "--no-taa", "--no-bloom", "--no-ddgi",
               "--no-ray-query", "--ray-shadows", "raster", "--sky", "plain", "--sky-horizon", "0", "0", "0",
               "--background-color", "0", "0", "0", "--environment-intensity", "0",
               "--directional-intensity", "2" if lit else "0", "--directional-direction", "-.6", ".3", "-1",
               "--camera-fov", str(FOV), "--exposure", "1", "--tone-map", "neutral", "--fixed-dt", ".016666667",
               "--capture-sequence", str(sequence), "--screenshot", str(image), *flags]
    if not ui:
        command.append("--no-ui")
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(directory), VK_LOADER_DEBUG="layer")
    if force_uncompressed:
        env["CONTAINER_DISABLE_TEXTURE_COMPRESSION"] = "1"
    with log.open("w", encoding="utf-8") as output:
        process = subprocess.run(command, cwd=renderer.parent, env=env, stdout=output, stderr=subprocess.STDOUT,
                                 timeout=300, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(text) and
            'Insert instance layer "VK_LAYER_KHRONOS_validation"' in text,
            f"Native capture or Khronos synchronization validation failed: {log}")
    captures = {}
    for sample in checkpoints:
        screenshot = image if sample == checkpoints[-1] else directory / f"texture.frame-{sample.frame:04d}.png"
        telemetry = screenshot.with_suffix(".telemetry.json")
        state = json.loads(telemetry.read_bytes())
        sidecar = json.loads(screenshot.with_suffix(".png.capture.json").read_bytes())
        require(state["validationEnabled"] and state["technique"] == technique and
                sidecar["source"] == "viewport" and not sidecar["includesUi"] and state["build"] == sidecar["build"],
                f"Unexpected capture route or validation state in {telemetry}")
        with Image.open(screenshot) as pixels:
            require(pixels.format == "PNG" and pixels.size == (WIDTH, HEIGHT), f"Invalid viewport capture {screenshot}")
        captures[sample] = dict(image=str(screenshot), telemetry=str(telemetry), state=state)
    return dict(log=str(log), command=command, captures=captures)


def assert_pick(state, sample):
    probe = state["depthProbe"]
    interaction = probe["sceneInteraction"]
    hit = (interaction["gpu"] or {}).get("hit")
    expected_name = sample.expected_pick or sample.probe.name
    raw = interaction["rawGpuId"]
    require(probe["pixel"] == list(sample.pixel(sample.probe, sample.probe_u)) and
            probe["opaque"] is not None and probe["opaque"] > 0 and
            type(raw) is int and raw > 0 and hit and hit["name"] == expected_name,
            f"GPU pick changed at {sample.phase}/{sample.frame}: expected {expected_name}, got {interaction}")
    return raw, hit


def texture_row(state, texture, *, color=True):
    source = next(row for row in state["gltfMaterialTextures"]["textures"] if row["sourceTexture"] == texture)
    slot = source["colorResource" if color else "dataResource"]
    require(type(slot) is int and slot >= 0, f"Source texture {texture} has no runtime resource")
    rows = [row for row in state["textureStreaming"]["rows"] if row["slot"] == slot]
    require(len(rows) == 1, f"Runtime slot {slot} does not identify exactly one effective image: {rows}")
    return rows[0]


def verified_rows(state, resident_mib, upload_mib):
    streaming = state["textureStreaming"]
    require(streaming["enabled"] is True and not state["rayShadows"]["supported"],
            "The capture did not exercise the requested raster streaming path")
    budget = state["assetResidency"]
    require(budget["limits"] == {"residentBytes": resident_mib * BYTES_PER_MIB,
                                 "uploadBytes": upload_mib * BYTES_PER_MIB}, "Incorrect scene budgets")
    for key, limit in budget["limits"].items():
        live, peak = budget["used"][key], budget["peak"][key]
        require(0 <= live <= peak and (not limit or peak <= limit),
                f"Actual VMA {key} allocations exceeded the configured cap: {live}, {peak}, {limit}")
    require(0 <= streaming["maxBytesPerTick"] <= UPLOAD_KIB * 1024 and
            0 <= streaming["peakStagingBytes"] <= upload_mib * BYTES_PER_MIB,
            f"Texture staging exceeded its copy or allocation limit: {streaming}")
    rows = tuple(texture_row(state, texture) for texture in range(2))
    for row in rows:
        first = row["residentFirstMip"]
        width = max(1, TEXTURE_SIZE >> first)
        require(row["sourceExtent"] == [TEXTURE_SIZE] * 2 and row["sourceMipCount"] == TEXTURE_SIZE.bit_length() and
                0 <= first < row["sourceMipCount"] and row["residentExtent"] == [width] * 2 and
                row["residentMipCount"] == row["sourceMipCount"] - first and row["residentBytes"] > 0 and
                row["imageId"] > 0 and row["format"] == 43,
                f"Texture slot did not expose a valid physical sRGB mip tail: {row}")
        require(0 <= row["demandFirstMip"] < row["sourceMipCount"], f"Invalid mip demand: {row}")
    require(sum(row["residentBytes"] for row in rows) <= budget["used"]["residentBytes"],
            "Effective texture images were missing from the physical resident budget")
    return streaming, rows, budget


def image_patch(image, point, radius=3):
    from PIL import ImageStat

    x, y = point
    return tuple(round(channel, 2) for channel in ImageStat.Stat(
        image.crop((x - radius, y - radius, x + radius + 1, y + radius + 1))).mean)


def color_error(first, second):
    return max(abs(a - b) for a, b in zip(first, second, strict=True))


def assert_coverage(control, stream):
    from PIL import Image, ImageChops, ImageFilter

    def mask(image):
        rgb = image.convert("RGB")
        background = Image.new("RGB", rgb.size, rgb.getpixel((WIDTH - 2, 2)))
        r, g, b = ImageChops.difference(rgb, background).split()
        return ImageChops.lighter(ImageChops.lighter(r, g), b).point(lambda value: 255 if value > 15 else 0)

    with Image.open(control) as a, Image.open(stream) as b:
        before, after = mask(a), mask(b)
        union = ImageChops.lighter(before, after).histogram()[255]
        shared = ImageChops.darker(before, after).histogram()[255]
        holes = ImageChops.subtract(before.filter(ImageFilter.MinFilter(3)), after).histogram()[255]
        require(union > 40 and shared / union >= .985 and holes == 0,
                f"Texture eviction changed a silhouette or left undefined interior pixels in {stream}")
    return dict(sharedPixels=shared, silhouettePixels=union, interiorHoles=holes)


def pressure_references(control):
    from PIL import Image

    captures = control["captures"]
    colors = {}
    for phase, mip in (("far", "tail"), ("close-a", "fine-a"), ("close-b", "fine-b")):
        sample = next(item for item in reversed(captures) if item.phase == phase)
        with Image.open(captures[sample]["image"]) as source:
            pixels = source.convert("RGB")
            for panel in sample.panels if mip == "tail" else sample.panels[:1]:
                color = image_patch(pixels, sample.pixel(panel), 2)
                colors[(panel.texture, "fine" if mip.startswith("fine") else "tail")] = color
    for texture in range(2):
        require(color_error(colors[(texture, "fine")], colors[(texture, "tail")]) > 40,
                f"Uncapped authored texture {texture} did not resolve distinct fine and coarse colors")
    return colors


def verify_pressure(control, run, *, upload_mib, require_turnover):
    from PIL import Image

    references = pressure_references(control)
    full_resident = max(entry["state"]["assetResidency"]["used"]["residentBytes"]
                        for entry in control["captures"].values())
    require(full_resident > RESIDENT_MIB * BYTES_PER_MIB,
            f"The independently loaded complete texture pyramids fit the pressure cap: {full_resident}")
    results, final, previous_counters, ids = [], {}, {}, None
    for sample, entry in run["captures"].items():
        before = control["captures"][sample]
        state = entry["state"]
        streaming, rows, budget = verified_rows(state, RESIDENT_MIB, upload_mib)
        slots = tuple(row["slot"] for row in rows)
        require(len(set(slots)) == 2 and (ids is None or slots == ids), "Mip publication changed a material's logical slot")
        ids = slots
        counters = {key: streaming[key] for key in
                    ("promotions", "evictions", "cancellations", "submittedChunks", "budgetDeferrals")}
        require(all(type(value) is int and value >= previous_counters.get(key, 0) for key, value in counters.items()),
                f"Texture streaming counters regressed at frame {sample.frame}")
        previous_counters = counters
        require(assert_pick(state, sample) == assert_pick(before["state"], sample),
                f"GPU picking identity changed during texture streaming at frame {sample.frame}")
        coverage = assert_coverage(before["image"], entry["image"])
        colors = {}
        with Image.open(entry["image"]) as source:
            pixels = source.convert("RGB")
            for index, panel in enumerate(sample.panels):
                color = image_patch(pixels, sample.pixel(panel), 2)
                level = rows[panel.texture]["residentFirstMip"]
                expected = "tail" if level >= 2 or "far" in sample.phase else "fine"
                options = ("fine", "tail") if index == 1 and "far" not in sample.phase else (expected,)
                require(any(color_error(color, references[(panel.texture, key)]) <= 8 for key in options),
                        f"Frame {sample.frame} sampled an unexpected color from {panel.name}: {color}, resident mip {level}")
                colors[panel.name] = color
        row = dict(frame=sample.frame, phase=sample.phase, image=entry["image"], telemetry=entry["telemetry"],
                   fullImage=before["image"], rows=rows, budget=budget, counters=counters, coverage=coverage, colors=colors,
                   knownGpuMs=state["gpuKnownMs"], fullKnownGpuMs=before["state"]["gpuKnownMs"])
        results.append(row)
        final[sample.phase] = row

    initial = final["far"]["rows"]
    require(all(row["residentExtent"][0] <= FALLBACK_SIZE and row["demandFirstMip"] >= 3 for row in initial),
            f"The initial far view retained fine residency: {initial}")
    early = final["early-close-a"]
    began_upload = early["counters"]["submittedChunks"] > final["far"]["counters"]["submittedChunks"]
    if began_upload:
        interrupted = final["interrupted-far"]
        require(early["rows"][0]["residentFirstMip"] > 0 and
                interrupted["counters"]["cancellations"] > early["counters"]["cancellations"],
                "The early camera cut failed to cancel an incomplete texture upload")
    if require_turnover:
        a, b, again = (final[key]["rows"] for key in ("close-a", "close-b", "return-a"))
        for near, focused in ((a, 0), (b, 1), (again, 0)):
            close, witness = near[focused], near[1 - focused]
            require(close["demandFirstMip"] == close["residentFirstMip"] == 0 and
                    witness["residentFirstMip"] >= 2 and close["residentBytes"] > witness["residentBytes"] * 3 and
                    close["imageId"] != initial[focused]["imageId"],
                    f"The cap did not trade actual fine image memory between textures: {near}")
        require(a[0]["imageId"] != b[0]["imageId"] != again[0]["imageId"] and
                b[1]["imageId"] != again[1]["imageId"] and previous_counters["evictions"] > 0 and
                previous_counters["promotions"] > 0 and previous_counters["submittedChunks"] > 0,
                "The repeated close swap failed to evict and republish physical mip allocations")
    performance = {key: statistics.median(row[key] for row in results)
                   for key in ("knownGpuMs", "fullKnownGpuMs")}
    return dict(log=run["log"], controlLog=control["log"], samples=results, settled=final,
                earlyUploadStarted=began_upload,
                completeResidentBytes=full_resident, medianTimedPassMs=performance)


def srgb_to_linear(value):
    return value / 12.92 if value <= .04045 else ((value + .055) / 1.055) ** 2.4


def write_bc3(path, size, endpoint, *, coarse_endpoint=None, mask=True):
    header = bytearray(128)
    blocks = (size + 3) // 4
    for offset, value in ((0, 0x20534444), (4, 124), (8, 0xa1007), (12, size), (16, size),
                          (20, blocks * blocks * 16), (28, size.bit_length()), (76, 32),
                          (80, 4), (84, 0x35545844), (108, 0x401008)):
        struct.pack_into("<I", header, offset, value)
    blocks_by_level = []
    for level in range(size.bit_length()):
        blocks = (max(1, size >> level) + 3) // 4
        packed_color = endpoint if coarse_endpoint is None or level < 2 else coarse_endpoint
        color = struct.pack("<HHI", packed_color, 0, 0)
        # A 0.30 mask separates 51/255 from 110/255. Applying an sRGB
        # transfer function to alpha would also wrongly discard 110/255.
        clear = bytes([51, 0]) + bytes(6) + color
        covered = bytes([110 if mask else 255, 0]) + bytes(6) + color
        left_blocks = blocks // 2 if mask else 0
        blocks_by_level.append((clear * left_blocks + covered * (blocks - left_blocks)) * blocks)
    path.write_bytes(bytes(header) + b"".join(blocks_by_level))


def write_semantic_fixtures(directory):
    import numpy as np
    from PIL import Image
    from gltf_texture_formats_runtime_regression import ktx2_bytes

    result = {}
    panel = Panel("alpha-panel", 0, 0., 0., 1.6, 1.2)
    receiver = Panel("receiver", 1, 0., 0., 1.8, 1.4, -.04)
    dds = directory / "coverage-mask.dds"
    fallback = directory / "unexpected-fallback.png"
    Image.new("RGBA", (4, 4), (250, 5, 240, 255)).save(fallback)
    r, g, b = 24, 15, 5
    write_bc3(dds, 512, r << 11 | g << 5 | b)
    images = (dict(uri=fallback.name, mimeType="image/png"), dict(uri=dds.name, mimeType="image/vnd-ms.dds"))
    result["mask"] = directory / "material-bc3-alpha.gltf"
    make_asset((panel, receiver), [color_material("BC3 coverage", texture=0, mask=True), color_material("blue", color=BACK)],
               images, dds=True).write(result["mask"])
    # An opaque half-panel is independent of alpha texture sampling. Decode the
    # packed color endpoints by their channel widths and author linear RGB.
    half = Panel(panel.name, 0, panel.width / 4, panel.y, panel.width / 2, panel.height)
    linear = tuple(srgb_to_linear(value) for value in (r / 31, g / 63, b / 31))
    result["maskReference"] = directory / "material-bc3-alpha-reference.gltf"
    make_asset((half, receiver), [color_material("analytic coverage", color=linear), color_material("blue", color=BACK)]).write(
        result["maskReference"])
    result["maskPanel"] = panel

    panel = Panel("opaque-bc3-panel", 0, 0., 0., 1.02, .98)
    fine, coarse = (24, 12, 5), (4, 47, 25)
    opaque = directory / "streamed-opaque.dds"
    write_bc3(opaque, TEXTURE_SIZE, fine[0] << 11 | fine[1] << 5 | fine[2],
              coarse_endpoint=coarse[0] << 11 | coarse[1] << 5 | coarse[2], mask=False)
    result["opaqueBc3"] = opaque.with_suffix(".gltf")
    make_asset((panel,), [color_material("Opaque BC3", texture=0)],
               (dict(uri=fallback.name, mimeType="image/png"), dict(uri=opaque.name, mimeType="image/vnd-ms.dds")),
               dds=True).write(result["opaqueBc3"])
    # The KTX control's RGB values are authored directly from the BC565
    # endpoints; this does not call the production decode or compression path.
    rgb8 = tuple(tuple(round(255 * value / scale) for value, scale in zip(color, (31, 63, 31), strict=True))
                 for color in (fine, coarse))
    opaque_reference = directory / "streamed-opaque-reference.ktx2"
    opaque_reference.write_bytes(ktx2_bytes(rgba_levels(TEXTURE_SIZE, rgb8)))
    result["opaqueBc3Reference"] = opaque_reference.with_suffix(".gltf")
    make_asset((panel,), [color_material("Independent RGBA8", texture=0)],
               [dict(uri=opaque_reference.name, mimeType="image/ktx2")]).write(result["opaqueBc3Reference"])
    result["opaqueBc3Panel"] = panel

    panel = Panel("normal-panel", 0, 0., 0., 1.6, 1.2)
    encoded = (181, 109, 228)
    values = np.asarray(encoded, dtype=np.float32) / 255.
    normals = {
        "normal": ktx2_bytes(rgba_levels(256, (encoded, encoded)), data_role=True),
        "normalReference": ktx2_bytes([np.array([[[*values, 1.]]], dtype=np.float32)], floating=True, data_role=True),
        "normalWrongGamma": ktx2_bytes([np.array([[[*(srgb_to_linear(value) for value in values), 1.]]], dtype=np.float32)],
                                      floating=True, data_role=True),
    }
    for label, data in normals.items():
        image = directory / f"{label}.ktx2"
        image.write_bytes(data)
        scene = image.with_suffix(".gltf")
        material = dict(name="normal probe", normalTexture=dict(index=0),
                        pbrMetallicRoughness=dict(baseColorFactor=[.7, .7, .7, 1.], metallicFactor=0., roughnessFactor=.8))
        make_asset((panel,), [material], [dict(uri=image.name, mimeType="image/ktx2")]).write(scene)
        result[label] = scene
    result["normalPanel"] = panel
    return result


def semantic_sequence(path, panel, *, mask=False):
    eye = (0., 0., 2.5)
    samples = ([Checkpoint("mask-clear", 19, panel, (panel,), eye, .25, "receiver"),
                Checkpoint("mask-covered", 20, panel, (panel,), eye, .75, panel.name)] if mask else
               [Checkpoint("normal", 20, panel, (panel,), eye)])
    write_sequence(path, samples, [(1, 20, eye)])
    return samples


def bc3_sequence(path, panel):
    far_eye, near_eye = (0., 0., 20.), (0., 0., NEAR)
    samples = [Checkpoint("far", 24, panel, (panel,), far_eye)]
    samples += [Checkpoint("close", frame, panel, (panel,), near_eye) for frame in (25, 26, 32, 44, 66, 104)]
    write_sequence(path, samples, [(1, 24, far_eye), (25, 104, near_eye)])
    return samples


def verify_mask(actual, independent):
    from PIL import Image

    results = []
    for sample, entry in actual["captures"].items():
        baseline = independent["captures"][sample]
        state = entry["state"]
        # The reference has different geometry for its clear half. Validate
        # both paths' resolved object name and compare the sampled colors.
        assert_pick(state, sample)
        assert_pick(baseline["state"], sample)
        slot = state["gltfMaterialTextures"]["textures"][0]["colorResource"]
        streaming = state["textureStreaming"]
        require(type(slot) is int and slot >= 0 and streaming["enabled"] and
                all(row["slot"] != slot for row in streaming["rows"]),
                "The renderer exposed a coverage-sensitive source as an evictable texture")
        probe = sample.pixel(sample.probe, sample.probe_u)
        with Image.open(entry["image"]) as actual_pixels, Image.open(baseline["image"]) as wanted_pixels:
            color = image_patch(actual_pixels.convert("RGB"), probe)
            expected = image_patch(wanted_pixels.convert("RGB"), probe)
        require(color_error(color, expected) <= 4,
                f"BC3 coverage or sRGB color did not match independent opaque geometry: {color} versus {expected}")
        results.append(dict(phase=sample.phase, expectedPick=sample.expected_pick, image=entry["image"],
                            reference=baseline["image"], color=color, expected=expected, pinnedSlot=slot))
    return dict(samples=results, log=actual["log"], referenceLog=independent["log"])


def verify_streamed_bc3(actual, independent, *, forced_uncompressed=False):
    from PIL import Image

    results = []
    limit = 2 * BYTES_PER_MIB
    captures = actual["captures"]
    far, near = next(iter(captures)), next(reversed(captures))
    references = {}
    for sample in (far, near):
        with Image.open(independent["captures"][sample]["image"]) as image:
            references[sample.phase] = image_patch(image.convert("RGB"), sample.pixel(sample.probe))
    require(color_error(references["far"], references["close"]) > 50, "BC3 mip witness colors are indistinguishable")
    for sample, entry in captures.items():
        state = entry["state"]
        control = independent["captures"][sample]
        streaming = state["textureStreaming"]
        budget = state["assetResidency"]
        row = texture_row(state, 0)
        device_bc3 = streaming["deviceBc3Srgb"]
        if forced_uncompressed:
            require(device_bc3 is False, "The renderer did not honor the no-compression control")
        require(type(device_bc3) is bool and row["format"] == (138 if device_bc3 else 43) and row["srgb"] is True and
                row["compressed"] is device_bc3 and streaming["enabled"],
                f"BC3 did not use native blocks or the sRGB CPU fallback selected by device capability: {row}")
        require(row["sourceExtent"] == [TEXTURE_SIZE, TEXTURE_SIZE] and
                row["residentMipCount"] == row["sourceMipCount"] - row["residentFirstMip"] and
                0 < row["residentBytes"] <= budget["used"]["residentBytes"] <= budget["peak"]["residentBytes"] <=
                RESIDENT_MIB * BYTES_PER_MIB and budget["peak"]["uploadBytes"] <= limit and
                streaming["maxBytesPerTick"] <= UPLOAD_KIB * 1024 and streaming["peakStagingBytes"] <= limit,
                f"BC3 mip residency did not respect allocation or copy bounds: {row}")
        require(assert_pick(state, sample) == assert_pick(control["state"], sample),
                f"BC3 mip publication changed the GPU pick at {sample.frame}")
        coverage = assert_coverage(control["image"], entry["image"])
        with Image.open(entry["image"]) as image:
            color = image_patch(image.convert("RGB"), sample.pixel(sample.probe))
        expected = "far" if sample.phase == "far" or row["residentFirstMip"] >= 2 else "close"
        require(color_error(color, references[expected]) <= 5,
                f"BC3 physical mip {row['residentFirstMip']} sampled the wrong authored color: {color}")
        results.append(dict(frame=sample.frame, phase=sample.phase, image=entry["image"], reference=control["image"],
                            color=color, expected=references[expected], coverage=coverage, mip=row))
    first, final = (item["mip"] for item in (results[0], results[-1]))
    require(first["residentFirstMip"] >= 3 and final["residentFirstMip"] == 0 and
            first["slot"] == final["slot"] and first["imageId"] != final["imageId"] and
            final["residentBytes"] > first["residentBytes"] * 3,
            f"The opaque BC3 fixture did not promote a physically smaller tail into mip zero: {first}, {final}")
    return dict(samples=results, log=actual["log"], referenceLog=independent["log"],
                nativeBc3=final["compressed"], forcedUncompressed=forced_uncompressed)


def verify_normal(runs, sample):
    from PIL import Image

    point = sample.pixel(sample.probe)
    colors = {}
    for label, run in runs.items():
        entry = run["captures"][sample]
        assert_pick(entry["state"], sample)
        with Image.open(entry["image"]) as pixels:
            colors[label] = image_patch(pixels.convert("RGB"), point, 12)
    state = runs["normal"]["captures"][sample]["state"]
    row = texture_row(state, 0, color=False)
    require(row["format"] == 37 and row["residentMipCount"] > 0, f"Normal image is not resident as linear RGBA8: {row}")
    require(color_error(colors["normal"], colors["normalReference"]) <= 3 and
            color_error(colors["normal"], colors["normalWrongGamma"]) >= 25,
            f"Normal map lost linear interpretation or its negative control is vacuous: {colors}")
    return dict(colors=colors, mip=row, images={label: run["captures"][sample]["image"] for label, run in runs.items()},
                logs={label: run["log"] for label, run in runs.items()})


def streaming_flags(resident_mib, upload_mib):
    return ("--texture-streaming", "--texture-upload-kb", str(UPLOAD_KIB),
            "--asset-memory-mb", str(resident_mib), "--asset-upload-mb", str(upload_mib))


def comparison(path, cases):
    from PIL import Image, ImageDraw

    width, height, label_height = 320, 180, 22
    canvas = Image.new("RGB", (width * 4, (height + label_height) * len(cases)), "#171717")
    labels = ImageDraw.Draw(canvas)
    for row, (technique, case) in enumerate(cases.items()):
        selected = case["settled"]
        for column, phase in enumerate(("far", "close-a", "close-b", "return-a")):
            capture = selected[phase]
            x, y = column * width, row * (height + label_height)
            levels = "/".join(str(entry["residentFirstMip"]) for entry in capture["rows"])
            labels.text((x + 7, y + 4), f"{technique}  {phase}  mip {levels}", fill="white")
            with Image.open(capture["image"]) as pixels:
                canvas.paste(pixels.convert("RGB").resize((width, height), Image.Resampling.LANCZOS), (x, y + label_height))
    canvas.save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--technique", choices=TECHNIQUES, action="append")
    parser.add_argument("--suite", choices=("all", "pressure", "reload", "semantics"), default="all")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--require-opt-in", action="store_true")
    args = parser.parse_args()
    if args.require_opt_in and os.environ.get(OPT_IN) != "1":
        print(f"SKIP: set {OPT_IN}=1 to run native texture streaming captures")
        return 77
    renderer = args.renderer.resolve(strict=not args.generate_only)
    output = args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True, exist_ok=False)
    hash_before = None if args.generate_only else digest(renderer)
    techniques = args.technique or TECHNIQUES
    results = dict(passed=False, generateOnly=args.generate_only, rendererSha256=hash_before,
                   residentCapMiB=RESIDENT_MIB, copyCapKiB=UPLOAD_KIB, pressure={}, reload={}, semantics={})
    try:
        if args.suite in ("all", "pressure", "reload"):
            scene = write_pressure_fixture(output)
        if args.suite in ("all", "pressure"):
            sequence = output / "pressure.sequence.json"
            checkpoints = pressure_sweep(sequence)
            if not args.generate_only:
                for technique in techniques:
                    full = capture(renderer, output / f"{technique}-full", scene, technique, sequence, checkpoints)
                    streamed = capture(renderer, output / f"{technique}-streamed", scene, technique, sequence,
                                       checkpoints, streaming_flags(RESIDENT_MIB, 6))
                    results["pressure"][technique] = verify_pressure(full, streamed, upload_mib=6, require_turnover=True)
                    if technique == techniques[0]:
                        tight = capture(renderer, output / f"{technique}-tight-staging", scene, technique, sequence,
                                        checkpoints, streaming_flags(RESIDENT_MIB, 2))
                        results["tightStaging"] = verify_pressure(full, tight, upload_mib=2, require_turnover=False)
                contact = output / "comparison.png"
                comparison(contact, results["pressure"])
                results["comparison"] = str(contact)
        if args.suite in ("all", "reload"):
            sequence = output / "rejected-reload.sequence.json"
            checkpoints, rejected, accepted = rejected_reload_sequence(sequence)
            if not args.generate_only:
                for technique in techniques:
                    run = capture(renderer, output / f"{technique}-rejected-reload", scene, technique, sequence,
                                  checkpoints, (*streaming_flags(RESIDENT_MIB, 6), "--geometry-upload-kb", "256"),
                                  ui=True)
                    results["reload"][technique] = verify_rejected_reload(run, rejected, accepted)
        if args.suite in ("all", "semantics"):
            fixtures = write_semantic_fixtures(output)
            mask_path = output / "mask.sequence.json"
            opaque_path = output / "opaque-bc3.sequence.json"
            normals_path = output / "normal.sequence.json"
            mask_samples = semantic_sequence(mask_path, fixtures["maskPanel"], mask=True)
            opaque_samples = bc3_sequence(opaque_path, fixtures["opaqueBc3Panel"])
            normal_samples = semantic_sequence(normals_path, fixtures["normalPanel"])
            if not args.generate_only:
                for technique in techniques:
                    mask = capture(renderer, output / f"{technique}-bc3-mask", fixtures["mask"], technique,
                                   mask_path, mask_samples, streaming_flags(2, 2))
                    reference = capture(renderer, output / f"{technique}-mask-reference", fixtures["maskReference"],
                                        technique, mask_path, mask_samples)
                    results["semantics"][f"{technique}-mask"] = verify_mask(mask, reference)
                    bc3_args = (technique, opaque_path, opaque_samples, streaming_flags(RESIDENT_MIB, 2))
                    native = capture(renderer, output / f"{technique}-streamed-bc3", fixtures["opaqueBc3"], *bc3_args)
                    oracle = capture(renderer, output / f"{technique}-bc3-reference", fixtures["opaqueBc3Reference"],
                                     technique, opaque_path, opaque_samples)
                    results["semantics"][f"{technique}-streamed-bc3"] = verify_streamed_bc3(native, oracle)
                    if technique == techniques[0]:
                        forced = capture(renderer, output / f"{technique}-bc3-cpu-decode", fixtures["opaqueBc3"],
                                         *bc3_args, force_uncompressed=True)
                        forced_result = verify_streamed_bc3(forced, oracle, forced_uncompressed=True)
                        results["semantics"]["bc3-cpu-decode"] = forced_result
                        hardware_result = results["semantics"][f"{technique}-streamed-bc3"]
                        if hardware_result["nativeBc3"]:
                            native_bytes = hardware_result["samples"][-1]["mip"]["residentBytes"]
                            expanded_bytes = forced_result["samples"][-1]["mip"]["residentBytes"]
                            require(expanded_bytes > 2 * native_bytes,
                                    "BC3 blocks did not physically save GPU memory compared with CPU expansion")
                if "deferred-raster" in techniques:
                    normals = {}
                    for key in ("normal", "normalReference", "normalWrongGamma"):
                        flags = streaming_flags(2, 2) if key == "normal" else ()
                        normals[key] = capture(renderer, output / key, fixtures[key], "deferred-raster", normals_path,
                                               normal_samples, flags, display="normals", lit=True)
                    results["semantics"]["normal"] = verify_normal(normals, normal_samples[-1])
        if not args.generate_only:
            require(digest(renderer) == hash_before, "The executable changed during texture residency captures")
            results["passed"] = True
    finally:
        write_json(output / "results.json", results)
    print(f"{'PLAN' if args.generate_only else 'PASS'} {output / 'results.json'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
