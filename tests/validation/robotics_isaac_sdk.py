"""Optional Isaac Sim 6.2.0 / Isaac Lab 3.0.0 GA interoperability checks.

Importing this module does not import OpenUSD or start Kit. ``run_sdk`` belongs
in a one-shot SDK Python process: owned Kit shutdown waits until its evidence
has been written, and its exit status is allowed to reach the worker.
No OpenUSD-only substitute is used when a requested SDK is unavailable.
"""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import struct
import sys
from time import monotonic
import traceback
import zlib


POSITION_TOLERANCE_METERS = 2e-5
ANGLE_TOLERANCE_RADIANS = 2e-5
RAY_TOLERANCE_METERS = 2e-5
RESOLUTION = (256, 256)
MAX_LOAD_UPDATES = 180
MAX_QUERY_UPDATES = 180
MAX_RENDER_UPDATES = 60
PHASE_TIMEOUT_SECONDS = 60.0

REFERENCES = {
    "simulationApp": "https://docs.isaacsim.omniverse.nvidia.com/6.2.0/py/source/extensions/isaacsim.simulation_app/docs/index.html",
    "labAppLauncher": "https://isaac-sim.github.io/IsaacLab/v3.0.0/source/api/lab/isaaclab.app.html",
    "labLauncherSource": "https://github.com/isaac-sim/IsaacLab/blob/v3.0.0/source/isaaclab/isaaclab/app/app_launcher.py",
    "worldTransforms": "https://docs.omniverse.nvidia.com/kit/docs/omni.usd/1.19.12/omni.usd/omni.usd.get_world_transform_matrix.html",
    "physxQueries": "https://docs.omniverse.nvidia.com/kit/docs/omni_physics/110.1/extensions/runtime/source/omni.physx/docs/dev_guide/scene_queries.html",
    "replicator": "https://docs.omniverse.nvidia.com/kit/docs/omni_replicator/1.13.30/source/extensions/omni.replicator.core/docs/API.html",
    "semanticCompatibility": "https://nvidia.github.io/simready-foundation/latest/guides/semantic_labels/semantic_labels.html",
}


def _require(condition, message):
    if not condition:
        raise RuntimeError(message)


def _json_value(value):
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, dict):
        return {str(key): _json_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_value(item) for item in value]
    if type(value) is float and not math.isfinite(value):
        return repr(value)
    if value is None or type(value) in (bool, int, float, str):
        return value
    if hasattr(value, "tolist"):
        return _json_value(value.tolist())
    return str(value)


