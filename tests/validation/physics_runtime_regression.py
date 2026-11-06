"""Opt-in native rigid-body/render integration and unload regression.

Uses the renderer's deterministic capture loop and normal GUI unload queue.
Requires a Vulkan display/runtime plus Pillow, just like the model lifecycle
captures. CPU collision, transform and real ImGui input tests run separately.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path

def check_floor_shadow_footprint(raster_record, ray_record):
    """Reject remote floor occlusion without constraining legitimate contacts.

    The demo consists of unit cubes and a scaled cube floor. Project those
    recorded transforms, including the cubes' actual directional-light shadow
    bounds, instead of choosing an image rectangle that could hide a defect.
    """
    try:
        import numpy as np
        from PIL import Image, ImageDraw, ImageFilter
    except ImportError:
        print("SKIP: physics floor-image validation requires Pillow and NumPy")
        raise SystemExit(77)
    raster = next(sample for sample in raster_record["samples"] if sample["frame"] == 180)
    rays = next(sample for sample in ray_record["samples"] if sample["frame"] == 180)
    telemetry = raster["telemetry"]
    with Image.open(raster["png"]) as source:
        reference = source.convert("RGB")
    with Image.open(rays["png"]) as source:
        traced = source.convert("RGB")
    assert reference.size == traced.size, "Floor comparison image sizes differ"
    width, height = reference.size
    camera = np.asarray(telemetry["camera"]["unjitteredViewProjColumns"], dtype=float).T
    assert camera.shape == (4, 4) and np.isfinite(camera).all(), "Invalid floor comparison camera"
    ray_camera = np.asarray(rays["telemetry"]["camera"]["unjitteredViewProjColumns"], dtype=float).T
    assert ray_camera.shape == (4, 4) and np.isfinite(ray_camera).all()
    assert np.allclose(camera, ray_camera, atol=1e-5, rtol=0), "Floor comparison cameras differ"

    def project(point):
        clip = camera @ np.append(point, 1.0)
        assert np.isfinite(clip).all() and clip[3] > 0, "Floor lies behind comparison camera"
        ndc = clip[:2] / clip[3]
        # Rendering uses a negative-height Vulkan viewport.
        return (float((ndc[0] * .5 + .5) * width),
                float((.5 - ndc[1] * .5) * height))

    def world(body, point):
        matrix = np.asarray(body["renderWorldMatrix"], dtype=float).reshape(4, 4).T
        assert np.isfinite(matrix).all(), "Invalid floor comparison body transform"
        return (matrix @ np.append(point, 1.0))[:3]

    bodies = telemetry["physics"]["bodies"]
    floor = next(body for body in bodies if body["name"] == "Physics floor")
    floor_points = [world(floor, point) for point in
                    ((-.5, .5, -.5), (.5, .5, -.5), (.5, .5, .5), (-.5, .5, .5))]
    floor_y = floor_points[0][1]
    assert max(abs(point[1] - floor_y) for point in floor_points) < 1e-5
    mask = Image.new("L", reference.size)
    ImageDraw.Draw(mask).polygon([project(point) for point in floor_points], fill=255)
    # Boundary rasterization, TAA jitter and filtering may disagree by a pixel.
    mask = mask.filter(ImageFilter.MinFilter(7))
    direction = np.asarray(telemetry["lighting"]["directionalDirection"], dtype=float)
    assert direction.shape == (3,) and np.isfinite(direction).all() and direction[1] < -1e-5
    excluded = Image.new("L", reference.size)
    draw = ImageDraw.Draw(excluded)
    for body in bodies:
        if body["name"] == "Physics floor":
            continue
        vertices = [world(body, (x, y, z)) for x in (-.5, .5)
                    for y in (-.5, .5) for z in (-.5, .5)]
        projected_shadows = [point + direction * ((floor_y - point[1]) / direction[1])
                             for point in vertices]
        pixels = np.asarray([project(point) for point in vertices + projected_shadows])
        draw.rectangle((pixels[:, 0].min(), pixels[:, 1].min(),
                        pixels[:, 0].max(), pixels[:, 1].max()), fill=255)
    excluded = excluded.filter(ImageFilter.MaxFilter(9))
    weights = np.asarray((.2126, .7152, .0722))
    baseline = np.asarray(reference, dtype=float) @ weights
    traced_luminance = np.asarray(traced, dtype=float) @ weights
    valid = (np.asarray(mask) > 0) & (np.asarray(excluded) == 0) & (baseline > 32)
    count = int(valid.sum())
    assert count >= 250, "Too little lit floor remains to validate remote shadows"
    extra_dark = valid & (traced_luminance < baseline * .6) & (baseline - traced_luminance > 16)
    dark_count = int(extra_dark.sum())
    fraction = dark_count / count
    metrics = {"passed": fraction <= .05, "comparisonPixels": count,
               "extraDarkPixels": dark_count, "extraDarkFraction": fraction,
               "maxExtraDarkFraction": .05}
    ray_record.setdefault("checks", {})["floorShadowFootprint"] = metrics
    assert metrics["passed"], (f"Oversized ray-shadow floor footprint: {dark_count}/{count} "
                                f"remote floor pixels ({fraction:.2%}) differ from raster; limit 5%")
    return metrics


def check(record):
    samples = {sample["frame"]: sample for sample in record["samples"]}
    assert sorted(samples) == [1, 30, 180, 184], "Incomplete physics captures"
    states = [samples[frame]["telemetry"]["physics"] for frame in (1, 30, 180)]
    for state in states:
        assert state["available"] and state["initialized"] and state["playing"]
        assert state["bodyCount"] == 4 and len(state["bodies"]) == 4
        assert abs(state["fixedStepSeconds"] - 1 / 60) < 1e-10
        assert state["droppedSeconds"] == 0
        for body in state["bodies"]:
            graph, rendered = body["sceneWorldMatrix"], body["renderWorldMatrix"]
            assert len(graph) == len(rendered) == 16, "Physics body has no rendered object"
            assert max(abs(a - b) for a, b in zip(graph, rendered)) < 1e-5, "Stale render transform"
            assert max(abs(rendered[12 + i] - body["position"][i]) for i in range(3)) < 1e-4, \
                "Centered demo collider and rendered mesh disagree"
    assert states[0]["totalSteps"] < states[1]["totalSteps"] < states[2]["totalSteps"]
    bodies = [{body["name"]: body for body in state["bodies"]} for state in states]
    assert bodies[0]["Physics floor"] == bodies[2]["Physics floor"], "Static floor moved"
    for name in ("Physics box 1", "Physics box 2", "Physics box 3"):
        first, falling, settled = (state[name] for state in bodies)
        assert first["motion"] == falling["motion"] == settled["motion"] == 2
        assert falling["position"][1] < first["position"][1] - 0.5, "Gravity did not move the mesh"
        assert 0.45 < settled["position"][1] < 0.6, "Box failed to settle on the floor"
    images = [hashlib.sha256(Path(samples[frame]["png"]).read_bytes()).hexdigest()
              for frame in (1, 30, 180)]
    assert len(set(images)) == 3, "Renderer did not publish changing physics transforms"
    unloaded = samples[184]["telemetry"]
    assert unloaded["physics"]["bodyCount"] == 0
    assert unloaded["scene"]["primaryObjectCount"] == 0, "Unload retained physics geometry"
    if record["mode"] == "rays-taa":
        assert samples[180]["telemetry"]["rayShadows"]["active"], "Ray-shadow path was not exercised"
        assert samples[180]["telemetry"]["taa"]["enabled"], "Temporal path was not exercised"
    return {"passed": True, "fixedSteps": states[2]["totalSteps"],
            "settledHeight": [bodies[2][f"Physics box {i}"]["position"][1] for i in (1, 2, 3)],
            "imageSha256": images, "unloadRetiredBodies": True}


def recheck_existing(output):
    """Apply current checks to saved captures without starting the renderer."""
    report = output / "results.json"
    results = json.loads(report.read_text(encoding="utf-8"))
    results["passed"] = False
    results.pop("failure", None)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    try:
        cases = {(record["technique"], record["mode"]): record for record in results["cases"]}
        assert len(cases) == len(results["cases"]) == 4, "Incomplete or duplicate physics cases"
        for record in cases.values():
            for sample in record["samples"]:
                # Copies of evidence retain the original absolute capture path.
                # Prefer the copy next to this report when it is available.
                local = output / Path(sample["png"]).name
                if local.is_file():
                    sample["png"] = str(local)
            record["checks"] = check(record)
        for technique in ("deferred-raster", "forward-raster"):
            check_floor_shadow_footprint(cases[(technique, "raster")], cases[(technique, "rays-taa")])
        results["passed"] = True
    except Exception as error:
        results["failure"] = str(error)
        print(f"Physics capture validation failed: {error}", flush=True)
    report.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0 if results["passed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--require-opt-in", action="store_true")
    parser.add_argument("--check-existing", action="store_true",
                        help="Revalidate saved results and images without launching the renderer")
    args = parser.parse_args()
    if args.check_existing:
        return recheck_existing(args.output.resolve())
    if args.require_opt_in and os.environ.get("CONTAINER_RUN_GPU_PHYSICS") != "1":
        print("Set CONTAINER_RUN_GPU_PHYSICS=1 to run native physics captures")
        return 77
    if args.exe is None:
        parser.error("--exe is required when capturing")
    from gui_model_loading_regression import run_lifecycle_capture

    exe, output = args.exe.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    (output / "imgui.ini").write_text(
        "[Window][Scene Controls]\nPos=0,0\nSize=350,500\nCollapsed=1\n\n"
        "[Window][Renderer Telemetry]\nPos=0,25\nSize=350,500\nCollapsed=1\n")
    source = exe.parent / "models/validation/cornell_box_local_light.gltf"
    assert exe.is_file() and source.is_file(), "Build the application and assets first"
    sequence = {"schemaVersion": 1, "frames": 184, "sampleFrames": [1, 30, 180, 184],
                "events": [{"frame": 181, "guiUnload": True}]}
    results = {"schemaVersion": 1, "passed": False,
               "executableSha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "cases": []}
    (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    try:
        for technique in ("deferred-raster", "forward-raster"):
            raster_record = None
            for mode in ("raster", "rays-taa"):
                name = f"physics-{technique}-{mode}"
                record = run_lifecycle_capture(exe, output, name, source, sequence, technique, mode,
                                               extra=("--physics-demo",))
                results["cases"].append(record)
                record["checks"] = check(record)
                if mode == "raster":
                    raster_record = record
                else:
                    check_floor_shadow_footprint(raster_record, record)
                print(json.dumps({"case": name, **record["checks"]}), flush=True)
                (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    except Exception as error:
        results["failure"] = str(error)
        (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
        raise
    assert hashlib.sha256(exe.read_bytes()).hexdigest() == results["executableSha256"]
    results["passed"] = True
    (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
