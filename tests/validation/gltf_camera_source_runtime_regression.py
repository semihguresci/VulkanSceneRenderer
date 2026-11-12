"""Verify authored camera source validation and integral-decimal camera references."""

import argparse
import copy
import json
import math
import os
from pathlib import Path
import subprocess

from PIL import Image

from gltf_accessor_runtime_regression import Asset, load_asset, mesh, require
from gltf_compression_runtime_regression import write_glb
from gltf_extended_materials_runtime_regression import sha, validation_log_error
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_material_variants_runtime_regression import pixels, run


TECHNIQUES = ('deferred-raster', 'forward-raster')
SETTINGS = ('--sky', 'plain', '--directional-intensity', '0', '--import-scale', '1', '--no-ray-query')
MISSING_BUFFER = 'camera-preflight-unreachable.bin'
NAMES = ('Perspective owner', 'Orthographic owner')
LENSES = [dict(name='Infinite perspective', type='perspective',
               values=[.7, 0, .125, 0, 0, 0, 0, 0]),
          dict(name='Finite orthographic', type='orthographic',
               values=[0, 0, 0, 0, 1.25, .75, .25, 48])]


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def fixture(output):
    asset = Asset()
    mesh(asset, asset.accessor('VEC3', [[-.7, -.6, 0], [.7, -.6, 0], [.7, .6, 0], [-.7, .6, 0]]),
         asset.accessor('VEC3', [[0, 0, 1]] * 4),
         asset.accessor('VEC2', [[0, 0], [1, 0], [1, 1], [0, 1]]),
         tangent=asset.accessor('VEC4', [[1, 0, 0, 1]] * 4))
    model = asset.model
    model['asset']['generator'] = 'Container camera source regression'
    model['materials'] = [dict(pbrMetallicRoughness=dict(baseColorFactor=[.04, .4, .85, 1]),
                              extensions=dict(KHR_materials_unlit={}))]
    model['extensionsUsed'] = ['KHR_materials_unlit']
    model['cameras'] = [dict(name=LENSES[0]['name'], type='perspective', perspective=dict(yfov=.7, znear=.125)),
                        dict(name=LENSES[1]['name'], type='orthographic',
                             orthographic=dict(xmag=1.25, ymag=.75, znear=.25, zfar=48))]
    model['nodes'] = [dict(name='Unlit panel', mesh=0),
                      dict(name='Camera rig', translation=[.125, -.125, 0], children=[2, 3]),
                      dict(name=NAMES[0], camera=0, translation=[0, 0, 4]),
                      dict(name=NAMES[1], camera=1, translation=[.375, -.125, 3])]
    model['scenes'][0]['nodes'] = [0, 1]
    integer, decimal, binary = (output / file for file in ('integer.gltf', 'decimal.gltf', 'decimal.glb'))
    asset.write(integer)
    decimal_model = copy.deepcopy(model)
    decimal_model['nodes'][2]['camera'] = 0.0
    decimal_model['nodes'][3]['camera'] = 1.0
    text = json.dumps(decimal_model, indent=2).replace('"camera": 1.0', '"camera": 1e0')
    decimal.write_text(text + '\n', encoding='utf-8')
    decimal_model['buffers'][0].pop('uri')
    write_glb(binary, decimal_model, bytes(asset.data))

    sequence = output / 'select-cameras.json'
    write_json(sequence, dict(schemaVersion=1, frames=6, sampleFrames=[3, 6], events=[
        dict(frame=1, cameraSelection=dict(nodeName=NAMES[0])),
        dict(frame=4, cameraSelection=dict(sourceNode=3)),
    ]))

    malformed = []

    def reject(name, keys, value, diagnostic, *, omit=False):
        candidate = copy.deepcopy(model)
        # An external mesh read would fail differently. The camera error must
        # win before the parser resolves even a required resource.
        candidate['buffers'][0]['uri'] = MISSING_BUFFER
        target = candidate
        for key in keys[:-1]:
            target = target[key]
        if omit:
            del target[keys[-1]]
        else:
            target[keys[-1]] = value
        path = output / f'{name}.gltf'
        write_json(path, candidate)
        malformed.append(dict(name=name, fixture=str(path), diagnostic=diagnostic))

    index_context = 'glTF scene hierarchy: nodes[2].camera'
    index_keys = ('nodes', 2, 'camera')
    for name, index in [('overflow-index', 4294967296), ('negative-index', -1),
                        ('fractional-index', .5), ('boolean-index', True)]:
        reject(name, index_keys, index, index_context + ' must be a nonnegative integer index')
    reject('out-of-range-index', index_keys, 2, index_context + ' references a missing entry')
    reject('missing-camera-table', ('cameras',), None, index_context + ' references a missing entry', omit=True)
    reject('invalid-camera-table', ('cameras',), {}, 'glTF cameras: cameras')
    reject('invalid-camera-type', ('cameras', 0, 'type'), 'pinhole', 'glTF cameras: cameras[0].type')
    reject('zero-perspective-far', ('cameras', 0, 'perspective', 'zfar'), 0, 'glTF cameras: cameras[0].perspective.zfar')
    reject('nonnumeric-aspect', ('cameras', 0, 'perspective', 'aspectRatio'), 'auto',
           'glTF cameras: cameras[0].perspective.aspectRatio')
    reject('reversed-orthographic-range', ('cameras', 1, 'orthographic', 'zfar'), .125,
           'glTF cameras: cameras[1].orthographic.zfar')
    return dict(integer=integer, decimal=decimal, binary=binary), sequence, malformed