def _write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(_json_value(value), stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    temporary.replace(path)


def _fingerprint(path):
    digest = hashlib.sha256()
    size = 0
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
            size += len(block)
    return {"sha256": digest.hexdigest(), "byteSize": size}


def _check_vector(actual, expected, tolerance, label):
    _require(len(actual) == len(expected) and all(
        math.isfinite(float(value)) and math.isclose(float(value), float(target), rel_tol=0., abs_tol=tolerance)
        for value, target in zip(actual, expected)), f"{label}: expected {expected}, got {actual}")


def _ga_version(actual, expected, label):
    # Build metadata does not change the GA release. Pre-release tags do.
    core = str(actual).split("+", 1)[0]
    _require(core == expected, f"Unsupported {label} version {actual!r}; this lane targets {expected} GA")


def _close_app(app, report):
    # Lab 3.0's Kit proxy can terminate Python in close(). The SDK receipt must
    # already exist, and SystemExit must reach the worker instead of becoming
    # an ignored atexit exception with an accidental successful process status.
    code = 0 if report.get("passed") else 1
    app.close(exit_code=code)


def _launch(backend, report):
    common = {"headless": True, "width": RESOLUTION[0], "height": RESOLUTION[1], "multi_gpu": False}
    if backend == "isaac-sim":
        from isaacsim import SimulationApp

        # Start Kit first, then let its own pxr validate dependencies before
        # the requested bundle is opened in the live omni.usd context.
        app = SimulationApp({**common, "sync_loads": True, "fast_shutdown": False})
        launcher = app
    else:
        import isaaclab
        from isaaclab.app import AppLauncher

        report["isaacLabVersion"] = str(isaaclab.__version__)
        _ga_version(report["isaacLabVersion"], "3.0.0", "Isaac Lab")
        launcher = AppLauncher({**common, "enable_cameras": True, "device": "cpu", "livestream": 0})
        app = launcher.app
        # GA KitLauncher forwards an explicit allowlist of SimulationApp fields;
        # fast_shutdown is not in that list. The worker handles native exit.
    return app, launcher


def _enable_extension(app, name):
    import omni.kit.app

    manager = omni.kit.app.get_app().get_extension_manager()
    _require(manager.set_extension_enabled_immediate(name, True) is not False,
             f"Cannot enable required SDK extension {name}")
    app.update()


def _open_context_stage(app, stage_path):
    import omni.usd

    context = omni.usd.get_context()
    stage = context.get_stage()
    if stage is None or Path(stage.GetRootLayer().realPath).resolve() != stage_path:
        _require(context.open_stage(str(stage_path)) is not False, f"Kit could not open {stage_path}")
    deadline = monotonic() + PHASE_TIMEOUT_SECONDS
    for _ in range(MAX_LOAD_UPDATES):
        app.update()
        stage = context.get_stage()
        loading = context.get_stage_loading_status()
        if (stage is not None and Path(stage.GetRootLayer().realPath).resolve() == stage_path
                and loading[2] == 0):
            return stage
        if monotonic() >= deadline:
            break
    raise RuntimeError(f"Kit did not finish loading the requested stage {stage_path}")


def _seek(app, timeline, seconds):
    timeline.pause()
    timeline.set_current_time(seconds)
    timeline.commit()
    app.update()
    measured = float(timeline.get_current_time())
    _require(math.isfinite(measured) and math.isclose(measured, seconds, rel_tol=0., abs_tol=1e-9),
             f"Kit timeline did not seek: expected {seconds}, got {measured}")
    return measured


def _pose_witnesses(app, stage, bundle, report):
    import omni.timeline
    import omni.usd
    from pxr import Gf, Usd

    timeline = omni.timeline.get_timeline_interface()
    robot = stage.GetPrimAtPath("/World/ReplayRobot")
    _require(robot.IsValid(), "Kit stage lacks /World/ReplayRobot")
    rate = bundle["ticksPerSecond"]
    _require(math.isclose(stage.GetTimeCodesPerSecond(), rate, rel_tol=0., abs_tol=1e-9),
             "Kit stage timeCodesPerSecond differs from the committed simulation")
    samples = {sample["tick"]: sample for sample in bundle["samples"]}
    measured = []
    for tick in bundle["witnessTicks"]:
        expected = samples[tick]
        seconds = _seek(app, timeline, tick / rate)
        # This is the actual Kit context's animated Xform at the measured Kit
        # timeline time. No xform operations or poses are authored or reset.
        matrix = omni.usd.get_world_transform_matrix(robot, Usd.TimeCode(seconds * rate))
        position = list(matrix.ExtractTranslation())
        forward = list(matrix.TransformDir(Gf.Vec3d(1., 0., 0.)))
        left = list(matrix.TransformDir(Gf.Vec3d(0., 1., 0.)))
        up = list(matrix.TransformDir(Gf.Vec3d(0., 0., 1.)))
        yaw = math.atan2(forward[1], forward[0])
        expected_yaw = expected["yawRadians"]
        yaw_error = math.remainder(yaw - expected_yaw, math.tau)
        witness = {"tick": tick, "kitSeconds": seconds, "positionMeters": position,
                   "yawRadians": yaw, "yawErrorRadians": yaw_error,
                   "forward": forward, "left": left, "up": up,
                   "worldMatrixRows": [[float(matrix[row][column]) for column in range(4)] for row in range(4)]}
        measured.append(witness)
        report["poseWitnesses"] = measured
        _check_vector(position, expected["positionMeters"], POSITION_TOLERANCE_METERS, f"Kit robot tick {tick} position")
        _check_vector(forward, [math.cos(expected_yaw), math.sin(expected_yaw), 0.],
                      ANGLE_TOLERANCE_RADIANS, f"Kit robot tick {tick} +X")
        _check_vector(left, [-math.sin(expected_yaw), math.cos(expected_yaw), 0.],
                      ANGLE_TOLERANCE_RADIANS, f"Kit robot tick {tick} +Y")
        _check_vector(up, [0., 0., 1.], ANGLE_TOLERANCE_RADIANS, f"Kit robot tick {tick} +Z")
        _require(abs(yaw_error) <= ANGLE_TOLERANCE_RADIANS, f"Kit robot tick {tick} yaw mismatch")
    _seek(app, timeline, 0.)


def _physics_witnesses(app, bundle, verified, report):
    import carb
    import omni.physx
    import omni.timeline

    timeline = omni.timeline.get_timeline_interface()
    query = omni.physx.get_physx_scene_query_interface()
    hit_rays = [ray for ray in bundle["rays"] if ray["sourceId"] is not None]
    _require(len(hit_rays) >= 3 and any(ray["sourceId"] is None for ray in bundle["rays"]),
             "SDK fixture must contain wall, floor and miss witnesses")

    def cast(ray):
        return query.raycast_closest(carb.Float3(*ray["originMeters"]), carb.Float3(*ray["direction"]),
                                     float(ray["maxDistanceMeters"]))

    timeline.play()
    timeline.commit()
    try:
        deadline = monotonic() + PHASE_TIMEOUT_SECONDS
        for _ in range(MAX_QUERY_UPDATES):
            app.update()
            # A miss before PhysX has populated the scene proves nothing.
            if all(cast(ray).get("hit", False) for ray in hit_rays):
                break
            if monotonic() >= deadline:
                raise RuntimeError("PhysX did not populate all expected fixture colliders")
        else:
            raise RuntimeError("PhysX fixture cooking did not complete within the update bound")
        witnesses = []
        for ray in bundle["rays"]:
            hit = cast(ray)
            actual = _json_value(hit)
            if hit.get("hit", False):
                for key in ("position", "normal"):
                    if key in hit:
                        actual[key] = [float(component) for component in hit[key]]
            witness = {"name": ray["name"], "expected": ray, "actual": actual}
            witnesses.append(witness)
            report["rayWitnesses"] = witnesses
            if ray["sourceId"] is None:
                _require(not hit.get("hit", False), f"PhysX {ray['name']}: expected a miss, got {hit}")
                continue
            _require(hit.get("hit", False), f"PhysX {ray['name']}: missing expected hit")
            collider = str(hit.get("collision") or hit.get("rigidBody"))
            expected_path = str(verified["colliders"][ray["sourceId"]])
            _require(collider == expected_path,
                     f"PhysX {ray['name']}: expected {expected_path}, hit {collider}")
            _check_vector([hit["distance"]], [ray["distanceMeters"]], RAY_TOLERANCE_METERS,
                          f"PhysX {ray['name']} distance")
            expected_position = [origin + component * ray["distanceMeters"]
                                 for origin, component in zip(ray["originMeters"], ray["direction"])]
            _check_vector(hit["position"], expected_position, RAY_TOLERANCE_METERS,
                          f"PhysX {ray['name']} hit position")
    finally:
        timeline.pause()
        timeline.commit()


def _class_tokens(value):
    if isinstance(value, dict):
        return _class_tokens(value.get("class", []))
    if isinstance(value, str):
        return {part.strip().casefold() for part in value.split(",") if part.strip()}
    if isinstance(value, (list, tuple)):
        return set().union(*(_class_tokens(item) for item in value)) if value else set()
    return set()


def _write_rgb_png(path, rgb):
    height, width, _ = rgb.shape

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    pixels = b"".join(b"\0" + row.tobytes() for row in rgb)
    image = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
             + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))
    with path.open("xb") as stream:
        stream.write(image)


