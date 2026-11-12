"""Verify periodic HDR longitude filtering in the rendered sky."""

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

from display_grade_runtime_regression import expected_rgb
from gltf_accessor_runtime_regression import require
from gltf_extended_materials_runtime_regression import extension_material, fixture, sha, validation_log_error
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_material_variants_runtime_regression import run
from hdr_environment_runtime_regression import ENV_HEIGHT, ENV_WIDTH, write_exr, write_hdr


TECHNIQUES = ('deferred-raster', 'forward-raster')
WIDTH, HEIGHT, FOV = 640, 360, 45
SKY = (268, 32, 372, 64)
MAX_CODE_ERROR = 4
YAW = -90
OPTIONS = ('--width', str(WIDTH), '--height', str(HEIGHT), '--camera-fov', str(FOV),
           '--no-ray-query', '--ray-shadows', 'raster', '--import-scale', '1',
           '--directional-intensity', '0', '--environment-intensity', '1',
           '--tone-map', 'clip', '--background-blur', '0')


def radiance(rgbe):
    return np.array(rgbe[:3], dtype=np.float64) * 2.0 ** (rgbe[3] - 136)


def linear_sample(row, longitude, *, periodic=True):
    coordinate = (longitude % 1.0) * len(row) - .5
    left = math.floor(coordinate)
    weight = coordinate - left
    if periodic:
        indices = (left % len(row), (left + 1) % len(row))
    else:
        indices = (max(0, left), min(left + 1, len(row) - 1))
    return radiance(row[indices[0]]) * (1 - weight) + radiance(row[indices[1]]) * weight


def expected_sky(row, baked, *, periodic=True):
    colors = []
    for x in range(SKY[0], SKY[2]):
        horizontal = (2 * (x + .5) - WIDTH) / HEIGHT * math.tan(math.radians(FOV / 2))
        # The unrotated camera looks along -Z at longitude .25. A -90-degree
        # yaw moves that direction to -X, where the source longitude wraps.
        longitude = math.atan(horizontal) / math.tau
        color = linear_sample(row, longitude, periodic=periodic)
        require(np.allclose(color, linear_sample(baked, .25 + longitude, periodic=periodic), atol=1e-12)
                or not periodic, 'Circularly shifted oracle does not match the rotated source')
        colors.append(expected_rgb(color, 'clip', 1., 1.))
    return np.asarray(colors, dtype=np.int16)[None, :, :]


def compare(report, technique, name, actual, reference, limit=MAX_CODE_ERROR):
    delta = np.abs(actual - reference)
    check = dict(technique=technique, name=name, maximumCodeError=int(delta.max()),
                 meanCodeError=float(delta.mean()), allowedCodeError=limit)
    report['checks'].append(check)
    require(check['maximumCodeError'] <= limit,
            f'{technique} {name}: {check["maximumCodeError"]} display codes, maximum {limit}')


def capture(renderer, output, model, path, label, yaw, technique, report):
    captures = run(renderer, output, model, f'{technique}-{label}', technique, 'lit', report['captures'],
                   extra=(*OPTIONS, '--environment', str(path), '--environment-yaw', str(yaw)),
                   require_validation=True, use_authored_sun=True)
    require(len(captures) == 1, f'{technique} {label}: expected one capture')
    result, = captures
    log = Path(result['log']).read_text(encoding='utf-8', errors='replace')
    require(not validation_log_error(log) and 'Inserted device layer "VK_LAYER_KHRONOS_validation"' in log,
            f'Vulkan validation did not pass: {result["log"]}')
    environment = result['state']['environment']
    require(Path(environment['hdrPath']) == path and environment['yawDegrees'] == yaw and
            not environment['loading'], f'{technique} {label}: wrong environment was published')
    with Image.open(result['image']) as image:
        require(image.size == (WIDTH, HEIGHT), f'{technique} {label}: unexpected capture size')
        return np.asarray(image.convert('RGB').crop(SKY), dtype=np.int16)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--technique', choices=TECHNIQUES, action='append')
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    techniques = tuple(dict.fromkeys(args.technique or TECHNIQUES))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = dict(schemaVersion=1, passed=False, generatedOnly=args.generate_only, nativeValidationPerformed=False,
                  captures=[], checks=[], expectedCaptureCount=4 * len(techniques),
                  expectedCheckCount=8 * len(techniques))
    try:
        quarter = ENV_WIDTH // 4
        row = ([(80, 16, 24, 129)] * quarter + [(24, 80, 16, 129)] * (2 * quarter) +
               [(16, 24, 80, 129)] * quarter)
        baked = row[-quarter:] + row[:-quarter]
        oracle = expected_sky(row, baked)
        clamped = expected_sky(row, baked, periodic=False)
        clamp_error = int(np.abs(oracle - clamped).max())
        require(clamp_error >= 30, 'The sky crop must distinguish clamped and periodic longitude')
        report.update(skyCrop=SKY, clampedOracleMaximumCodeError=clamp_error)

        model = output / 'receiver.gltf'
        fixture(model, {'material': extension_material(base=(.8, .8, .8), metallic=1., specular=1.)})
        paths = {}
        for kind, write in (('hdr', write_hdr), ('exr', write_exr)):
            source, shifted = output / f'source.{kind}', output / f'baked.{kind}'
            write(source, [row] * ENV_HEIGHT)
            write(shifted, [baked] * ENV_HEIGHT)
            paths[kind] = (source, shifted)
        settings = output / 'vk_layer_settings.txt'
        settings.write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
        report['inputs'] = [dict(path=str(path), sha256=sha(path)) for path in sorted(output.iterdir())]
        if args.generate_only:
            report['passed'] = True
            return

        require(args.renderer is not None and args.manifest is not None, 'Native validation requires renderer and manifest')
        renderer, manifest = args.renderer.resolve(), args.manifest.resolve()
        report.update(rendererSha256=sha(renderer), nativeFreeze=verify_runtime_manifest(manifest, renderer))
        for technique in techniques:
            images = {}
            for kind, (source, shifted) in paths.items():
                runtime = capture(renderer, output, model, source, f'{kind}-yaw', YAW, technique, report)
                reference = capture(renderer, output, model, shifted, f'{kind}-baked', 0, technique, report)
                compare(report, technique, f'{kind}-yaw-periodic-oracle', runtime, oracle)
                compare(report, technique, f'{kind}-baked-periodic-oracle', reference, oracle)
                compare(report, technique, f'{kind}-yaw-matches-circular-shift', runtime, reference)
                images[kind] = (runtime, reference)
            for name, hdr, exr in zip(('yaw', 'baked'), images['hdr'], images['exr'], strict=True):
                compare(report, technique, f'{name}-hdr-matches-exr', hdr, exr, limit=0)
            print(f'PASS {technique}: HDR and EXR seam filtering and baked rotation', flush=True)

        require(report['nativeFreeze'] == verify_runtime_manifest(manifest, renderer) and
                sha(renderer) == report['rendererSha256'] and
                all(sha(Path(row['path'])) == row['sha256'] for row in report['inputs']),
                'Frozen runtime or fixture inputs changed during validation')
        require(len(report['captures']) == report['expectedCaptureCount'] and
                len(report['checks']) == report['expectedCheckCount'], 'Incomplete seam verification')
        report.update(passed=True, nativeValidationPerformed=True)
    finally:
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        (output / 'results.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
