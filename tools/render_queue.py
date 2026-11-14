#!/usr/bin/env python3
"""Run a local queue of stills and turntables through the renderer CLI."""

import argparse
from collections import defaultdict
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile


RENDERERS = ("forward-raster", "deferred-raster", "path-tracing")
IMAGE_SIGNATURES = {".png": b"\x89PNG\r\n\x1a\n", ".exr": b"\x76\x2f\x31\x01"}
VALIDATION_ERROR = re.compile(
    rb"VUID-|SYNC-HAZARD|Validation Error|\bhazard detected\b|"
    rb"\b(?:READ|WRITE)_AFTER_(?:READ|WRITE)\b[^\r\n]*\bhazard\b",
    re.IGNORECASE,
)


def unique_fields(pairs):
    result = dict(pairs)
    if len(result) != len(pairs):
        raise ValueError("duplicate JSON field")
    return result


def fields(value, label, required, optional=()):
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    missing = set(required) - value.keys()
    extra = value.keys() - set(required) - set(optional)
    if missing:
        raise ValueError(f"{label} needs {', '.join(sorted(missing))}")
    if extra:
        raise ValueError(f"{label} has unknown fields {', '.join(sorted(extra))}")


def integer(value, name, lower, upper):
    if type(value) is not int or not lower <= value <= upper:
        raise ValueError(f"{name} must be an integer in [{lower}, {upper}]")
    return value


def number(value, name, lower, upper):
    if type(value) not in (int, float) or not lower <= value <= upper:
        raise ValueError(f"{name} must be finite and in [{lower}, {upper}]")
    return float(value)


def path_from(base, name, value):
    if not isinstance(value, str) or not value.strip() or "\0" in value:
        raise ValueError(f"{name} must be a nonempty file path")
    # Resolve directory aliases for collision detection. Keep the final name
    # so a dangling symlink at an output destination is still a collision.
    path = base / value
    return path.parent.resolve() / path.name


def sampling_options(renderer, settings):
    if renderer == "path-tracing":
        fields(settings, "sampling", (), ("samples", "samplesPerFrame", "bounces", "seed"))
        samples = integer(settings.get("samples", 64), "samples", 1, 16777216)
        batch = integer(settings.get("samplesPerFrame", 4), "samplesPerFrame", 1, 64)
        bounces = integer(settings.get("bounces", 8), "bounces", 1, 32)
        seed = integer(settings.get("seed", 1), "seed", 0, 2**32 - 1)
        options = ["--msaa", "1", "--no-taa", "--no-ddgi",
                   "--path-samples", str(samples), "--path-spp", str(batch),
                   "--path-bounces", str(bounces), "--path-seed", str(seed)]
        return options, (samples + batch - 1) // batch, 1, False, samples

    fields(settings, "sampling", (), ("msaa", "taa", "warmupFrames"))
    msaa = settings.get("msaa", 1)
    if type(msaa) is not int or msaa not in (1, 2, 4, 8, 16, 32, 64):
        raise ValueError("msaa must be a Vulkan sample count")
    taa = settings.get("taa", False)
    if type(taa) is not bool or (taa and msaa != 1):
        raise ValueError("taa must be a boolean and requires msaa=1 when enabled")
    warmup = integer(settings.get("warmupFrames", 0), "warmupFrames", 0, 9999)
    options = ["--msaa", str(msaa), "--taa" if taa else "--no-taa",
               "--no-ddgi", "--no-ray-query", "--ray-shadows", "raster"]
    return options, warmup + 1, msaa, taa, None