def assert_valid(record, selected):
    state = record['state']
    camera = state['gltfCameraAuthoring']
    require(camera['authored'] == camera['evaluated'] == camera['rests'] == LENSES and
            camera['selectedSourceNode'] == 2 + selected and not camera['truncated'],
            f'Wrong authored camera defaults or selection: {record["telemetry"]}')
    bindings = sorted((row['sourceNode'], row['authoredCamera'], row['evaluatedCamera'])
                      for row in camera['cameras'])
    require(bindings == [(2, 0, 0), (3, 1, 1)], f'Camera references changed: {record["telemetry"]}')
    require(state['camera']['position'] == ([.125, -.125, 4] if selected == 0 else [.5, -.25, 3]),
            f'Authored rig transform was not used: {record["telemetry"]}')
    projection = camera['projection']
    if selected == 0:
        require(projection[10:12] == [0, -1] and projection[14:] == [.125, 0] and
                math.isclose(projection[5] / projection[0], 640 / 360, rel_tol=1e-6),
                f'Omitted perspective far or aspect changed: {record["telemetry"]}')
    else:
        require(projection[15] == 1 and math.isclose(projection[0], 1 / 1.25, rel_tol=1e-6) and
                math.isclose(projection[5], 1 / .75, rel_tol=1e-6),
                f'Orthographic lens was not used: {record["telemetry"]}')
    with Image.open(record['image']) as image:
        red, _, blue = image.convert('RGB').getpixel((320, 180))
        require(blue > red + 30, f'Camera capture has no visible blue surface: {record["image"]}')
    return camera


def assert_export(path):
    model, _ = load_asset(path)
    cameras = model['cameras']
    require(len(cameras) == 2 and cameras[0]['perspective'] == dict(yfov=.7, znear=.125) and
            cameras[1]['orthographic'] == dict(xmag=1.25, ymag=.75, znear=.25, zfar=48),
            f'Save altered the camera lens or wrote omitted perspective fields: {path}')
    bindings = {node['name']: node['camera'] for node in model['nodes'] if 'camera' in node}
    require(bindings == dict(zip(NAMES, (0, 1), strict=True)) and all(type(value) is int for value in bindings.values()),
            f'Save changed the normalized camera references: {path}')