def _release_render_product(stage, render_product):
    # Replicator 1.13.30 documents create.render_product as returning USD paths.
    # Retain compatibility with HydraTexture handles accepted by annotator APIs.
    if not isinstance(render_product, str):
        destroy = getattr(render_product, "destroy", None)
        _require(callable(destroy), "Unsupported SDK render-product handle")
        destroy()
        return
    _require(render_product.startswith("/"), "SDK render-product path is not absolute")
    previous = stage.GetEditTarget()
    try:
        stage.SetEditTarget(stage.GetSessionLayer())
        _require(stage.RemovePrim(render_product), f"Could not remove owned session render product {render_product}")
    finally:
        stage.SetEditTarget(previous)


def _render_witnesses(app, stage, verified, evidence, report):
    import numpy as np
    import omni.replicator.core as rep
    import omni.timeline
    from pxr import Gf, UsdGeom, UsdLux

    # Session-only camera and neutral illumination. Imported materials, labels,
    # geometry, physics schemas and animated transforms remain untouched.
    stage.SetEditTarget(stage.GetSessionLayer())
    camera_path = "/__ContainerIsaacValidation/Camera"
    camera = UsdGeom.Camera.Define(stage, camera_path)
    camera.CreateFocalLengthAttr(35.)
    camera.CreateHorizontalApertureAttr(36.)
    camera.CreateVerticalApertureAttr(36.)
    camera.CreateClippingRangeAttr(Gf.Vec2f(.05, 20.))
    camera_from_world = Gf.Matrix4d().SetLookAt(Gf.Vec3d(0., 0., 1.5), Gf.Vec3d(4.125, .625, 1.5),
                                            Gf.Vec3d(0., 0., 1.))
    camera.AddTransformOp().Set(camera_from_world.GetInverse())
    light = UsdLux.DomeLight.Define(stage, "/__ContainerIsaacValidation/WhiteDome")
    light.CreateColorAttr(Gf.Vec3f(1., 1., 1.))
    light.CreateIntensityAttr(1000.)
    expected_classes = _class_tokens(verified["products"][27]["labels"])
    _require(expected_classes, "Red wall has no authored class labels to test")
    render_product = rep.create.render_product(camera_path, RESOLUTION, force_new=True, write_to_session_layer=True)
    semantic = rep.AnnotatorRegistry.get_annotator("semantic_segmentation", init_params={"colorize": False})
    rgba_annotator = rep.AnnotatorRegistry.get_annotator("rgb")
    semantic.attach(render_product)
    rgba_annotator.attach(render_product)
    timeline = omni.timeline.get_timeline_interface()
    _seek(app, timeline, 0.)
    latest = None
    passed = False
    deadline = monotonic() + PHASE_TIMEOUT_SECONDS
    try:
        for attempt in range(MAX_RENDER_UPDATES):
            rep.orchestrator.step(rt_subframes=4, pause_timeline=True, delta_time=0., wait_for_render=True)
            labels = semantic.get_data()
            rgba = np.asarray(rgba_annotator.get_data())
            if isinstance(labels, dict) and "data" in labels and "info" in labels:
                ids = np.asarray(labels["data"])
                if (ids.shape == (RESOLUTION[1], RESOLUTION[0]) and ids.dtype == np.uint32
                        and rgba.shape in ((RESOLUTION[1], RESOLUTION[0], 3), (RESOLUTION[1], RESOLUTION[0], 4))
                        and rgba.dtype == np.uint8):
                    info = labels["info"]
                    mapping = info.get("idToLabels", {})
                    matching_ids = [int(key) for key, value in mapping.items()
                                    if expected_classes <= _class_tokens(value)]
                    matched = np.isin(ids, matching_ids)
                    height, width = ids.shape
                    roi = np.zeros(ids.shape, dtype=bool)
                    roi[height // 4:3 * height // 4, width // 4:3 * width // 4] = True
                    selected = matched & roi
                    rgb = rgba[:, :, :3].astype(np.float64)
                    red = ((rgb[:, :, 0] > 1.2 * rgb[:, :, 1] + 8.) &
                           (rgb[:, :, 0] > 1.2 * rgb[:, :, 2] + 8.) & (rgb[:, :, 0] > 24.))
                    count = int(np.count_nonzero(selected))
                    red_count = int(np.count_nonzero(selected & red))
                    measured = {"renderUpdates": attempt + 1, "resolution": list(RESOLUTION),
                                "kitSeconds": float(timeline.get_current_time()), "cameraPath": camera_path,
                                "expectedRedWallClasses": sorted(expected_classes), "matchingSemanticIds": matching_ids,
                                "centerRegionLabelPixels": count, "centerRegionRedLabelPixels": red_count,
                                "minimumLabelPixels": 64, "minimumRedLabelPixels": 64,
                                "redPixelPolicy": "R > 1.2*G + 8, R > 1.2*B + 8 and R > 24 (uint8 RGB)",
                                "labelRegionMedianRgb": np.median(rgb[selected], axis=0).tolist() if count else None,
                                "classLabelInheritance": "Authored red-wall class tokens must be present; inherited parent labels may coexist"}
                    latest = (rgba.copy(), ids.copy(), _json_value(info), measured)
                    report["renderWitness"] = measured
                    if count >= 64 and red_count >= 64:
                        passed = True
                        break
            if monotonic() >= deadline:
                break
        if latest is not None:
            rgba, ids, info, measured = latest
            np.save(evidence / "rgba.npy", rgba, allow_pickle=False)
            np.save(evidence / "semantic-ids.npy", ids, allow_pickle=False)
            _write_json(evidence / "semantic-info.json", info)
            _write_rgb_png(evidence / "rgb.png", np.ascontiguousarray(rgba[:, :, :3]))
            report["renderArtifacts"] = {name: _fingerprint(evidence / name) for name in
                                          ("rgba.npy", "semantic-ids.npy", "semantic-info.json", "rgb.png")}
        stage.GetSessionLayer().Export(str(evidence / "session-overrides.usda"))
        _require(latest is not None, "Replicator did not produce aligned uint8 RGB and uint32 semantic frames")
        _require(passed, f"SDK did not render the authored red wall with its semantic class: {report.get('renderWitness')}")
    finally:
        semantic.detach(render_product)
        rgba_annotator.detach(render_product)
        _release_render_product(stage, render_product)


def run_sdk(backend: str, bundle: dict, report_dir: Path) -> dict:
    """Run the requested real SDK lane and persist raw evidence or its failure.

    Missing SDK imports and unsupported versions raise. The caller owns CLI
    status handling; this function never turns an unavailable SDK into a pass.
    """
    if backend not in ("isaac-sim", "isaac-lab"):
        raise ValueError(f"Unknown Isaac backend {backend!r}")
    evidence = Path(report_dir).resolve() / "sdk"
    evidence.mkdir(parents=True, exist_ok=False)
    stage_path = Path(bundle["stage"]).resolve(strict=True)
    source_paths = {"stage": stage_path, "scene": Path(bundle["scene"]).resolve(strict=True)}
    report = {"schema": "container.robotics-isaac-sdk", "version": 1, "backend": backend,
              "passed": False, "status": "starting", "stage": str(stage_path),
              "sdkPython": sys.version, "pythonExecutable": sys.executable, "references": REFERENCES,
              "inputFingerprints": {name: _fingerprint(value) for name, value in source_paths.items()},
              "tolerances": {"positionMeters": POSITION_TOLERANCE_METERS,
                             "yawRadians": ANGLE_TOLERANCE_RADIANS, "rayMeters": RAY_TOLERANCE_METERS},
              "integrationScope": "Actual Kit stage, timeline, PhysX queries and Replicator via the selected official launcher",
              "limitations": ["ReplayRobot is an authored visual proxy; robot dynamics and Lab RL tasks are not tested",
                              "RGB checks test red material consumption, not cross-renderer photometric equality"],
              "shutdown": "Owned SDK closes after SDK receipt publication; native exit status reaches the worker"}
    report_path = evidence / "sdk-report.json"
    _write_json(report_path, report)
    started = monotonic()
    app = None
    try:
        app, launcher = _launch(backend, report)
        _enable_extension(app, "isaacsim.core.version")
        from isaacsim.core.version import get_version
        from pxr import Usd
        from robotics_isaac_compare import verify_dependencies, verify_stage

        raw_version = list(get_version())
        report["isaacSimVersionRaw"] = raw_version
        _require(bool(raw_version), "Isaac Sim did not report its runtime version")
        _ga_version(raw_version[0], "6.2.0", "Isaac Sim")
        _require(len(raw_version) < 6 or not raw_version[5], f"Isaac Sim prerelease is not supported: {raw_version}")
        report["openUsdVersion"] = list(Usd.GetVersion())
        verify_dependencies(bundle)
        stage = _open_context_stage(app, stage_path)
        verified = verify_stage(stage, bundle)
        report["stageVerification"] = verified
        report["status"] = "stage-verified"
        _write_json(report_path, report)
        _physics_witnesses(app, bundle, verified, report)
        _pose_witnesses(app, stage, bundle, report)
        report["status"] = "physics-and-poses-verified"
        _write_json(report_path, report)
        _enable_extension(app, "omni.replicator.core")
        _render_witnesses(app, stage, verified, evidence, report)
        for name, value in source_paths.items():
            _require(_fingerprint(value) == report["inputFingerprints"][name], f"SDK changed input {name} bytes")
        # Keep the official launcher in scope until the whole lane completes.
        _require(launcher is not None, "SDK launcher ownership was lost")
        report.update(passed=True, status="passed")
    except BaseException as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}", traceback=traceback.format_exc())
        raise
    finally:
        report["durationSeconds"] = monotonic() - started
        _write_json(report_path, report)
        if app is not None:
            try:
                _close_app(app, report)
            except BaseException as error:
                expected = 0 if report["passed"] else 1
                actual = (0 if error.code is None else error.code) if isinstance(error, SystemExit) else None
                if actual != expected:
                    report.update(passed=False, status="failed", shutdownError=f"{type(error).__name__}: {error}")
                    _write_json(report_path, report)
                # Even an expected SystemExit must propagate: only the actual
                # child exit plus the persisted receipt establishes success.
                raise
    return report
