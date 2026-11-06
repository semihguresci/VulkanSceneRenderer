"""Native animation/deformation save and reload checks with portable fixtures.

Exports from a nonzero animation pose, then compares both endpoints after
reload. This catches saving only the current pose or losing/remapping tracks.
Requires NumPy, Pillow and the built Vulkan renderer; no downloaded assets.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

import numpy as np
from PIL import Image

import gltf_animation_runtime_regression as animation
import gltf_deformation_runtime_regression as deformation


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_model(path):
    data = path.read_bytes()
    if data[:4] == b'glTF':
        magic, version, length, size, kind = struct.unpack_from('<5I', data)
        require(version == 2 and length == len(data) and kind == 0x4e4f534a,
                f'Invalid exported GLB: {path}')
        return json.loads(data[20:20 + size])
    return json.loads(data)


def append_range_clip(path):
    model = read_model(path)
    require(len(model['animations']) == 4, 'Range fixture requires the four core animation clips')
    clip = copy.deepcopy(model['animations'][2])
    clip['name'] = 'Light range in authored units'
    clip['channels'][0]['target']['extensions']['KHR_animation_pointer']['pointer'] = (
        '/extensions/KHR_lights_punctual/lights/0/range')
    model['animations'].append(clip)
    # With importScale2 this places front-face distances on both sides of the
    # short world range4. The original lamp left that endpoint entirely dark.
    model['nodes'][1]['translation'] = [1, 1, 1.5]
    path.write_text(json.dumps(model, indent=2), encoding='utf-8')


def check_texture_metadata(source, saved):
    expected = source['materials'][0]['pbrMetallicRoughness']['baseColorTexture']['extensions']['KHR_texture_transform']['extras']
    # Resolve the live exported material through the emitted primitive binding;
    # an archived source material alone must not make this check succeed.
    materials = {primitive['material'] for mesh in saved['meshes'] for primitive in mesh['primitives']
                 if 'material' in primitive}
    matches = [saved['materials'][index] for index in materials
               if saved['materials'][index].get('name') == source['materials'][0]['name']]
    require(len(matches) == 1, 'Animated texture material binding was lost or duplicated')
    actual = matches[0]['pbrMetallicRoughness']['baseColorTexture']['extensions']['KHR_texture_transform']
    require(actual.get('extras') == expected, 'Texture-transform authored metadata was lost')
    require(saved.get('samplers') == source.get('samplers'), 'Export changed authored sampler policies')
    return dict(extras=actual['extras'], samplersPreserved=True)


def check_point_light(before, after, case, clip, time):
    left = before['state']['gltfAnimation']['pointLights']
    right = after['state']['gltfAnimation']['pointLights']
    require(len(left) == len(right) == 1, 'Fixture must retain its one punctual light binding')
    expected_range = 2 * (2 + 78 * time) if case == 'animation' and clip == 4 else 20
    expected_intensity = 2 + 78 * time if clip == 2 else 30
    expected_position = [2, 2, 3] if case == 'animation' else [4, 4, 6]
    for light in (left[0], right[0]):
        require(abs(light['range'] - expected_range) < 1e-5,
                f'Authored range lost import scaling at clip{clip}/time{time:g}: {light}')
        require(abs(light['intensity'] - expected_intensity) < 1e-5,
                f'Authored light intensity drifted across clips: {light}')
        require(len(light['color']) == 3 and max(abs(value - 1) for value in light['color']) < 1e-6,
                f'Authored light color drifted across clips: {light}')
        require(len(light['position']) == 3 and
                max(abs(a - b) for a, b in zip(light['position'], expected_position)) < 1e-5,
                f'Light pose did not retain its world units: {light}')
    return dict(source=left[0], reload=right[0])


def check_visible_range_endpoints(records):
    checks = {}
    for kind in ('source', 'reload'):
        a, b = (np.asarray(Image.open(record[kind]['image']).convert('RGB'), dtype=float) for record in records)
        delta = np.abs(a-b)
        changed = float(np.mean(np.max(delta, axis=2) >= 8))
        mean = float(delta.mean())
        require(changed >= .002 and mean >= .1,
                f'{kind} range endpoints did not visibly change: changed={changed:.4%}, MAE={mean:.4f}')
        require(float(b.mean()) > float(a.mean()) + .1,
                f'{kind} longer range did not increase fixture brightness')
        checks[kind] = dict(changedPixelFraction=changed, meanAbsolutePixelError=mean)
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=('animation', 'texture', 'defaults', 'skin', 'shared', 'combined'))
    parser.add_argument('--format', action='append', choices=('glb', 'gltf'))
    parser.add_argument('--technique', action='append', choices=('deferred-raster', 'forward-raster'))
    parser.add_argument('--clip', type=int, action='append', choices=range(5),
                        help='Restrict clips per case; animation4 is range and texture4 is texture offset')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    require(renderer.is_file(), f'Missing renderer: {renderer}')
    output.mkdir(parents=True, exist_ok=True)
    (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
    binary_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report = dict(schemaVersion=1, passed=False, executableSha256=binary_hash,
                  requestedClips=args.clip, captures=[], checks=[], rangeEndpointChecks=[], textureMetadataChecks=[], generatedCases=[])

    def save():
        report['captureCount'] = len(report['captures'])
        report['referencePairCount'] = len(report['checks'])
        (output / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')

    def capture(path, name, technique, clip, time, spec, scale=1, destination=None, camera_node=None):
        image = output / (name + '.png')
        telemetry = image.with_suffix('.telemetry.json')
        image.unlink(missing_ok=True)
        telemetry.unlink(missing_ok=True)
        command = [str(renderer), '--hidden', '--no-ui', '--validation', '--width', '640', '--height', '360',
                   '--msaa', '1', '--warmup-frames', '3', '--capture-frame', '4', '--fixed-dt', '.016666667',
                   '--display-mode', 'lit', '--no-bloom', '--environment-intensity', str(spec['environment']),
                   '--directional-intensity', str(spec['sun']), '--exposure', '1',
                   '--camera-position', *map(str, spec['camera']), '--camera-target', *map(str, spec['target']),
                   '--camera-fov', '45', '--render-technique', technique, '--model', str(path),
                   '--import-scale', str(scale), '--screenshot', str(image)]
        if clip is not None:
            command += ['--animation-clip', str(clip), '--animation-time', str(time), '--animation-paused']
        if camera_node is not None:
            command += ['--gltf-camera-node', str(camera_node)]
        if destination:
            destination.unlink(missing_ok=True)
            command += ['--save-scene', str(destination)]
        log = image.with_suffix('.log')
        with log.open('w', encoding='utf-8') as stream:
            process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                     timeout=args.timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                     creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        text = log.read_text(encoding='utf-8', errors='replace')
        require(process.returncode == 0, f'Native export/reload failed ({process.returncode}): {log}')
        require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error', text, re.I), f'Vulkan validation failed: {log}')
        require(image.is_file() and telemetry.is_file(), f'Missing fresh capture: {image}')
        if destination:
            require(destination.is_file(), f'Export did not produce {destination}; see {log}')
        state = json.loads(telemetry.read_text(encoding='utf-8'))
        require(animation.finite_json(state), f'Nonfinite telemetry: {telemetry}')
        require(state['technique'] == technique, 'Wrong raster technique')
        if clip is not None:
            actual = state['gltfAnimation']
            require(actual['selectedClip'] == clip and not actual['playing'] and abs(actual['time']-time) < 1e-6,
                    f'Animation seek did not survive reload: {telemetry}')
            require(actual['diagnostics'] == 0, f'Exported animation targets were omitted: {telemetry}')
        record = dict(image=str(image), telemetry=str(telemetry), log=str(log), command=command)
        report['captures'].append(record)
        save()
        return dict(**record, state=state)

    save()
    try:
        for case in args.case or ('animation', 'texture', 'defaults', 'skin', 'shared', 'combined'):
            source = output / (case + '-source.gltf')
            if case in ('animation', 'texture'):
                animation.write_fixture(source, texture_motion=case == 'texture')
                if case == 'animation':
                    append_range_clip(source)
                spec = dict(camera=[0, 0, 6], target=[0, 0, 0], environment=0, sun=0)
                # Scaling is encoded in the exported graph, camera and range.
                scale = 2
                case_clips = tuple(range(5)) if case == 'animation' else (4,)
                times = (0., 1.)
            else:
                spec = deformation.spec_for(case)
                deformation.write_fixture(source, spec)
                spec.update(environment=.35, sun=3)
                scale = 1
                case_clips = (0,) if spec['tracks'] else (None,)
                times = (0., 1.) if spec['tracks'] else (0.,)
            clips = [clip for clip in case_clips if args.clip is None or clip in args.clip]
            if not clips:
                continue
            report['generatedCases'].append(dict(case=case, source=str(source), clips=clips,
                                                  sourceImportScale=scale, reloadImportScale=1))
            if args.generate_only:
                continue
            source_model = read_model(source)
            for extension in args.format or ('glb',):
                destination = output / (case + '-saved.' + extension)
                # Focused replay still saves the same nonzero source pose. In
                # particular clip1 must not inherit a saved clip0 translation.
                export_clip = case_clips[0]
                capture(source, case + '-' + extension + '-save', 'deferred-raster', export_clip, .65 if export_clip is not None else 0,
                        spec, scale, destination)
                saved_model = read_model(destination)
                require(len(saved_model.get('animations', [])) == len(source_model.get('animations', [])),
                        'Export changed animation clip count')
                if case == 'texture':
                    report['textureMetadataChecks'].append(dict(format=extension,
                        **check_texture_metadata(source_model, saved_model)))
                    save()
                if source_model.get('skins'):
                    require(len(saved_model.get('skins', [])) >= len(source_model['skins']), 'Export discarded skins')
                if any(p.get('targets') for m in source_model['meshes'] for p in m['primitives']):
                    require(any(p.get('targets') for m in saved_model['meshes'] for p in m['primitives']),
                            'Export baked away morph targets')
                for technique in args.technique or ('deferred-raster', 'forward-raster'):
                    for clip in clips:
                        endpoints = []
                        for time in times:
                            name = f'{case}-{extension}-{technique}-clip{clip}-time{time:g}'
                            source_camera = 2 if case == 'animation' and clip == 3 else None
                            saved_camera = None
                            if source_camera is not None:
                                cameras = [i for i, node in enumerate(saved_model['nodes']) if 'camera' in node]
                                require(len(cameras) == 1, 'Authored camera binding lost or duplicated')
                                saved_camera = cameras[0]
                            before = capture(source, name + '-source', technique, clip, time, spec, scale, camera_node=source_camera)
                            after = capture(destination, name + '-reload', technique, clip, time, spec, camera_node=saved_camera)
                            a = np.asarray(Image.open(before['image']).convert('RGB'), dtype=float)
                            b = np.asarray(Image.open(after['image']).convert('RGB'), dtype=float)
                            require(a.shape == b.shape == (360, 640, 3) and a.std() > 3, 'Vacuous or wrong-sized comparison')
                            delta = np.abs(a-b)
                            result = dict(case=case, format=extension, technique=technique, clip=clip, time=time,
                                          meanAbsolutePixelError=float(delta.mean()), percentile99_9PixelError=float(np.percentile(delta, 99.9)))
                            if case in ('animation', 'texture'):
                                result['pointLight'] = check_point_light(before, after, case, clip, time)
                            else:
                                left, right = before['state']['gltfDeformation'], after['state']['gltfDeformation']
                                require(left['active'] and right['active'] and right['currentBoundToGpu'],
                                        'Reload no longer renders dynamic geometry')
                                require(left['instanceCount'] == right['instanceCount'], 'Reload lost independent instances')
                            require(result['meanAbsolutePixelError'] <= .1 and result['percentile99_9PixelError'] <= 2,
                                    f'Animated round-trip appearance differs: {result}')
                            report['checks'].append(result)
                            endpoints.append(dict(source=before, reload=after))
                            save()
                            print('PASS', json.dumps(result), flush=True)
                        if case == 'animation' and clip == 4:
                            report['rangeEndpointChecks'].append(dict(case=case, format=extension, technique=technique,
                                **check_visible_range_endpoints(endpoints)))
                            save()
        require(report['generatedCases'], 'No requested clip applies to the selected cases')
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == binary_hash, 'Renderer changed during native regression')
        report['passed'] = True
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        save()
    print(f"PASS: {report['referencePairCount']} pairs, {report['captureCount']} native captures; {output}")


if __name__ == '__main__':
    main()
