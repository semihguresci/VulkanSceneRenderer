"""Measure automatic DDGI room placement and light leakage with native Vulkan."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from display_capture_test_support import validation_log_error


QUALITY_HARNESS = Path(__file__).resolve().parents[1] / "visual-regression" / "ddgi"
sys.path.insert(0, str(QUALITY_HARNESS))
from quality_fixtures import box, build_scene, gltf_scene
from run_quality import linear_receiver_metrics


WIDTH, HEIGHT = 480, 320
TECHNIQUE = "deferred-raster"
PRIMARY = {"origin": [-3., 0., -2.], "spacing": [.75, .75, .75], "counts": [9, 5, 5]}
REFERENCE_DETAIL = {"origin": [-.25, -.25, -2.25], "spacing": [.25, .25, .25], "counts": [15, 15, 15]}
MEMORY_MB = 48
MAX_DETAIL_PROBES = 2048
RAYS, UPDATE_PROBES = 128, 256
WARM = 160
FRAMES = {
    "baseline": WARM,
    "tight-preview": WARM + 1,
    "tight-rejected": WARM + 2,
    "discarded": WARM + 3,
    "stale-preview": WARM + 4,
    "stale-rejected": WARM + 6,
    "preview": WARM + 8,
    "first-applied": WARM + 9,
    "settled": 2 * WARM + 9,
}
ROOMS = {
    "Shaded chamber": [[.015, 0., -2.], [3., 2.8, 1.]],
    "Upper mapped room": [[-.25, 4., -1.], [2.25, 6.3, 2.5]],
    "West annex": [[-11., 5., -2.], [-7., 7.5, 2.]],
    "Far upper room": [[8., 5., -2.], [12., 7.6, 2.]],
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def guid(number):
    return f"{number:022d}"


def source_axes(point):
    x, y, z = point
    return [1000. * x, -1000. * z, 1000. * y]


def ifcx_mesh(bounds):
    faces = box("Space bounds", *bounds)
    points = [source_axes(point) for face in faces for point in face["corners"]]
    return {"points": points, "faceVertexCounts": [4] * len(faces), "faceVertexIndices": list(range(len(points)))}


def room_attributes(label, number, floor):
    return {"bsi::ifc::class": "IfcSpace", "bsi::ifc::name": label, "bsi::ifc::globalId": guid(number),
            "bsi::ifc::storeyId": guid(100 + floor), "bsi::ifc::storeyName": f"Level {floor}",
            "gltf::material": {"alphaMode": "BLEND", "pbrMetallicRoughness": {"baseColorFactor": [1., 1., 1., 0.]}}}


def write_bim_rooms(path):
    records = []

    def space(path, label, number, floor, bounds):
        attributes = room_attributes(label, number, floor)
        attributes["usd::usdgeom::mesh"] = ifcx_mesh(bounds)
        records.append(dict(path=path, attributes=attributes))

    minimum, maximum = ROOMS["Shaded chamber"]
    space("ground-rear", "Shaded chamber", 1, 0, [minimum, [maximum[0], maximum[1], -.5]])
    space("ground-front", "Shaded chamber", 1, 0, [[minimum[0], minimum[1], -.5], maximum])

    # IFCX instantiates the referenced body. Its authored +X and +Y axes
    # rotate 90 degrees about +Z before the millimetre-to-metre conversion.
    attributes = room_attributes("Upper mapped room", 2, 1)
    attributes["usd::xformop"] = {"transform": [[0., 1., 0., 0.], [-1., 0., 0., 0.], [0., 0., 1., 0.],
                                                     [2250., 1000., 4000., 1.]]}
    records.append(dict(path="upper-room", attributes=attributes, children={"Body": "mapped-room-body"}))
    records.append(dict(path="mapped-room-body", attributes={"usd::usdgeom::mesh":
                        ifcx_mesh([[-3.5, 0., -2.5], [0., 2.3, 0.]])}))

    space("west-annex", "West annex", 3, 1, ROOMS["West annex"])
    space("far-room", "Far upper room", 4, 1, ROOMS["Far upper room"])
    attributes = room_attributes("Boundary survey only", 5, 1)
    attributes["points::array"] = {"positions": [[0., 0., 4500.], [100., 100., 4550.]]}
    records.append(dict(path="boundary-points", attributes=attributes))
    space("conflict-low", "Conflicting room", 6, 0, [[20., 0., -1.], [21., 1., 1.]])
    space("conflict-high", "Conflicting room", 6, 1, [[20., 5., -1.], [21., 6., 1.]])
    write_json(path, {"data": records})


def write_inputs(output):
    scene = build_scene("sealed-thin", WIDTH, HEIGHT)
    model, bim = output / "sealed-thin.gltf", output / "storeys.ifcx"
    write_json(model, gltf_scene(scene))
    write_bim_rooms(bim)
    sequence = output / "plan.sequence.json"
    events = []

    def action(frame, operation, **fields):
        events.append(dict(frame=FRAMES[frame], ddgiRoomPlan=dict(action=operation, **fields)))

    action("tight-preview", "preview", maxDetailProbes=27)
    action("tight-rejected", "apply", expectFailure=True)
    action("discarded", "discard")
    action("stale-preview", "preview", maxDetailProbes=MAX_DETAIL_PROBES)
    events.append(dict(frame=FRAMES["stale-rejected"] - 1, ddgiIntensity=.5))
    action("stale-rejected", "apply", expectFailure=True)
    events.append(dict(frame=FRAMES["preview"] - 1, ddgiIntensity=1.))
    action("preview", "preview", maxDetailProbes=MAX_DETAIL_PROBES)
    action("first-applied", "apply")
    write_json(sequence, dict(schemaVersion=1, frames=FRAMES["settled"],
                              sampleFrames=[FRAMES[name] for name in ("baseline", "preview", "settled")],
                              telemetryFrames=list(FRAMES.values()), events=events))
    return scene, model, bim, sequence


def renderer_command(renderer, scene, model, bim, base, frames):
    camera = scene["camera"]
    return [str(renderer), "--model", str(model), "--bim-model", str(bim), "--import-scale", "1",
            "--bim-import-scale", ".001", "--hidden", "--no-ui", "--validation", "--no-bloom", "--no-taa",
            "--msaa", "1", "--width", str(WIDTH), "--height", str(HEIGHT),
            "--render-technique", TECHNIQUE, "--display-mode", "lit", "--sky", "plain",
            "--sky-horizon", "0", "0", "0", "--camera-position", *map(str, camera["position"]),
            "--camera-target", *map(str, camera["target"]), "--camera-fov", str(camera["fov"]),
            "--environment-intensity", "0", "--directional-intensity", "0", "--bounce-intensity", "0",
            "--ray-shadows", "hard", "--no-ray-shadow-denoise", "--area-lighting", "sampled", "--area-light-samples", "64",
            "--ddgi", "--ddgi-intensity", "1", "--ddgi-origin", *map(str, PRIMARY["origin"]),
            "--ddgi-spacing", *map(str, PRIMARY["spacing"]), "--ddgi-counts", *map(str, PRIMARY["counts"]),
            "--ddgi-rays", str(RAYS), "--ddgi-probes-per-frame", str(UPDATE_PROBES),
            "--ddgi-ray-budget", str(RAYS * UPDATE_PROBES), "--ddgi-memory-mb", str(MEMORY_MB),
            "--ddgi-max-distance", "12", "--no-ddgi-async", "--no-ddgi-camera-follow", "--fixed-dt", ".016666667",
            "--warmup-frames", str(frames - 1), "--capture-frame", str(frames), "--screenshot-viewport", "--screenshot", str(base)]


def run(renderer, output, command, base):
    log = base.with_suffix(".log")
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=renderer.parent, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                stdout=stream, stderr=subprocess.STDOUT, timeout=240,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    text = log.read_text(encoding="utf-8", errors="replace")
    require(result.returncode == 0 and not validation_log_error(text), f"Native DDGI capture failed: {log}")
    if os.environ.get("VK_LOADER_DEBUG") == "layer":
        require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text, f"Khronos validation missing: {log}")


def capture_path(base, frame):
    return base if frame == FRAMES["settled"] else base.with_name(f"{base.stem}.frame-{frame:04d}{base.suffix}")


def read_state(path, model, bim):
    state = json.loads(path.with_suffix(".telemetry.json").read_bytes())
    require(state["validationEnabled"] and state["technique"] == TECHNIQUE and state["resolution"] == [WIDTH, HEIGHT],
            f"Unexpected capture route: {path}")
    require(Path(state["scene"]["primary"]).resolve() == model and Path(state["scene"]["auxiliary"]).resolve() == bim,
            f"Both fixture providers must be present: {path}")
    return state


def runtime_volumes(settings):
    return [{key: settings[key] for key in PRIMARY}, *settings["additionalVolumes"]]


def element_coverage(bounds, volumes, band_cells):
    for volume in volumes:
        if all(bounds[0][axis] - volume["origin"][axis] >= volume["spacing"][axis] * band_cells - 1e-5 and
               volume["origin"][axis] + volume["spacing"][axis] * (volume["counts"][axis] - 1) - bounds[1][axis] >=
                   volume["spacing"][axis] * band_cells - 1e-5 for axis in range(3)):
            return True
    return False


def check_planning(states):
    import numpy as np

    results = {label: state["ddgi"]["roomPlan"] for label, state in states.items()}
    preview, tight = results["preview"], results["tight-preview"]
    require(results["baseline"]["state"] == "idle" and results["discarded"]["state"] == "idle", results)
    require(tight["state"] == "preview" and not tight["volumes"] and
            any(room["status"] == "uncovered" for room in tight["rooms"]), tight)
    require(results["tight-rejected"]["state"] == "rejected" and results["tight-rejected"]["diagnostic"], results)
    stale = results["stale-rejected"]
    require(results["stale-preview"]["state"] == "preview" and stale["state"] == "rejected" and
            not stale["volumes"] and "settings changed" in stale["diagnostic"], stale)
    require(preview["state"] == "preview" and 0 < len(preview["volumes"]) <= 3, preview)
    require(results["first-applied"]["state"] == results["settled"]["state"] == "applied", results)
    require(math.isclose(preview["effectiveImportScale"], .001, abs_tol=1e-8), preview)
    baseline = states["baseline"]["ddgi"]
    require(runtime_volumes(baseline["settings"]) == [PRIMARY], "Wrong coarse primary grid")
    for label in ("tight-preview", "tight-rejected", "discarded", "stale-preview", "stale-rejected", "preview"):
        current = states[label]["ddgi"]
        settings = baseline["settings"] | {"intensity": .5} if label == "stale-rejected" else baseline["settings"]
        require(current["settings"] == settings and current["historyResets"] == baseline["historyResets"] and
                current["totalAllocatedBytes"] == baseline["totalAllocatedBytes"],
                f"Preview mutated the active grid or history at {label}")
    planned = [{key: volume[key] for key in PRIMARY} for volume in preview["volumes"]]
    for label in ("first-applied", "settled"):
        gi = states[label]["ddgi"]
        require(runtime_volumes(gi["settings"]) == [PRIMARY, *planned],
                f"The applied lattices differ from the room proposal at {label}")
        require(0 < gi["totalAllocatedBytes"] <= MEMORY_MB * 1024 ** 2 and
                gi["totalAllocatedBytes"] > baseline["totalAllocatedBytes"], "Applied DDGI allocation is outside its budget")
    by_label = {room["label"]: room for room in preview["rooms"]}
    require(len(by_label) == len(preview["rooms"]) == 6, by_label)
    require(by_label["Boundary survey only"]["status"] == "missing-geometry" and
            by_label["Conflicting room"]["status"] == "ambiguous", by_label)
    for room, bounds in ROOMS.items():
        entry = by_label[room]
        np.testing.assert_allclose([entry["boundsMin"], entry["boundsMax"]], bounds, atol=1e-4, rtol=0, err_msg=room)
        require(not element_coverage(bounds, [PRIMARY], 1.), f"{room} is already covered in baseline")
        status = entry["status"]
        covered = element_coverage(bounds, planned, 1.)
        require(status == ("covered-planned" if covered else "uncovered"), entry)
        if covered:
            index = entry["volumeIndex"]
            volume, = (volume for volume in preview["volumes"] if volume["volumeIndex"] == index)
            require(element_coverage(bounds, [volume], 1.) and max(volume["spacing"]) < min(PRIMARY["spacing"]),
                    f"{room} was assigned a coarse or unpadded volume")
    require(by_label["Shaded chamber"]["status"] == "covered-planned" and
            by_label["Upper mapped room"]["status"] == "covered-planned", by_label)
    require(any(room["status"] == "uncovered" for room in by_label.values()), by_label)
    require(preview["baseProbeCount"] == math.prod(PRIMARY["counts"]) and
            preview["totalProbeCount"] == sum(math.prod(volume["counts"]) for volume in (PRIMARY, *planned)) and
            all(volume["probeCount"] == math.prod(volume["counts"]) <= MAX_DETAIL_PROBES for volume in preview["volumes"]),
            f"Invalid proposed probe counts: {preview}")
    require(0 < preview["basePayloadBytes"] < preview["payloadBytes"] <= MEMORY_MB * 1024 ** 2 and
            preview["framesPerProbeCycle"] >= math.ceil(preview["totalProbeCount"] / UPDATE_PROBES), preview)
    return preview


def check_ddgi(state):
    gi = state["ddgi"]
    require(gi["supported"] and gi["enabled"] and gi["active"] and state["rayShadows"]["active"],
            "Native ray query / DDGI was requested but inactive")
    require(0 < gi["updatedProbes"] <= UPDATE_PROBES and gi["tracedRays"] == RAYS * gi["updatedProbes"] and
            gi["refreshRemainingProbes"] == 0 and gi["updateFrame"] >= 32, gi)
    for name in ("DdgiTrace", "DdgiUpdate"):
        entry, = (entry for entry in state["passes"] if entry["name"] == name)
        require(entry["active"] and entry["gpuTimed"] and entry["gpuMs"] > 0., entry)


def measure(path, scene, state):
    info = json.loads(Path(f"{path}.capture.json").read_bytes())
    require(info["source"] == "raster-scene-composite" and info["colorSpace"] == "linear-sRGB" and
            not info["includesUi"] and not info["toneMappingApplied"], f"Invalid EXR sources: {info}")
    return linear_receiver_metrics(path, scene, state["camera"])


def compare(samples):
    baseline, preview, fitted, reference = (samples[key] for key in ("baseline", "preview", "settled", "reference"))
    luminance = lambda record, name: record[name]["estimatedLuminance"]
    lit, shaded = "lit-back", "shaded-back"
    base_lit, base_dark = luminance(baseline, lit), luminance(baseline, shaded)
    require(base_lit > .005 and base_dark > .001, "Expected coarse light leak and a legitimately lit receiver")
    for receiver in baseline:
        require(abs(luminance(preview, receiver) - luminance(baseline, receiver)) < .01 * base_lit,
                f"The preview changed a measured receiver before Apply: {receiver}")
    plan_lit, plan_dark = luminance(fitted, lit), luminance(fitted, shaded)
    oracle_dark = luminance(reference, shaded)
    require(plan_lit >= .75 * base_lit and luminance(reference, lit) >= .75 * base_lit,
            "Detail placement removed legitimate room light")
    require(plan_dark <= .8 * base_dark and abs(plan_dark - oracle_dark) < abs(base_dark - oracle_dark),
            f"No measured leakage improvement toward the dense reference: {base_dark}, {plan_dark}, {oracle_dark}")
    return {"coarseLeakToLitRatio": base_dark / base_lit, "plannedLeakToLitRatio": plan_dark / plan_lit,
            "denseReferenceLeakToLitRatio": oracle_dark / luminance(reference, lit),
            "retainedLitFraction": plan_lit / base_lit, "remainingCoarseLeakFraction": plan_dark / base_dark}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args()
    if not args.generate_only and os.environ.get("CONTAINER_RUN_GPU_DDGI") != "1":
        print("SKIP: set CONTAINER_RUN_GPU_DDGI=1 to run native DDGI")
        return 77
    renderer, output = args.renderer.resolve(), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True)
    scene, model, bim, sequence = write_inputs(output)
    if args.generate_only:
        return 0
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    base, preset = output / "plan.exr", output / "baseline-ddgi.json"
    command = renderer_command(renderer, scene, model, bim, base, FRAMES["settled"])
    command += ["--capture-sequence", str(sequence), "--ddgi-save-preset", str(preset)]
    run(renderer, output, command, base)
    states = {label: read_state(capture_path(base, frame), model, bim) for label, frame in FRAMES.items()}
    planned = check_planning(states)
    samples = {label: measure(capture_path(base, FRAMES[label]), scene, states[label])
               for label in ("baseline", "preview", "settled")}
    reference, ref_preset = output / "dense-reference.exr", output / "dense-reference-ddgi.json"
    settings = json.loads(preset.read_bytes())
    settings["settings"]["additionalVolumes"] = [REFERENCE_DETAIL]
    write_json(ref_preset, settings)
    ref_command = renderer_command(renderer, scene, model, bim, reference, WARM)
    ref_command += ["--ddgi-preset", str(ref_preset)]
    run(renderer, output, ref_command, reference)
    ref_state = read_state(reference, model, bim)
    require(runtime_volumes(ref_state["ddgi"]["settings"]) == [PRIMARY, REFERENCE_DETAIL],
            "The dense reference did not use the independently authored lattice")
    for state in (states["baseline"], states["settled"], ref_state):
        check_ddgi(state)
    samples["reference"] = measure(reference, scene, ref_state)
    results = {"revision": states["settled"]["build"]["revision"], "technique": TECHNIQUE,
               "executableSha256": hashlib.sha256(renderer.read_bytes()).hexdigest(),
               "plan": planned, "receivers": samples, "commands": [command, ref_command],
               "limitations": ["Four projected receiver patches in a closed single-sided test partition.",
                               "The dense lattice is a DDGI comparison, not independent light transport ground truth.",
                               "A reported room bounds fit does not guarantee live probe support or zero leakage."]}
    try:
        results["lightingComparison"] = compare(samples)
    finally:
        write_json(output / "results.json", results)
    print(f"PASS DDGI room planning and applied coverage ({output})", flush=True)
    print(results["lightingComparison"], flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