@dataclass
class Job:
    settings: dict
    scene: Path
    output: Path
    options: list
    capture_frame: int
    msaa: int
    taa: bool
    samples: int | None

    @property
    def name(self):
        return self.settings["name"]

    @property
    def frame_count(self):
        return self.settings["orbit"]["frames"] if "orbit" in self.settings else 1

    def destination(self, frame):
        if "orbit" not in self.settings:
            return self.output
        return self.output.with_name(f"{self.output.stem}.{frame:04d}{self.output.suffix}")

    def camera_options(self, frame):
        if "orbit" not in self.settings:
            return ["--gltf-camera-node", str(self.settings["cameraNode"])]
        orbit = self.settings["orbit"]
        x, y, z = orbit["target"]
        angle = math.tau * (frame - 1) / self.frame_count
        position = (x + orbit["radius"] * math.cos(angle), y + orbit["height"],
                    z + orbit["radius"] * math.sin(angle))
        return ["--camera-position", *map(str, position),
                "--camera-target", *map(str, orbit["target"]),
                "--camera-fov", str(orbit.get("fov", 45))]


def parse_job(base, job):
    fields(job, "job", ("name", "scene", "renderer", "width", "height", "output"),
           ("cameraNode", "orbit", "sampling"))
    if not isinstance(job["name"], str) or not job["name"].strip():
        raise ValueError("job needs a nonempty name")
    if job["renderer"] not in RENDERERS:
        raise ValueError(f"renderer must be one of {', '.join(RENDERERS)}")
    integer(job["width"], "width", 1, 16384)
    integer(job["height"], "height", 1, 16384)
    if ("cameraNode" in job) == ("orbit" in job):
        raise ValueError("set exactly one of cameraNode and orbit")
    scene = path_from(base, "scene", job["scene"])
    output = path_from(base, "output", job["output"])
    if output.suffix.lower() not in IMAGE_SIGNATURES:
        raise ValueError("output must end in .png or .exr")
    if "cameraNode" in job:
        integer(job["cameraNode"], "cameraNode", 0, 2**31 - 1)
        if scene.suffix.lower() not in (".gltf", ".glb"):
            raise ValueError("authored cameraNode requires a glTF or GLB scene")
    else:
        orbit = job["orbit"]
        fields(orbit, "orbit", ("target", "radius", "height", "frames"), ("fov",))
        target = orbit["target"]
        if not isinstance(target, list) or len(target) != 3:
            raise ValueError("orbit.target must contain three coordinates")
        for coordinate in target:
            number(coordinate, "orbit.target coordinate", -1e8, 1e8)
        number(orbit["radius"], "orbit.radius", 0.001, 1e8)
        number(orbit["height"], "orbit.height", -1e8, 1e8)
        integer(orbit["frames"], "orbit.frames", 1, 10000)
        number(orbit.get("fov", 45), "orbit.fov", 1, 179)
    options = sampling_options(job["renderer"], job.get("sampling", {}))
    return Job(job, scene, output, *options)


def read_queue(source):
    queue = json.loads(source.read_bytes(), object_pairs_hook=unique_fields)
    fields(queue, "queue", ("schemaVersion", "jobs"))
    integer(queue["schemaVersion"], "schemaVersion", 1, 1)
    if not isinstance(queue["jobs"], list) or not queue["jobs"]:
        raise ValueError("jobs must be a nonempty list")
    jobs = []
    for index, item in enumerate(queue["jobs"], 1):
        try:
            jobs.append(parse_job(source.parent, item))
        except ValueError as error:
            raise ValueError(f"job {index}: {error}") from error
    return jobs


def capture_files(image):
    # Match Application.cpp: publish the primary image after its sidecars.
    return Path(f"{image}.capture.json"), image.with_suffix(".telemetry.json"), image


def preflight(jobs):
    errors = defaultdict(set)
    destinations = defaultdict(list)
    for index, job in enumerate(jobs):
        if not job.scene.is_file():
            errors[index].add(f"scene is unavailable: {job.scene}")
        for frame in range(1, job.frame_count + 1):
            for output in capture_files(job.destination(frame)):
                key = os.path.normcase(str(output))
                destinations[key].append((index, output))
                if os.path.lexists(output):
                    errors[index].add(f"output already exists: {output}")
    for matches in destinations.values():
        if len(matches) > 1:
            for index, output in matches:
                errors[index].add(f"output is shared by queued jobs: {output}")
    return errors