def reject_launch(renderer, output, case, technique, timeout):
    image = output / (case['name'] + '.png')
    log = image.with_suffix('.log')
    command = [str(renderer), '--hidden', '--no-ui', '--validation', '--width', '64', '--height', '64',
               '--msaa', '1', '--no-taa', '--warmup-frames', '1', '--capture-frame', '2',
               '--render-technique', technique, '--model', case['fixture'], '--screenshot', str(image), *SETTINGS]
    environment = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG='layer')
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=timeout, env=environment,
                                 creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    text = log.read_text(encoding='utf-8', errors='replace')
    require(process.returncode != 0 and case['diagnostic'] in text and MISSING_BUFFER not in text,
            f'Malformed camera did not reject before resource I/O: {log}')
    require(not image.exists() and not image.with_suffix('.telemetry.json').exists(),
            f'Malformed camera produced capture evidence: {log}')
    require(not validation_log_error(text), f'Vulkan validation error: {log}')
    return dict(case, technique=technique, returnCode=process.returncode, log=str(log), logSha256=sha(log), command=command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--timeout', type=float, default=240)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    techniques = tuple(dict.fromkeys(args.technique or TECHNIQUES))
    report = dict(schemaVersion=1, passed=False, generatedOnly=args.generate_only, nativeValidationPerformed=False,
                  captures=[], checks=[], exports=[], rejected=[])
    try:
        sources, sequence, malformed = fixture(output)
        inputs = [*sources.values(), sequence, *(Path(case['fixture']) for case in malformed)]
        report.update(expectedCaptureCount=2 * len(sources) * len(techniques),
                      expectedRejectedLaunches=len(malformed), malformedFixtures=malformed,
                      inputs=[dict(path=str(path), sha256=sha(path)) for path in inputs])
        if args.generate_only:
            report['passed'] = True
            return
        require(args.renderer is not None and args.manifest is not None, 'Native validation requires renderer and manifest')
        require(math.isfinite(args.timeout) and args.timeout > 0, 'Timeout must be finite and positive')
        renderer, manifest = args.renderer.resolve(), args.manifest.resolve()
        report.update(rendererSha256=sha(renderer), nativeFreeze=verify_runtime_manifest(manifest, renderer))
        (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
        for technique in techniques:
            integer = None
            for name, source in sources.items():
                export = None if name == 'integer' else output / f'{technique}-{name}-saved{source.suffix}'
                rows = run(renderer, output, source, f'{technique}-{name}', technique, 'lit', report['captures'],
                           sequence=sequence, save=export, timeout=args.timeout, extra=SETTINGS,
                           require_validation=True, use_authored_sun=True)
                require(len(rows) == 2, f'Wrong capture count: {technique}/{name}')
                for selected, row in enumerate(rows):
                    camera = assert_valid(row, selected)
                    check = dict(technique=technique, fixture=name, lens=LENSES[selected]['type'])
                    if integer is not None:
                        reference = integer[selected]
                        oracle = reference['state']['gltfCameraAuthoring']
                        require(all(camera[key] == oracle[key] for key in ('view', 'projection', 'shadowProjection')),
                                f'Normalized camera matrix differs from integer control: {technique}/{name}')
                        metrics = pixels(reference['image'], row['image'])
                        require(metrics['maximumError'] == 0, f'Normalized camera pixels changed: {technique}/{name}')
                        check.update(metrics)
                    report['checks'].append(check)
                if integer is None:
                    integer = rows
                    require(pixels(rows[0]['image'], rows[1]['image'])['changedPixelFraction'] > .05,
                            f'Perspective and orthographic captures are not distinguishable: {technique}')
                if export is not None:
                    assert_export(export)
                    report['exports'].append(dict(technique=technique, path=str(export), sha256=sha(export)))
        for case in malformed:
            report['rejected'].append(reject_launch(renderer, output, case, techniques[0], args.timeout))
        for log in {row['log'] for row in report['captures']}:
            text = Path(log).read_text(encoding='utf-8', errors='replace')
            require('Inserted device layer "VK_LAYER_KHRONOS_validation"' in text and not validation_log_error(text),
                    f'Device validation is missing or reported an error: {log}')
        require(report['nativeFreeze'] == verify_runtime_manifest(manifest, renderer) and
                report['rendererSha256'] == sha(renderer) and len(report['captures']) == report['expectedCaptureCount'] and
                len(report['rejected']) == report['expectedRejectedLaunches'] and
                all(sha(Path(entry['path'])) == entry['sha256'] for entry in report['inputs']),
                'Incomplete or changed camera source run')
        report.update(passed=True, nativeValidationPerformed=True)
    finally:
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']), exportCount=len(report['exports']),
                      rejectionCount=len(report['rejected']))
        write_json(output / 'results.json', report)


if __name__ == '__main__':
    main()
