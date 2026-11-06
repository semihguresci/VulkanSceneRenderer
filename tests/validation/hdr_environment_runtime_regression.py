"""Compare matching Radiance and OpenEXR panoramas through Vulkan IBL."""

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

import numpy as np
from PIL import Image, ImageChops

from display_grade_runtime_regression import capture, expected_rgb, WIDTH, HEIGHT
from gltf_extended_materials_runtime_regression import extension_material, fixture, validation_log_error


ENV_WIDTH, ENV_HEIGHT = 32, 16


def panorama():
    return [[(128, 32, 16, 130) if x < ENV_WIDTH // 2 else (16, 48, 128, 130)
             for x in range(ENV_WIDTH)] if y < ENV_HEIGHT // 2 else
            [(32, 128, 16, 129) for _ in range(ENV_WIDTH)] for y in range(ENV_HEIGHT)]


def write_hdr(path, rows):
    data = bytearray(f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {ENV_HEIGHT} +X {ENV_WIDTH}\n".encode())
    for row in rows:
        data.extend((2, 2, 0, ENV_WIDTH))
        for channel in zip(*row, strict=True):
            data.extend((ENV_WIDTH, *channel))
    path.write_bytes(data)


def write_exr(path, rows):
    write_float_exr(path, [[tuple(pixel[c] * 2.0 ** (pixel[3] - 136) for c in range(3))
                           for pixel in row] for row in rows])


def write_float_exr(path, rows):
    # OpenEXR v2 single-part scanline layout, with uncompressed FLOAT channels.
    # https://openexr.com/en/latest/OpenEXRFileLayout.html
    def attribute(name, kind, data):
        return name.encode() + b"\0" + kind.encode() + b"\0" + struct.pack("<I", len(data)) + data

    channels = b"".join(bytes((channel, 0)) + struct.pack("<iB3xii", 2, 0, 1, 1)
                        for channel in b"BGR") + b"\0"
    height, width = len(rows), len(rows[0])
    window = struct.pack("<iiii", 0, 0, width - 1, height - 1)
    header = (struct.pack("<II", 20000630, 2) + attribute("channels", "chlist", channels)
              + attribute("compression", "compression", b"\0")
              + attribute("dataWindow", "box2i", window) + attribute("displayWindow", "box2i", window)
              + attribute("lineOrder", "lineOrder", b"\0")
              + attribute("pixelAspectRatio", "float", struct.pack("<f", 1))
              + attribute("screenWindowCenter", "v2f", struct.pack("<ff", 0, 0))
              + attribute("screenWindowWidth", "float", struct.pack("<f", 1)) + b"\0")
    row_size = width * 3 * 4
    first_chunk = len(header) + height * 8
    offsets = b"".join(struct.pack("<Q", first_chunk + y * (8 + row_size))
                       for y in range(height))
    blocks = []
    for y, row in enumerate(rows):
        values = [pixel[channel] for channel in (2, 1, 0) for pixel in row]
        blocks.append(struct.pack("<iI", y, row_size) + struct.pack(f"<{len(values)}f", *values))
    path.write_bytes(header + offsets + b"".join(blocks))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--live", action="store_true", help="Exercise replacements during one render process")
    parser.add_argument("--ddgi", action="store_true", help="Enable asynchronous DDGI in live captures")
    parser.add_argument("--presentation", action="store_true", help="Validate rotation and background controls")
    parser.add_argument("--sky", action="store_true", help="Validate procedural skies and live replacement")
    parser.add_argument("--multiple", action="store_true", help="Start the live sky sequence with multiple scattering")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    rows = panorama()
    hdr, exr, alternate = (output / name for name in ("panorama-\u0131.HDR", "panorama.exr", "alternate.hdr"))
    write_hdr(hdr, rows)
    write_exr(exr, rows)
    write_hdr(alternate, [[(24, 24, 24, 129)] * ENV_WIDTH for _ in rows])
    model = output / "ibl-receiver.gltf"
    material = extension_material(base=(.8, .8, .8), metallic=1., specular=1.)
    material["pbrMetallicRoughness"]["roughnessFactor"] = .25
    fixture(model, {"material": material})
    if args.sky:
        verify_sky(renderer, output, model, exr, args.ddgi, args.multiple)
        return
    if args.live:
        verify_live(renderer, output, model, exr, hdr, alternate, args.ddgi)
        return
    if args.presentation:
        # Keep both the original and shifted equirect seams inside a uniform
        # sector. The oracle isolates orientation from seam filtering.
        sectors = [(128, 32, 16, 130)] * 4 + [(16, 48, 128, 130)] * 7
        sectors += [(32, 128, 16, 129)] * 9 + [(128, 96, 16, 130)] * 8
        sectors += [(128, 32, 16, 130)] * 4
        rows = [sectors[:] if y < ENV_HEIGHT // 2 else row
                for y, row in enumerate(rows)]
        write_hdr(hdr, rows)
        verify_presentation(renderer, output, model, hdr, rows)
        return
    for technique in ("deferred-raster", "forward-raster"):
        images = [capture(renderer, output, model, technique, label,
                          ("--environment", str(path), "--environment-intensity", "1"))
                  for label, path in (("radiance", hdr), ("openexr", exr), ("alternate", alternate))]
        if ImageChops.difference(images[0], images[1]).getbbox():
            raise RuntimeError(f"{technique}: identical source radiance differs by file format")
        for label, point in (("receiver", (WIDTH // 2, HEIGHT // 2)), ("background", (20, 20))):
            original, changed = (image.getpixel(point) for image in (images[0], images[2]))
            if max(abs(a - b) for a, b in zip(original, changed, strict=True)) < 8:
                raise RuntimeError(f"{technique}: environment change did not affect the {label}")
        baseline = output / f"{technique}-default-baseline.png"
        if baseline.exists():
            current = capture(renderer, output, model, technique, "default", ("--environment-intensity", "1"))
            with Image.open(baseline) as image:
                if ImageChops.difference(current, image.convert("RGB")).getbbox():
                    raise RuntimeError(f"{technique}: bundled EXR changed from the pre-change baseline")
        print(f"{technique}: EXR and Radiance match, alternate changes background and IBL", flush=True)

    malformed = output / "malformed.hdr"
    malformed.write_bytes(b"not Radiance")
    oversized = output / "oversized.exr"
    window = b"dataWindow\0box2i\0" + struct.pack("<Iiiii", 16, 0, 0, ENV_WIDTH - 1, ENV_HEIGHT - 1)
    oversized.write_bytes(exr.read_bytes().replace(window, window[:-8] + struct.pack("<ii", 65535, 65535)))
    out_of_range = output / "out-of-range.hdr"
    write_hdr(out_of_range, [[(128, 32, 16, 147)] * ENV_WIDTH for _ in rows])
    for path in (malformed, oversized, out_of_range, output / "missing.exr"):
        process = subprocess.run([str(renderer), "--hidden", "--no-ui", "--validation", "--environment", str(path)],
                                 cwd=renderer.parent, capture_output=True, timeout=60)
        log = (process.stdout + process.stderr).decode("utf-8", errors="replace")
        path.with_suffix(".reject.log").write_text(log, encoding="utf-8")
        if process.returncode == 0 or "[HDR]" not in log or validation_log_error(log):
            raise RuntimeError(f"Missing clean failure for {path}, see rejection log")
    print("Malformed, missing, oversized and unrepresentable files rejected")


def verify_live(renderer, output, model, exr, hdr, alternate, ddgi):
    invalid = output / "invalid-live.hdr"
    invalid.write_bytes(b"not Radiance")
    samples = (2, 12, 15, 18, 21, 24, 27, 30)
    preparing_frames = tuple(range(4, 12))
    paths = (exr, alternate, alternate, hdr, alternate, hdr, hdr, hdr)
    sequence = output / "live-hdr.json"
    sequence.write_text(json.dumps(dict(schemaVersion=1, frames=31,
        sampleFrames=sorted((*samples, *preparing_frames)),
        events=[dict(frame=4, environment=str(alternate), environmentAsync=True),
                dict(frame=14, environment=str(invalid)),
                dict(frame=17, resize=[384, 288], environment=str(hdr)),
                dict(frame=20, environment=str(alternate)), dict(frame=23, environment=str(hdr)),
                dict(frame=26, environmentYaw=90), dict(frame=29, environmentYaw=0)])),
        encoding="utf-8")
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    for technique in ("deferred-raster", "forward-raster"):
        target = output / f"{technique}-live"
        command = [
            str(renderer), "--hidden", "--no-ui", "--validation", "--no-bloom", "--no-taa",
            "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1",
            "--render-technique", technique, "--display-mode", "lit", "--exposure", "1",
            "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0",
            "--camera-fov", "45", "--import-scale", "1", "--environment-intensity", "1",
            "--directional-intensity", "0", "--model", str(model), "--environment", str(exr),
            "--capture-sequence", str(sequence), "--screenshot", str(target.with_suffix(".png")),
        ]
        command += ["--ddgi", "--ddgi-async"] if ddgi else ["--no-ray-query", "--ray-shadows", "raster"]
        log = target.with_suffix(".log")
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                     env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)), timeout=150)
        if process.returncode or validation_log_error(log.read_text(encoding="utf-8", errors="replace")):
            raise RuntimeError(f"{technique}: live Vulkan capture failed, see {log}")
        frames = []
        epochs = []
        probe_history = []
        for frame, expected_path in zip(samples, paths, strict=True):
            stem = output / f"{target.name}.frame-{frame:04d}"
            telemetry = json.loads(Path(f"{stem}.telemetry.json").read_text(encoding="utf-8"))
            environment = telemetry["environment"]
            if Path(environment["hdrPath"]) != expected_path or environment["loading"]:
                raise RuntimeError(f"{technique}: wrong published environment at frame {frame}: {environment}")
            if frame == 15 and "missing Radiance" not in environment["status"]:
                raise RuntimeError(f"{technique}: malformed environment was not reported")
            if environment["yawDegrees"] != (90 if frame == 27 else 0):
                raise RuntimeError(f"{technique}: incorrect environment rotation at frame {frame}")
            epochs.append(telemetry["taa"]["epoch"])
            if ddgi:
                probe_history.append(telemetry["ddgi"])
            with Image.open(Path(f"{stem}.png")) as image:
                frames.append(image.convert("RGB"))
        generated = []
        for frame in preparing_frames:
            stem = output / f"{target.name}.frame-{frame:04d}"
            telemetry = json.loads(Path(f"{stem}.telemetry.json").read_text(encoding="utf-8"))
            environment = telemetry["environment"]
            if environment["generating"]:
                if (Path(environment["hdrPath"]) != exr or not environment["loading"] or
                        telemetry["taa"]["epoch"] != epochs[0]):
                    raise RuntimeError(f"{technique}: incomplete GPU generation was published")
                if not ddgi:
                    with Image.open(Path(f"{stem}.png")) as image:
                        if ImageChops.difference(frames[0], image.convert("RGB")).getbbox():
                            raise RuntimeError(f"{technique}: pending generation changed the active render")
                generated.append(frame)
        if not generated:
            raise RuntimeError(f"{technique}: no submitted generation observed before publication")
        if not epochs[0] < epochs[1] == epochs[2] < epochs[3] < epochs[4] < epochs[5] < epochs[6] < epochs[7]:
            raise RuntimeError(f"{technique}: unexpected history epochs {epochs}")
        if frames[2].size != (WIDTH, HEIGHT) or frames[3].size != (384, 288):
            raise RuntimeError(f"{technique}: resize did not publish")
        if ddgi:
            if not all(state["active"] and state["asyncActive"] for state in probe_history):
                raise RuntimeError(f"{technique}: asynchronous DDGI was not active for the samples")
            if not probe_history[-1]["asyncSubmittedUpdates"]:
                raise RuntimeError(f"{technique}: DDGI never submitted work")
            resets = [state["historyResets"] for state in probe_history]
            if not resets[0] < resets[1] == resets[2] < resets[3] < resets[4] < resets[5] < resets[6] < resets[7]:
                raise RuntimeError(f"{technique}: unexpected DDGI history resets {resets}")
        if not ddgi:
            for a, b in ((1, 2), (3, 5), (5, 7)):
                if ImageChops.difference(frames[a], frames[b]).getbbox():
                    raise RuntimeError(f"{technique}: invalid load or repeated environment changed the image")
        for a, b in ((0, 1), (3, 4), (5, 6)):
            if not ImageChops.difference(frames[a], frames[b]).getbbox():
                raise RuntimeError(f"{technique}: replacing the environment did not change the image")
        print(f"{technique}: old maps remained active at generation frames {generated}; "
              "swap, rejection, resize, and history invalidation passed", flush=True)


def verify_presentation(renderer, output, model, hdr, rows):
    baked = output / "baked-yaw.hdr"
    shift = ENV_WIDTH // 4
    write_hdr(baked, [row[shift:] + row[:shift] for row in rows])
    receiver = (WIDTH // 2 - 24, HEIGHT // 2 - 24, WIDTH // 2 + 24, HEIGHT // 2 + 24)
    sky = (0, 0, WIDTH, 40)
    solid_color = (.125, .25, .375)
    for technique in ("deferred-raster", "forward-raster"):
        def render(label, path=hdr, *options):
            return capture(renderer, output, model, technique, label,
                           ("--environment", str(path), "--environment-intensity", "1", *options))

        original = render("presentation-default")
        rotated = render("presentation-yaw", hdr, "--environment-yaw", "90")
        reference = render("presentation-baked", baked)
        blurred = render("presentation-blur", hdr, "--background-blur", "1")
        solid = render("presentation-solid", hdr, "--background-color", *map(str, solid_color))
        for label, region in (("sky", sky), ("receiver", receiver)):
            error = np.abs(np.asarray(rotated.crop(region), dtype=np.int16) -
                           np.asarray(reference.crop(region), dtype=np.int16))
            if error.mean() > 1.5 or np.percentile(error, 95) > 4:
                raise RuntimeError(f"{technique}: yaw disagrees with baked {label}: {error.mean():.3f}")
            if not ImageChops.difference(original.crop(region), rotated.crop(region)).getbbox():
                raise RuntimeError(f"{technique}: yaw did not affect the {label}")
        for name, image in (("blur", blurred), ("solid", solid)):
            if ImageChops.difference(original.crop(receiver), image.crop(receiver)).getbbox():
                raise RuntimeError(f"{technique}: background {name} affected IBL")
            if not ImageChops.difference(original.crop(sky), image.crop(sky)).getbbox():
                raise RuntimeError(f"{technique}: background {name} did not affect the sky")
        expected = expected_rgb(solid_color, "aces", 1., 1.)
        if np.max(np.abs(np.asarray(solid.crop(sky), dtype=np.int16) - expected)) > 2:
            raise RuntimeError(f"{technique}: solid background used the wrong color transform")
        print(f"{technique}: yaw matches baked panorama and background controls preserve IBL", flush=True)


def verify_sky(renderer, output, model, exr, ddgi, multiple=False):
    horizon, zenith, ground = (.25, .5, .125), (.75, .125, .25), (.0625, .25, .625)
    plain = output / "plain-reference.exr"
    gradient = output / "gradient-reference.exr"
    write_float_exr(plain, [[horizon] * 16 for _ in range(8)])
    rows = []
    for y in range(256):
        elevation = 1 - (2*y + 1) / 256
        pole = zenith if elevation >= 0 else ground
        color = tuple(a + abs(elevation) * (b - a) for a, b in zip(horizon, pole, strict=True))
        rows.append([color] * 512)
    write_float_exr(gradient, rows)
    controls = ("--sky-horizon", *map(str, horizon), "--sky-zenith", *map(str, zenith),
                "--sky-ground", *map(str, ground), "--environment-intensity", "1")
    sequence = output / "live-sky.json"
    frames = (2, 5, 8, 11, 14)
    sequence.write_text(json.dumps(dict(schemaVersion=1, frames=15, sampleFrames=frames,
        events=[dict(frame=4, proceduralSky="plain"), dict(frame=7, proceduralSky="gradient"),
                dict(frame=10, proceduralSky="atmosphere"), dict(frame=13, environment=str(exr))])),
        encoding="utf-8")
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    for technique in ("deferred-raster", "forward-raster"):
        for mode, reference in (("plain", plain), ("gradient", gradient)):
            actual = capture(renderer, output, model, technique, f"sky-{mode}",
                             ("--sky", mode, *controls))
            expected = capture(renderer, output, model, technique, f"sky-{mode}-reference",
                               ("--environment", str(reference), "--environment-intensity", "1"))
            error = np.abs(np.asarray(actual, dtype=np.int16) - np.asarray(expected, dtype=np.int16))
            if error.max() > 2:
                raise RuntimeError(f"{technique}: {mode} differs from equivalent EXR by {error.max()} codes")
        target = output / f"{technique}-sky-live"
        command = [
            str(renderer), "--hidden", "--no-ui", "--validation", "--no-bloom", "--no-taa",
            "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1",
            "--render-technique", technique, "--display-mode", "lit", "--exposure", "1",
            "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0",
            "--camera-fov", "45", "--import-scale", "1", "--environment-intensity", "1",
            "--directional-intensity", "1", "--model", str(model),
            "--capture-sequence", str(sequence), "--screenshot", str(target.with_suffix(".png")),
        ]
        command += ["--ddgi", "--ddgi-async"] if ddgi else ["--no-ray-query"]
        if multiple:
            command += ["--sky", "atmosphere", "--sky-multiple-scattering", "--sky-aerial-scale", "1000"]
        else:
            command += ["--environment", str(exr)]
        log = target.with_suffix(".log")
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                    env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)), timeout=120)
        if result.returncode or validation_log_error(log.read_text(encoding="utf-8", errors="replace")):
            raise RuntimeError(f"{technique}: procedural sky replacement failed, see {log}")
        previous_epoch, previous_resets = -1, -1
        for frame, mode in zip(frames, (3 if multiple else 0, 1, 2, 3, 0), strict=True):
            path = output / f"{target.name}.frame-{frame:04d}.telemetry.json"
            state = json.loads(path.read_text(encoding="utf-8"))
            environment = state["environment"]
            if environment["skyMode"] != mode or environment["loading"]:
                raise RuntimeError(f"{technique}: incorrect sky source at frame {frame}")
            if environment["hdrPath"] != (str(exr) if mode == 0 else ""):
                raise RuntimeError(f"{technique}: stale HDR path at frame {frame}")
            directional = state["lighting"]["directionalIntensity"]
            if (mode == 3 and directional <= 1.) or (mode != 3 and directional != 1.):
                raise RuntimeError(f"{technique}: linked sun was not installed or cleared at frame {frame}")
            if state["taa"]["epoch"] <= previous_epoch:
                raise RuntimeError(f"{technique}: sky change did not reset temporal history")
            previous_epoch = state["taa"]["epoch"]
            if ddgi:
                probes = state["ddgi"]
                if not probes["asyncActive"] or not probes["active"] or probes["historyResets"] <= previous_resets:
                    raise RuntimeError(f"{technique}: sky change did not reset active DDGI history")
                previous_resets = probes["historyResets"]
        print(f"{technique}: plain and gradient match baked EXR; live transitions reset history", flush=True)


if __name__ == "__main__":
    main()