def create_new(source, destination):
    if os.name == "nt":
        # os.rename on Windows fails if the target already exists.
        source.rename(destination)
    else:
        os.link(source, destination)


def save_queue(jobs, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    queue = {"schemaVersion": 1, "jobs": [
        {**job.settings, "scene": str(job.scene), "output": str(job.output)} for job in jobs]}
    with tempfile.TemporaryDirectory(prefix=".render-queue-", dir=destination.parent) as temp:
        staged = Path(temp) / "queue.json"
        staged.write_text(json.dumps(queue, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        create_new(staged, destination)


def publish_frame(staged, destination):
    published = []
    try:
        for source, target in zip(capture_files(staged), capture_files(destination)):
            identity = source.stat()
            create_new(source, target)
            published.append((target, identity))
    except OSError:
        for target, identity in reversed(published):
            try:
                if os.path.samestat(target.lstat(), identity):
                    target.unlink()
            except FileNotFoundError:
                pass
        raise


def receipt_object(value, name):
    if not isinstance(value, dict):
        raise ValueError(f"{name} must be an object")
    return value


def verify_capture(image, job):
    metadata, telemetry, _ = capture_files(image)
    with image.open("rb") as stream:
        signature = IMAGE_SIGNATURES[image.suffix.lower()]
        if stream.read(len(signature)) != signature or not stream.read(1):
            raise ValueError(f"renderer wrote an invalid {image.suffix} image")
    info = receipt_object(json.loads(metadata.read_bytes()), "capture metadata")
    state = receipt_object(json.loads(telemetry.read_bytes()), "capture telemetry")
    taa = receipt_object(state.get("taa"), "TAA telemetry")
    dims = [job.settings["width"], job.settings["height"]]
    technique = job.settings["renderer"]
    source = "viewport" if image.suffix.lower() == ".png" else (
        "raster-scene-composite" if job.samples is None else "path-trace-mean"
    )
    if ([info.get("width"), info.get("height")] != dims or
            info.get("source") != source or info.get("includesUi") is not False or
            state.get("resolution") != dims or state.get("technique") != technique or
            state.get("msaaSamples") != job.msaa or taa.get("enabled") is not job.taa):
        raise ValueError("capture metadata or telemetry differs from requested render settings")
    if "cameraNode" in job.settings:
        animation = receipt_object(state.get("gltfAnimation"), "glTF camera telemetry")
        if animation.get("selectedCameraNode") != job.settings["cameraNode"]:
            raise ValueError("capture did not use the requested authored camera")
    if job.samples is not None:
        path = receipt_object(info.get("referencePath"), "reference capture metadata")
        sampling = job.settings.get("sampling", {})
        if (path.get("attemptedSamplesPerPixel") != job.samples or
                path.get("sampleLimit") != job.samples or
                path.get("samplesPerFrame") != sampling.get("samplesPerFrame", 4) or
                path.get("maxBounces") != sampling.get("bounces", 8) or
                path.get("seed") != sampling.get("seed", 1) or
                path.get("unsupportedSamples") != 0 or path.get("rejectedSamples") != 0):
            raise ValueError("reference capture did not use the requested settings and clean sample count")


def run_capture(renderer, job, frame, staged, validation, check_cancel):
    check_cancel()
    command = [str(renderer), "--model", str(job.scene), "--import-scale", "1",
               "--hidden", "--no-ui", "--frames-in-flight", "1", "--display-mode", "lit",
               "--width", str(job.settings["width"]), "--height", str(job.settings["height"]),
               "--render-technique", job.settings["renderer"], "--fixed-dt", ".016666667",
               "--warmup-frames", "0", "--capture-frame", str(job.capture_frame),
               "--validation" if validation else "--no-validation",
               *job.options, *job.camera_options(frame), "--screenshot", str(staged)]
    if staged.suffix.lower() == ".png":
        command.append("--screenshot-viewport")
    # Keep terminal Ctrl+C in the queue so it can terminate and reap its child.
    isolation = ({"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt"
                 else {"start_new_session": True})
    log = staged.parent / "renderer.log"
    with log.open("wb") as stream:
        child = subprocess.Popen(command, cwd=renderer.parent, stdin=subprocess.DEVNULL,
                                 stdout=stream, stderr=subprocess.STDOUT, **isolation)
        try:
            while True:
                check_cancel()
                try:
                    status = child.wait(timeout=0.25)
                    break
                except subprocess.TimeoutExpired:
                    continue
        except KeyboardInterrupt:
            if child.poll() is None:
                child.kill()
            child.wait()
            raise
    check_cancel()
    try:
        if status:
            raise ValueError(f"renderer exited with status {status}")
        if validation:
            with log.open("rb") as records:
                for line in records:
                    if VALIDATION_ERROR.search(line):
                        raise ValueError(line.decode("utf-8", "replace").strip())
        verify_capture(staged, job)
    except (OSError, ValueError) as error:
        with log.open("rb") as stream:
            stream.seek(max(0, log.stat().st_size - 4096))
            tail = stream.read().decode("utf-8", "replace").strip()
        raise ValueError(f"{error}\n{tail}".rstrip()) from error


def run_queue(jobs, renderer, validation):
    errors = preflight(jobs)
    failed = False
    interrupted = False
    cancelled = False

    def request_cancel(*_):
        nonlocal cancelled
        cancelled = True

    def check_cancel():
        if cancelled:
            raise KeyboardInterrupt

    previous = signal.signal(signal.SIGINT, request_cancel)
    try:
        for index, job in enumerate(jobs):
            print(f"[{index + 1}/{len(jobs)}] {job.name}", flush=True)
            if errors[index]:
                failed = True
                print(f"  failed: {'; '.join(sorted(errors[index]))}", file=sys.stderr, flush=True)
                continue
            try:
                check_cancel()
                job.output.parent.mkdir(parents=True, exist_ok=True)
                with tempfile.TemporaryDirectory(prefix=".render-queue-", dir=job.output.parent) as temp:
                    for frame in range(1, job.frame_count + 1):
                        destination = job.destination(frame)
                        staged = Path(temp) / destination.name
                        print(f"  rendering {frame}/{job.frame_count}: {destination.name}", flush=True)
                        run_capture(renderer, job, frame, staged, validation, check_cancel)
                        check_cancel()
                        publish_frame(staged, destination)
                        print(f"  saved {frame}/{job.frame_count}: {destination}", flush=True)
            except KeyboardInterrupt:
                interrupted = True
                cancelled = False
                print(f"  cancelled: {job.name}", file=sys.stderr, flush=True)
            except (OSError, ValueError) as error:
                failed = True
                print(f"  failed: {job.name}: {error}", file=sys.stderr, flush=True)
    finally:
        signal.signal(signal.SIGINT, previous)
    return 130 if interrupted or cancelled else int(failed)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate", help="Check a versioned job list")
    validate.add_argument("queue", type=Path)
    validate.add_argument("--save-as", type=Path, help="Save a copy with absolute paths")
    run = commands.add_parser("run", help="Render the jobs in order")
    run.add_argument("queue", type=Path)
    run.add_argument("--renderer", type=Path, required=True, help="Path to VulkanSceneRenderer")
    run.add_argument("--validation", action="store_true", help="Enable renderer Vulkan validation")
    args = parser.parse_args(argv)
    try:
        jobs = read_queue(args.queue.resolve(strict=True))
        if args.command == "run":
            renderer = args.renderer.resolve(strict=True)
            if not renderer.is_file():
                raise ValueError("renderer must be an executable file")
            return run_queue(jobs, renderer, args.validation)
        if args.save_as:
            save_queue(jobs, path_from(Path.cwd(), "save-as", str(args.save_as)))
        print(f"Validated {len(jobs)} jobs")
        return 0
    except (OSError, ValueError) as error:
        print(f"render queue: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
