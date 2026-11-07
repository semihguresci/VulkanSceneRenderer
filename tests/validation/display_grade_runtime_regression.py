"""Validate scene-linear tone curves and display grading on both Vulkan raster paths."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess

from PIL import Image, ImageChops

from display_capture_test_support import Asset, validation_log_error


WIDTH, HEIGHT = 320, 240
CENTERS = (-1.2, -.4, .4, 1.2)
COLORS = ((0., 0., 0.), (.0625, .125, .25), (.25, .5, 1.), (4., 2., .5))
RECORDS = []


def srgb_to_linear(value):
    value = min(max(value, 0), 1)
    return value / 12.92 if value <= .04045 else ((value + .055) / 1.055) ** 2.4


def uncharted_curve(value):
    a, b, c, d, e, f = .15, .50, .10, .20, .02, .30
    return ((value * (a * value + c * b) + d * e) /
            (value * (a * value + b) + d * f) - e / f)


def agx_curve(color):
    # Reference coefficients are arranged by input channel. Each column
    # contributes to all three outputs, which exercises matrix orientation.
    inset = (
        (.842479062253094, .0423282422610123, .0423756549057051),
        (.0784336, .878468636469772, .0784336),
        (.0792237451477643, .0791661274605434, .879142973793104),
    )
    outset = (
        (1.19687900512017, -.0528968517574562, -.0529716355144438),
        (-.0980208811401368, 1.15190312990417, -.0980434501171241),
        (-.0990297440797205, -.0989611768448433, 1.15107367264116),
    )
    polynomial = []
    for column in zip(*inset, strict=True):
        value = sum(channel * weight for channel, weight in zip(color, column, strict=True))
        log_value = math.log2(max(value, 2 ** -12.47393))
        x = min(max((log_value + 12.47393) / 16.5, 0), 1)
        polynomial.append(15.5*x**6 - 40.14*x**5 + 31.96*x**4 - 6.868*x**3
                          + .4298*x**2 + .1191*x - .0023)
    return [srgb_to_linear(sum(p * weight for p, weight in zip(polynomial, column, strict=True)))
            for column in zip(*outset, strict=True)]


def pbr_neutral_curve(color):
    minimum = min(color)
    offset = minimum - 6.25 * minimum ** 2 if minimum < .08 else .04
    color = [channel - offset for channel in color]
    peak = max(color)
    start = .8 - .04
    if peak < start:
        return color
    span = 1 - start
    compressed = 1 - span ** 2 / (peak + span - start)
    scale = 1 / (1 + .15 * (peak - compressed))
    return [compressed * (1 - scale * (1 - channel / peak)) for channel in color]


def write_chart(path, centers=CENTERS, colors=COLORS, half_width=.3):
    asset = Asset()
    primitives = []
    asset.model["extensionsUsed"] = ["KHR_materials_emissive_strength"]
    indices = asset.accessor("SCALAR", [0, 1, 2, 0, 2, 3], 5123, indices=True)
    for center, color in zip(centers, colors, strict=True):
        positions = [[center + x, y, 0] for x, y in
                     ((-half_width, -.45), (half_width, -.45),
                      (half_width, .45), (-half_width, .45))]
        primitives.append(dict(attributes=dict(POSITION=asset.accessor("VEC3", positions)),
                               indices=indices, material=len(asset.model["materials"])))
        strength = max(1., *color)
        asset.model["materials"].append(dict(
            doubleSided=True,
            pbrMetallicRoughness=dict(baseColorFactor=[0, 0, 0, 1], metallicFactor=1),
            emissiveFactor=[channel / strength for channel in color],
            extensions=dict(KHR_materials_emissive_strength=dict(emissiveStrength=strength))))
    asset.model["meshes"] = [dict(primitives=primitives)]
    asset.write(path)


def white_balance(color, temperature, tint):
    def locus(t):
        return (
            (1.28641212e-7*t*t + 1.54118254e-4*t + .860117757) /
            (7.08145163e-7*t*t + 8.42420235e-4*t + 1),
            (4.20481691e-8*t*t + 4.22806245e-5*t + .317398726) /
            (1.61456053e-7*t*t - 2.89741816e-5*t + 1),
        )

    u, v = locus(temperature)
    lo, hi = locus(temperature - .01), locus(temperature + .01)
    du, dv = (b - a for a, b in zip(lo, hi, strict=True))
    length = math.hypot(du, dv)
    u, v = u + tint * dv / length, v - tint * du / length
    # Convert CIE 1960 directly to XYZ, with Y = 1.
    xyz = (3*u/(2*v), 1., max((4-u-10*v)/(2*v), 0.))

    def product(matrix, value):
        return [sum(a*b for a, b in zip(row, value, strict=True)) for row in matrix]

    xyz_to_cone = ((.7982, .3389, -.1371), (-.5918, 1.5512, .0406), (.0008, .0239, .9753))
    to_cone = ((.39858179, .51147528, .03823013), (.08651038, .90263008, .04376704),
               (.02423435, .13363512, .92889263))
    from_cone = ((2.86209568, -1.61563711, -.04166966), (-.27259146, 1.26953249, -.04859816),
                 (-.03545430, -.14049009, 1.08462940))
    white = product(xyz_to_cone, xyz)
    reference = product(to_cone, (1., 1., 1.))
    response = product(to_cone, color)
    return [max(0, c) for c in product(from_cone, [c*r/max(w, .05)
            for c, r, w in zip(response, reference, white, strict=True)])]


def expected_rgb(color, mode, saturation, contrast, *, illuminant=None, vibrance=0.,
                 shadows=(1., 1., 1.), highlights=(1., 1., 1.)):
    if illuminant:
        color = white_balance(color, *illuminant)
    if mode == "clip":
        mapped = [min(channel, 1) for channel in color]
    elif mode == "reinhard":
        mapped = [channel / (1 + channel) for channel in color]
    elif mode == "filmic":
        shifted = [max(channel - .004, 0) for channel in color]
        mapped = [srgb_to_linear((6.2*x*x + .5*x) / (6.2*x*x + 1.7*x + .06)) for x in shifted]
    elif mode == "uncharted":
        mapped = [srgb_to_linear(max(0, uncharted_curve(2 * channel) /
                                    uncharted_curve(11.2)) ** (1 / 2.2)) for channel in color]
    elif mode == "agx":
        mapped = agx_curve(color)
    elif mode == "neutral":
        mapped = pbr_neutral_curve(color)
    else:
        mapped = [min(channel * (2.51 * channel + .03) /
                      (channel * (2.43 * channel + .59) + .14), 1) for channel in color]
    luminance = sum(channel * weight for channel, weight in
                    zip(mapped, (.2126, .7152, .0722), strict=True))
    mapped = [luminance + (channel - luminance) * saturation for channel in mapped]
    chroma = min(1, max(mapped) - min(mapped))
    graded = [luminance + (channel - luminance) * (1 + vibrance * (1 - chroma))
              for channel in mapped]
    graded = [(channel - .18) * contrast + .18 for channel in graded]
    tonal_luminance = min(max(sum(c * w for c, w in
                                 zip(graded, (.2126, .7152, .0722), strict=True)), 0), 1)
    weight = tonal_luminance ** 2 * (3 - 2 * tonal_luminance)
    result = []
    for channel, shadow, highlight in zip(graded, shadows, highlights, strict=True):
        linear = min(max(channel * ((1 - weight) * shadow + weight * highlight), 0), 1)
        encoded = 12.92 * linear if linear <= .0031308 else 1.055 * linear ** (1 / 2.4) - .055
        result.append(round(255 * encoded))
    return result


def capture(renderer, output, model, technique, case, options):
    screenshot = output / f"{technique}-{case}.png"
    screenshot.unlink(missing_ok=True)
    log = screenshot.with_suffix(".log")
    command = [
        str(renderer), "--hidden", "--no-ui", "--validation", "--no-bloom", "--no-taa",
        "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1",
        "--warmup-frames", "12", "--capture-frame", "13", "--fixed-dt", ".016666667",
        "--render-technique", technique, "--display-mode", "lit", "--exposure", "1",
        "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0",
        "--camera-fov", "45", "--import-scale", "1", "--environment-intensity", "0",
        "--directional-intensity", "0", "--no-ray-query", "--ray-shadows", "raster",
        "--model", str(model), "--screenshot", str(screenshot), *options,
    ]
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=120, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)))
    if process.returncode or validation_log_error(log.read_text(encoding="utf-8", errors="replace")):
        raise RuntimeError(f"Vulkan capture failed, see {log}")
    RECORDS.append(dict(image=str(screenshot), log=str(log), command=command))
    with Image.open(screenshot) as image:
        return image.convert("RGB")


def verify_presets(renderer, output, options):
    preset = output / "color-grade-\u0131.json"
    round_trip = output / "color-grade-round-trip.json"
    malformed = output / "color-grade-invalid.json"

    def run(*args, accepted=True):
        process = subprocess.run([str(renderer), *map(str, args)], cwd=renderer.parent,
                                 capture_output=True, text=True, encoding="utf-8",
                                 errors="replace", timeout=20)
        if (process.returncode == 0) != accepted:
            raise RuntimeError(f"Unexpected preset result for {args}: {process.stderr}")

    run(*options, "--save-display-grade", preset)
    run("--display-grade", preset, "--save-display-grade", round_trip)
    canonical = preset.read_bytes()
    if round_trip.read_bytes() != canonical:
        raise RuntimeError("Display preset round trip changed the saved grade")
    run("--display-grade", preset, "--tone-map", "neutral", "--save-display-grade", round_trip)
    if json.loads(round_trip.read_bytes()) != dict(json.loads(canonical), toneMap="neutral"):
        raise RuntimeError("A later CLI flag did not override the preset")

    original = round_trip.read_bytes()
    model = json.loads(canonical)
    malformed_values = [
        json.dumps(dict(model, version=2)), json.dumps(dict(model, tintDuv=0.3)),
        json.dumps(dict(model, shadowTone=[1, 0.5])), json.dumps(dict(model, contrast=True)),
        json.dumps(dict(model, unknown=0)), canonical.decode().replace('"version": 1', '"version": 1, "version": 1'),
        " " * 4097, "[" * 32 + "0" + "]" * 32,
    ]
    for value in malformed_values:
        malformed.write_text(value, encoding="utf-8")
        run("--display-grade", malformed, "--save-display-grade", round_trip, accepted=False)
        if round_trip.read_bytes() != original:
            raise RuntimeError("Rejected preset replaced an existing grade")
    blocked = output / "blocked-preset.json"
    blocked.mkdir(exist_ok=True)
    run("--save-display-grade", blocked, accepted=False)
    if not blocked.is_dir() or list(output.glob("blocked-preset.json.tmp-*")):
        raise RuntimeError("Failed preset publication damaged the destination or leaked a temporary")
    return preset


def check_samples(image, centers, colors, mode, saturation=1., contrast=1., **grade):
    for center, color in zip(centers, colors, strict=True):
        x = round(WIDTH / 2 + center * HEIGHT / (8 * math.tan(math.radians(22.5))))
        actual = image.getpixel((x, HEIGHT // 2))
        expected = expected_rgb(color, mode, saturation, contrast, **grade)
        if max(abs(a - b) for a, b in zip(actual, expected, strict=True)) > 2:
            raise RuntimeError(f"{mode} {color}: expected {expected}, got {actual}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--baseline", action="store_true", help="Capture default output before rebuilding")
    args = parser.parse_args()
    renderer_hash = hashlib.sha256(args.renderer.read_bytes()).hexdigest()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    model = output / "display-chart.gltf"
    write_chart(model)
    cases = [
        ("default", (), "aces", 1., 1.),
        ("aces", ("--tone-map", "aces"), "aces", 1., 1.),
        ("reinhard", ("--tone-map", "reinhard"), "reinhard", 1., 1.),
        ("clip", ("--tone-map", "clip"), "clip", 1., 1.),
        ("desaturated", ("--tone-map", "clip", "--saturation", "0"), "clip", 0., 1.),
        ("contrast", ("--tone-map", "clip", "--contrast", "1.5"), "clip", 1., 1.5),
    ]
    cases.extend((mode, ("--tone-map", mode), mode, 1., 1.)
                 for mode in ("filmic", "uncharted", "agx", "neutral"))
    cases.append(("filmic-graded", ("--tone-map", "filmic", "--saturation", "0",
                                    "--contrast", "1.25"), "filmic", 0., 1.25))
    advanced = {
        "white-balance": (("--white-balance", "3100"), dict(illuminant=(3100, .00325895312))),
        "tint": (("--tint", "-0.021"), dict(illuminant=(6506.11144, -.021))),
        "vibrance": (("--vibrance", ".8"), dict(vibrance=.8)),
        "split-tone": (("--shadow-tone", ".4", ".75", "1", "--highlight-tone", "1", ".75", ".4"),
                       dict(shadows=(.4, .75, 1.), highlights=(1., .75, .4))),
    }
    cases.extend((name, ("--tone-map", "clip", *options), "clip", 1., 1.)
                 for name, (options, _) in advanced.items())
    preset_options = ("--tone-map", "agx", "--saturation", ".8", "--contrast", "1.1",
                      "--white-balance", "5100", "--tint", ".01", "--vibrance", ".5",
                      "--shadow-tone", ".5", ".8", "1", "--highlight-tone", "1", ".75", ".4")
    preset = None if args.baseline else verify_presets(args.renderer.resolve(), output, preset_options)
    ramp = output / "display-ramp.gltf"
    ramp_centers = tuple((index - 7.5) / 4 for index in range(16))
    ramp_colors = ((0., 0., 0.),) + tuple((2. ** exponent,) * 3 for exponent in range(-8, 7))
    if not args.baseline:
        write_chart(ramp, ramp_centers, ramp_colors, half_width=.1)
    for technique in ("deferred-raster", "forward-raster"):
        if args.baseline:
            capture(args.renderer.resolve(), output, model, technique, "baseline", ())
            continue
        images = {}
        for name, options, mode, saturation, contrast in cases:
            image = capture(args.renderer.resolve(), output, model, technique, name, options)
            images[name] = image
            grade = advanced[name][1] if name in advanced else {}
            check_samples(image, CENTERS, COLORS, mode, saturation, contrast, **grade)
        if ImageChops.difference(images["default"], images["aces"]).getbbox():
            raise RuntimeError(f"{technique} explicit ACES differs from defaults")
        baseline = output / f"{technique}-baseline.png"
        if baseline.exists():
            with Image.open(baseline) as image:
                if ImageChops.difference(images["default"], image.convert("RGB")).getbbox():
                    raise RuntimeError(f"{technique} default differs from the pre-change renderer")
        from_flags = capture(args.renderer.resolve(), output, model, technique, "preset-flags", preset_options)
        from_preset = capture(args.renderer.resolve(), output, model, technique, "preset-file",
                              ("--display-grade", str(preset)))
        if ImageChops.difference(from_flags, from_preset).getbbox():
            raise RuntimeError(f"{technique} preset reload changed the rendered output")
        modes = ("aces", "reinhard", "clip", "filmic", "uncharted", "agx", "neutral")
        for mode in modes:
            image = capture(args.renderer.resolve(), output, ramp, technique, f"ramp-{mode}",
                            ("--tone-map", mode))
            check_samples(image, ramp_centers, ramp_colors, mode)
        print(f"{technique}: {len(cases)} chart cases, {len(modes)} ramps and preset reload passed", flush=True)

    assert hashlib.sha256(args.renderer.read_bytes()).hexdigest() == renderer_hash
    (output / "results.json").write_text(json.dumps(dict(
        passed=not args.baseline, baselineOnly=args.baseline,
        rendererSha256=renderer_hash, captures=len(RECORDS), records=RECORDS,
        analyticToneCurves=not args.baseline, presetValidation=not args.baseline,
    ), indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
