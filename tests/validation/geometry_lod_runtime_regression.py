"""Capture deterministic native raster LOD sweeps for glTF, dotBIM, and USD."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import tempfile

from display_capture_test_support import Asset, validation_log_error


OPT_IN = "CONTAINER_RUN_GPU_GEOMETRY_LOD"
WIDTH, HEIGHT, FOV = 640, 400, 42
GRID = 24
FULL_TRIANGLES = 2 * GRID * GRID
PIXEL_ERROR = 2
PANEL_CENTERS = (-1.75, 1.75)
PANEL_COLORS = ((.82, .10, .04), (.04, .19, .85))
PANEL_GUIDS = ("geometry-lod-left", "geometry-lod-right")
BIM_STOREY = "LOD Test Storey"
TECHNIQUES = ("deferred-raster", "forward-raster")


@dataclass(frozen=True)
class Sample:
    cycle: int
    direction: str
    distance: float
    panel: int
    frame: int

    @property
    def pixel(self):
        focal = HEIGHT / (2 * math.tan(math.radians(FOV / 2)))
        return (int(WIDTH / 2 + focal * PANEL_CENTERS[self.panel] / (self.distance - .25)),
                HEIGHT // 2)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def folded_panel(center, columns=GRID, rows=GRID):
    # Binary fractions keep the five planar bands exactly coplanar in all importers.
    points = [(center + (x - columns / 2) / 8, (y - rows / 2) * 3 / 32,
               max(0, min(x - 4, 20 - x, 4)) / 16)
              for y in range(rows + 1) for x in range(columns + 1)]
    stride = columns + 1
    indices = []
    for y in range(rows):
        for x in range(columns):
            first = y * stride + x
            # Generated face tangents on a coplanar band share an axis.
            indices.extend((first, first + 1, first + stride + 1,
                            first + stride + 1, first + stride, first))
    return points, indices


def bim_mesh(panel, points, indices):
    # Source dotBIM axes are Z-up and +Y forward.
    return dict(mesh_id=panel, indices=indices,
                coordinates=[value for x, y, z in points for value in (x, -z, y)])


def write_fixtures(directory):
    asset = Asset()
    asset.model["scenes"] = [dict(nodes=[0, 1])]
    asset.model["nodes"] = []
    bim_meshes, bim_elements = [], []
    usd = ['#usda 1.0', '(upAxis = "Y" metersPerUnit = 1)', 'def Xform "GeometryLodFixture" {']
    for panel, (center, color, guid) in enumerate(zip(PANEL_CENTERS, PANEL_COLORS, PANEL_GUIDS, strict=True)):
        points, indices = folded_panel(center)
        attributes = dict(POSITION=asset.accessor("VEC3", points),
                          NORMAL=asset.accessor("VEC3", [(0, 0, 1)] * len(points)))
        asset.model["nodes"].append(dict(name=guid, mesh=panel))
        asset.model["meshes"].append(dict(primitives=[dict(attributes=attributes, material=panel,
            indices=asset.accessor("SCALAR", indices, 5123, indices=True))]))
        asset.model["materials"].append(dict(name=guid, pbrMetallicRoughness=dict(
            baseColorFactor=[*color, 1], metallicFactor=0, roughnessFactor=.85)))

        bim_meshes.append(bim_mesh(panel, points, indices))
        bim_elements.append(dict(mesh_id=panel, guid=guid, sourceId=guid,
            type="IfcBuildingElementProxy", storeyName=BIM_STOREY,
            color=dict(zip("rgba", [*(round(c * 255) for c in color), 255])),
            vector=dict(x=0, y=0, z=0), rotation=dict(qx=0, qy=0, qz=0, qw=1)))

        coordinates = ', '.join(str(point) for point in points)
        usd += [f'def Mesh "Panel{panel}" (customData = {{ string guid = "{guid}"',
                f'    string storeyName = "{BIM_STOREY}" }}) {{',
                f'point3f[] points = [{coordinates}]',
                'int[] faceVertexCounts = [' + ', '.join(['3'] * FULL_TRIANGLES) + ']',
                'int[] faceVertexIndices = [' + ', '.join(map(str, indices)) + ']',
                'uniform token subdivisionScheme = "none"',
                f'color3f[] primvars:displayColor = [{color}]', 'bool doubleSided = false', '}']

    fixtures = {name: directory / f"folded-panels.{extension}" for name, extension in
                (("gltf", "gltf"), ("bim", "bim"), ("usd", "usda"))}
    asset.write(fixtures["gltf"])
    write_json(fixtures["bim"], dict(schema_version="1.1.0", meshes=bim_meshes, elements=bim_elements))
    fixtures["usd"].write_text("\n".join([*usd, '}', '']), encoding="utf-8")
    return fixtures


def write_sequence(path, cycles, *, bim_checkpoints=False, warmup_frames=0):
    # Match in/out distances, with enough samples within the transition band to
    # observe path dependence in the actual draw ranges. Each stop settles both
    # render frames in flight before either panel is picked.
    distances = [6.] + [round(14 * 1.16 ** step, 3) for step in range(20)]
    stops = ([('out', distance) for distance in distances] +
             [('in', distance) for distance in reversed(distances)])
    samples, cameras = [], []
    if warmup_frames:
        cameras.append(dict(frame=1, position=[0, 0, distances[0]], target=[0, 0, 0]))
    # The startup selection outline can cover adjacent meshes when each panel
    # occupies a few pixels. Clear it before sampling coverage and materials.
    events = [dict(frame=2, viewportPointer={"action": "select", "pixel": [1, 1]})]
    for cycle in range(cycles):
        for stop, (direction, distance) in enumerate(stops):
            first = warmup_frames + 5 * (cycle * len(stops) + stop) + 1
            cameras += [dict(frame=frame, position=[0, 0, distance], target=[0, 0, 0])
                        for frame in (first, first + 4)]
            for panel in range(2):
                sample = Sample(cycle, direction, distance, panel, first + 3 + panel)
                samples.append(sample)
                events.append(dict(frame=sample.frame, depthProbe=list(sample.pixel)))
    if bim_checkpoints:
        # Leave enough frames for the visibility mask and compaction setup to
        # settle after the filter changes, including every swapchain image.
        distance = distances[-2]
        for phase, event in (("filtered", {"bimStoreyFilter": BIM_STOREY}),
                             ("selected", {"bimSelectedObject": 0}),
                             ("sectioned", {"sectionPlane": [0, 1, 0, .2],
                                            "viewportPointer": {"action": "select", "pixel": [1, 1]}})):
            first = samples[-1].frame + 1
            cameras += [dict(frame=frame, position=[0, 0, distance], target=[0, 0, 0])
                        for frame in (first, first + 8)]
            events.append(dict(frame=first, **event))
            for panel in range(2):
                sample = Sample(cycles, phase, distance, panel, first + 7 + panel)
                samples.append(sample)
                events.append(dict(frame=sample.frame, depthProbe=list(sample.pixel)))
    write_json(path, dict(schemaVersion=1, frames=samples[-1].frame,
                          sampleFrames=[sample.frame for sample in samples], camera=cameras, events=events))
    return samples


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def capture(renderer, directory, fixture, technique, sequence, samples, lod_flags):
    from PIL import Image

    directory.mkdir()
    output = directory / "sweep.png"
    command = [str(renderer), "--hidden", "--no-ui", "--screenshot-viewport", "--validation",
               "--model", str(fixture), "--import-scale", "1", "--width", str(WIDTH), "--height", str(HEIGHT),
               "--msaa", "1", "--render-technique", technique, "--display-mode", "lit", "--no-taa",
               "--no-bloom", "--no-ddgi", "--no-ray-query", "--ray-shadows", "raster", "--sky", "plain",
               "--sky-horizon", "0", "0", "0", "--background-color", "0", "0", "0", "--environment-intensity", ".15",
               "--directional-direction", "0", "0", "-1", "--directional-intensity", "2.5",
               "--camera-position", "0", "0", "6", "--camera-target", "0", "0", "0", "--camera-fov", str(FOV),
               "--exposure", ".8", "--tone-map", "neutral", "--fixed-dt", ".016666667",
               "--capture-sequence", str(sequence), "--screenshot", str(output), *lod_flags]
    log = directory / "renderer.log"
    (directory / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(directory), VK_LOADER_DEBUG="layer")
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, env=env, stdout=stream,
                                 stderr=subprocess.STDOUT, timeout=300,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    messages = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0 and not validation_log_error(messages),
            f"Vulkan capture or synchronization validation failed. See {log}")
    require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in messages,
            f"Vulkan loader did not confirm an active validation layer. See {log}")
    states = {}
    for sample in samples:
        stem = "sweep" if sample == samples[-1] else f"sweep.frame-{sample.frame:04d}"
        image = directory / f"{stem}.png"
        telemetry = directory / f"{stem}.telemetry.json"
        state = json.loads(telemetry.read_bytes())
        metadata = json.loads(image.with_suffix(".png.capture.json").read_bytes())
        require(state["validationEnabled"] and state["technique"] == technique and
                metadata["source"] == "viewport" and not metadata["includesUi"] and
                metadata["build"] == state["build"] and metadata["sceneRevisions"] == state["sceneRevisions"],
                f"Inconsistent renderer or viewport capture metadata {image}")
        with Image.open(image) as pixels:
            require(pixels.format == "PNG" and pixels.size == (WIDTH, HEIGHT), f"Invalid capture {image}")
        states[sample] = dict(image=image, telemetry=telemetry, state=state)
    return states


def compare_coverage(control, active, sample, *, measure_material=True):
    from PIL import Image, ImageChops, ImageFilter

    def mask(rgb):
        background = Image.new("RGB", rgb.size, rgb.getpixel((0, 0)))
        channels = ImageChops.difference(rgb, background).split()
        maximum = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
        return maximum.point(lambda value: 255 if value >= 18 else 0)

    with Image.open(control) as control_image, Image.open(active) as active_image:
        baseline, enabled = control_image.convert("RGB"), active_image.convert("RGB")
        before, after = mask(baseline), mask(enabled)
        common = ImageChops.darker(before, after).histogram()[255]
        total = ImageChops.lighter(before, after).histogram()[255]
        require(total > 32 and common / total >= .97,
                f"Visible silhouette changed in {active} at {sample.distance:g} m ({common}/{total})")
        interior = before.filter(ImageFilter.MinFilter(3))
        holes = ImageChops.subtract(interior, after).histogram()[255]
        require(holes == 0, f"{holes} lost interior pixels in {active}")
        first, second = (0, 2) if sample.panel == 0 else (2, 0)
        base_color, color = baseline.getpixel(sample.pixel), enabled.getpixel(sample.pixel)
        if measure_material:
            require(base_color[first] - base_color[second] > 12 and color[first] - color[second] > 12 and
                    max(abs(a - b) for a, b in zip(base_color, color, strict=True)) <= 12,
                    f"Panel {sample.panel} material changed at {sample.pixel} in {active}: {base_color} to {color}")
    return dict(sharedMaskPixels=common, unionMaskPixels=total, interiorHoles=holes, panelRgb=color)


def identity(state, sample, provider):
    probe = state["depthProbe"]
    require(probe["pixel"] == list(sample.pixel) and probe["opaque"] is not None and probe["opaque"] > 0,
            f"Panel {sample.panel} did not produce opaque depth at {sample.pixel}")
    interaction = probe["sceneInteraction"]
    raw = interaction["rawGpuId"]
    require(type(raw) is int and raw > 0, f"Panel {sample.panel} has no GPU pick at {sample.pixel}")
    if provider == "gltf":
        for route in ("cpu", "gpu"):
            hit = interaction[route]["hit"]
            require(hit and hit["sourceNode"] == sample.panel and hit["sourceMesh"] == sample.panel and
                    hit["sourcePrimitive"] == 0 and hit["name"] == PANEL_GUIDS[sample.panel],
                    f"The {route} pick resolved the wrong glTF source at {sample.pixel}: {hit}")
        return dict(raw=raw, cpu=interaction["cpu"], gpu=interaction["gpu"])
    bim = interaction["bim"]
    require(bim and bim["guid"] == PANEL_GUIDS[sample.panel],
            f"GPU pick at {sample.pixel} resolved the wrong BIM/USD product: {bim}")
    return dict(raw=raw, **{key: bim[key] for key in ("objectIndex", "guid", "sourceId", "materialIndex")})


def lod_stats(state, provider):
    key = "primary" if provider == "gltf" else "bim"
    lod = state["geometryLod"][key]
    require(lod["baseTriangles"] == 2 * FULL_TRIANGLES and 0 < lod["triangles"] <= lod["baseTriangles"],
            f"The {provider} draw counts do not match both fixture panels: {lod}")
    budget = state["assetResidency"]["used"]["residentBytes"]
    return dict(**lod, residentBytes=budget, gpuMs=state["gpuKnownMs"])


def verify(control, active, provider):
    results, trajectory, original = [], {}, {}
    checkpoints = []
    for sample, entry in active.items():
        if sample.direction not in ("out", "in"):
            continue
        state = entry["state"]
        draw = lod_stats(state, provider)
        picked = identity(state, sample, provider)
        key = (sample.direction, sample.distance, sample.panel)
        if sample.cycle == 0:
            first = control[sample]
            before = lod_stats(first["state"], provider)
            require(before["triangles"] == before["baseTriangles"] and before["extraIndexBytes"] == 0,
                    f"The disabled fallback allocated or selected simplified geometry: {before}")
            require(draw["extraIndexBytes"] > 0 and not state["rayShadows"]["supported"],
                    f"The {provider} sweep did not upload LOD ranges and exercise the raster-only capability path")
            reference_pick = identity(first["state"], sample, provider)
            require(picked == reference_pick, f"LOD changed the identity or material selected at {sample.pixel}")
            images = compare_coverage(first["image"], entry["image"], sample)
            original[key] = (draw["triangles"], draw["levels"], picked)
            if sample.panel == 0:
                trajectory[(sample.direction, sample.distance)] = draw["triangles"]
            results.append(dict(direction=sample.direction, distance=sample.distance, panel=sample.panel,
                                pixel=sample.pixel, pick=picked, image=images, full=before, lod=draw,
                                fullCapture=str(first["image"]), lodCapture=str(entry["image"]),
                                fullTelemetry=str(first["telemetry"]), lodTelemetry=str(entry["telemetry"])))
        else:
            require(original[key] == (draw["triangles"], draw["levels"], picked),
                    f"The second sweep selected different geometry or a different identity: {key}")
    outbound = sorted((distance, triangles) for (direction, distance), triangles in trajectory.items()
                      if direction == "out")
    inbound = sorted((distance, triangles) for (direction, distance), triangles in trajectory.items()
                     if direction == "in")
    require(all(series[0][1] == 2 * FULL_TRIANGLES and series[-1][1] <= FULL_TRIANGLES
                for series in (outbound, inbound)), f"LOD sweep must preserve near detail and simplify far draws: {trajectory}")
    require(all(a[1] >= b[1] for series in (outbound, inbound) for a, b in zip(series, series[1:])),
            "Raster LOD did not simplify monotonically with increasing distance")
    hysteresis = [distance for (distance, out), (_, back) in zip(outbound, inbound, strict=True) if out > back]
    require(hysteresis, f"Matching outbound and return distances showed no hysteresis: {trajectory}")
    if provider != "gltf":
        checkpoints = verify_bim_checkpoints(control, active, provider, trajectory)
    far_samples = [row for row in results if row["panel"] == 0 and row["distance"] == outbound[-1][0]]
    return dict(samples=results, bimCheckpoints=checkpoints, hysteresisDistances=hysteresis,
                farMeshTriangles=outbound[-1][1],
                medianFarTimedPassGpuMs={key: statistics.median(row[key]["gpuMs"] for row in far_samples)
                                         for key in ("full", "lod")})


def verify_bim_checkpoints(control, active, provider, trajectory):
    control_checkpoints = {(sample.direction, sample.panel): entry for sample, entry in control.items()
                           if sample.direction not in ("out", "in")}
    results = []
    for sample, entry in active.items():
        phase = sample.direction
        if phase in ("out", "in"):
            continue
        baseline = control_checkpoints[(phase, sample.panel)]
        full = lod_stats(baseline["state"], provider)
        lod = lod_stats(entry["state"], provider)
        coarse = trajectory[("out", sample.distance)]
        expected = {"filtered": (coarse, 0), "selected": (FULL_TRIANGLES + coarse // 2, 1),
                    "sectioned": (2 * FULL_TRIANGLES, 2)}
        triangles, full_draws = expected[phase]
        require(coarse < 2 * FULL_TRIANGLES and full["triangles"] == 2 * FULL_TRIANGLES and
                lod["triangles"] == triangles and lod["levels"][0] == full_draws,
                f"Unexpected {provider} geometry in the {phase} checkpoint: {lod}")
        require(full["opaqueCompactionReady"] and lod["opaqueCompactionReady"],
                f"The {provider} storey filter did not exercise GPU draw compaction in the {phase} checkpoint")
        picked = identity(entry["state"], sample, provider)
        require(picked == identity(baseline["state"], sample, provider),
                f"{phase} changed the {provider} product or material selected at {sample.pixel}")
        image = compare_coverage(baseline["image"], entry["image"], sample, measure_material=phase == "filtered")
        results.append(dict(phase=phase, distance=sample.distance, panel=sample.panel, pick=picked, image=image,
                            full=full, lod=lod, fullCapture=str(baseline["image"]), lodCapture=str(entry["image"]),
                            fullTelemetry=str(baseline["telemetry"]), lodTelemetry=str(entry["telemetry"])))
    require(len(results) == 6, "Missing filtered, selected, or sectioned BIM coverage")
    masks = {row["phase"]: row["image"]["unionMaskPixels"] for row in results if row["panel"] == 0}
    require(masks["sectioned"] < .8 * masks["filtered"], f"Section plane did not visibly clip the panels: {masks}")
    return results


def verify_bim_budget_fallback(renderer, output, technique):
    # Face-expanded dotBIM uploads 3 packed 72-byte vertices plus 3 uint32
    # indices per triangle. The 1 MiB cap fits this source but excludes the
    # extra index ranges generated from the dense interior.
    points, indices = folded_panel(0, columns=47, rows=48)
    fixture = output / "budget-panel.bim"
    write_json(fixture, dict(schema_version="1.1.0", meshes=[bim_mesh(0, points, indices)],
        elements=[dict(mesh_id=0, guid="geometry-lod-budget", type="IfcBuildingElementProxy",
                       color=dict(r=209, g=25, b=10, a=255))]))
    sample = Sample(0, "budget", 350., 0, 8)
    sequence = output / "budget.sequence.json"
    write_json(sequence, dict(schemaVersion=1, frames=sample.frame, sampleFrames=[sample.frame],
        camera=[dict(frame=1, position=[0, 0, sample.distance], target=[0, 0, 0])],
        events=[dict(frame=sample.frame, depthProbe=list(sample.pixel))]))
    flags = ("--geometry-lod-error", str(PIXEL_ERROR))
    normal = capture(renderer, output / "bim-budget-uncapped", fixture, technique,
                     sequence, [sample], flags)[sample]
    limited = capture(renderer, output / "bim-budget-capped", fixture, technique,
                      sequence, [sample], (*flags, "--asset-memory-mb", "1"))[sample]
    normal_lod, limited_lod = (capture["state"]["geometryLod"]["bim"] for capture in (normal, limited))
    normal_budget, limited_budget = (capture["state"]["assetResidency"] for capture in (normal, limited))
    base_triangles = len(indices) // 3
    cap = 1024 * 1024
    require(normal_lod["baseTriangles"] == base_triangles and normal_lod["triangles"] < base_triangles and
            normal_lod["extraIndexBytes"] > 0 and normal_budget["used"]["residentBytes"] > cap,
            f"Uncapped BIM fixture did not exceed the tested budget with its generated levels: {normal_lod}")
    require(limited_lod["baseTriangles"] == limited_lod["triangles"] == base_triangles and
            limited_lod["extraIndexBytes"] == 0 and limited_budget["limits"]["residentBytes"] == cap and
            0 < limited_budget["used"]["residentBytes"] <= limited_budget["peak"]["residentBytes"] <= cap,
            f"The capped BIM provider did not admit the exact source on its own: {limited_lod}")
    picks = [capture["state"]["depthProbe"]["sceneInteraction"]["bim"] for capture in (normal, limited)]
    require(picks[0] and picks[0] == picks[1] and picks[0]["guid"] == "geometry-lod-budget",
            f"The budget fallback changed the picked BIM product: {picks}")
    return {key: dict(lod=lod, assetResidency=budget, image=str(entry["image"]), telemetry=str(entry["telemetry"]))
            for key, lod, budget, entry in (("uncapped", normal_lod, normal_budget, normal),
                                            ("capped", limited_lod, limited_budget, limited))}


def write_displacement_fixtures(directory):
    from PIL import Image, ImageDraw

    height_map = directory / "height.png"
    image = Image.new("RGBA", (32, 4), (0, 0, 0, 255))
    ImageDraw.Draw(image).rectangle((8, 0, 23, 3), fill=(255, 255, 255, 255))
    image.save(height_map)

    asset = Asset()
    asset.model.update(scenes=[dict(nodes=[0, 1])], nodes=[], images=[dict(uri=height_map.name)],
                       textures=[dict(source=0, sampler=0)],
                       samplers=[dict(minFilter=9728, magFilter=9728, wrapS=33071, wrapT=33071)])
    for panel, center in enumerate(PANEL_CENTERS):
        points, indices = folded_panel(center)
        uvs = [((x - center) / 3 + .5, y / 2.25 + .5) for x, y, _ in points]
        attributes = dict(POSITION=asset.accessor("VEC3", points),
                          NORMAL=asset.accessor("VEC3", [(0, 0, 1)] * len(points)),
                          TEXCOORD_0=asset.accessor("VEC2", uvs))
        asset.model["nodes"].append(dict(name=PANEL_GUIDS[panel], mesh=panel))
        asset.model["meshes"].append(dict(primitives=[dict(attributes=attributes, material=panel,
            indices=asset.accessor("SCALAR", indices, 5123, indices=True))]))
        asset.model["materials"].append(dict(name=PANEL_GUIDS[panel], heightTexture=dict(index=0),
            heightScale=0.0, pbrMetallicRoughness=dict(baseColorFactor=[*PANEL_COLORS[panel], 1],
                                                     metallicFactor=0, roughnessFactor=.85)))

    flat = directory / "zero-height-scale.gltf"
    asset.write(flat)
    displaced = directory / "active-height-scale.gltf"
    asset.model["materials"][0]["heightScale"] = 1.5
    asset.write(displaced)
    return flat, displaced


def verify_displacement_fallback(renderer, output, techniques):
    flat, displaced = write_displacement_fixtures(output)
    sample = Sample(0, "displacement", 25.0, 0, 8)
    sequence = output / "displacement.sequence.json"
    write_json(sequence, dict(schemaVersion=1, frames=sample.frame, sampleFrames=[sample.frame],
        camera=[dict(frame=1, position=[0, 0, sample.distance], target=[0, 0, 0])],
        events=[dict(frame=sample.frame, depthProbe=list(sample.pixel))]))
    captures = (("zero-scale", flat, "0"), ("full-displaced", displaced, "0"),
                ("lod-displaced", displaced, "64"))
    results = {}
    for technique in techniques:
        entries = {}
        for label, fixture, tolerance in captures:
            entry = capture(renderer, output / f"{technique}-{label}", fixture, technique,
                            sequence, [sample], ("--geometry-upload-kb", "0", "--geometry-lod-error", tolerance))[sample]
            state = entry["state"]
            entries[label] = dict(depth=state["depthProbe"]["opaque"], pick=identity(state, sample, "gltf"),
                                  lod=lod_stats(state, "gltf"), image=str(entry["image"]),
                                  telemetry=str(entry["telemetry"]))
        zero_scale, full, active = (entries[label] for label, _, _ in captures)
        require(full["depth"] > zero_scale["depth"] * 1.02,
                f"Height texture did not visibly displace the reference panel: {entries}")
        require(active["lod"]["extraIndexBytes"] > 0 and active["lod"]["levels"][0] == 1 and
                sum(active["lod"]["levels"][1:]) == 1 and
                FULL_TRIANGLES < active["lod"]["triangles"] < full["lod"]["triangles"],
                f"Only the zero-scale panel should use a reduced draw: {active['lod']}")
        require(active["pick"] == full["pick"] and
                math.isclose(active["depth"], full["depth"], rel_tol=1e-6, abs_tol=1e-8),
                f"LOD changed the displaced panel's depth or pick: {entries}")
        results[technique] = entries
    return results


def comparison(path, cases):
    from PIL import Image, ImageDraw

    panel_width, panel_height, label_height = 320, 200, 22
    canvas = Image.new("RGB", (4 * panel_width, len(cases) * (panel_height + label_height)), "#171717")
    labels = ImageDraw.Draw(canvas)
    for row, (name, case) in enumerate(cases.items()):
        outbound = [sample for sample in case["samples"] if sample["direction"] == "out" and sample["panel"] == 0]
        close = outbound[0]
        distant = next(sample for sample in outbound if sample["lod"]["triangles"] < sample["full"]["triangles"])
        for column, (sample, mode) in enumerate(((close, "full"), (close, "lod"), (distant, "full"), (distant, "lod"))):
            x, y = column * panel_width, row * (panel_height + label_height)
            label = f"{name}  {sample['distance']:g} m  {mode}  {sample[mode]['triangles']:,} tris"
            labels.text((x + 6, y + 4), label, fill="white")
            with Image.open(sample[f"{mode}Capture"]) as image:
                if sample is distant:
                    focal = HEIGHT / (2 * math.tan(math.radians(FOV / 2)))
                    margin = int(4 * focal / sample["distance"]) + 4
                    height = round(margin * HEIGHT / WIDTH)
                    image = image.crop((WIDTH // 2 - margin, HEIGHT // 2 - height,
                                        WIDTH // 2 + margin, HEIGHT // 2 + height))
                canvas.paste(image.convert("RGB").resize((panel_width, panel_height), Image.Resampling.NEAREST),
                             (x, y + label_height))
    canvas.save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--provider", choices=("gltf", "bim", "usd"), action="append")
    parser.add_argument("--technique", choices=TECHNIQUES, action="append")
    parser.add_argument("--displacement-only", action="store_true")
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--require-opt-in", action="store_true")
    args = parser.parse_args()
    if args.require_opt_in and os.environ.get(OPT_IN) != "1":
        print(f"SKIP: set {OPT_IN}=1 to run native geometry LOD captures")
        return 77

    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True, exist_ok=False)
    fixtures = {} if args.displacement_only else write_fixtures(output)
    providers = () if args.displacement_only else (args.provider or fixtures)
    techniques = args.technique or TECHNIQUES
    checksum = digest(renderer)
    report = dict(passed=False, rendererSha256=checksum, pixelError=PIXEL_ERROR, cases={})
    try:
        for provider in providers:
            control_sequence = output / f"{provider}-control.sequence.json"
            active_sequence = output / f"{provider}-active.sequence.json"
            # Give primary geometry's background worker and bounded index upload
            # time to publish before either matched sweep starts. Near samples
            # still require the full-detail selection and a resident LOD buffer.
            warmup = 20 if provider == "gltf" else 0
            control_samples = write_sequence(control_sequence, 1, bim_checkpoints=provider != "gltf",
                                             warmup_frames=warmup)
            active_samples = write_sequence(active_sequence, 2, bim_checkpoints=provider != "gltf",
                                            warmup_frames=warmup)
            for technique in techniques:
                name = f"{provider}-{technique}"
                control = capture(renderer, output / f"{name}-full", fixtures[provider], technique,
                                  control_sequence, control_samples, ("--geometry-lod-error", "0"))
                active = capture(renderer, output / f"{name}-lod", fixtures[provider], technique,
                                 active_sequence, active_samples, ("--geometry-lod-error", str(PIXEL_ERROR)))
                report["cases"][name] = verify(control, active, provider)
        if "bim" in providers:
            report["bimBudgetFallback"] = verify_bim_budget_fallback(renderer, output, techniques[0])
        if args.displacement_only or "gltf" in providers:
            report["displacementFallback"] = verify_displacement_fallback(renderer, output, techniques)
        if report["cases"]:
            comparison_path = output / "comparison.png"
            comparison(comparison_path, report["cases"])
            report["comparison"] = str(comparison_path)
        require(digest(renderer) == checksum, "Renderer binary changed during the captures")
        report["passed"] = True
    finally:
        write_json(output / "results.json", report)
    print(f"PASS geometry LOD validation in {output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
