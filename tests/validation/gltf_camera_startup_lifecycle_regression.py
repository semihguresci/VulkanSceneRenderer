"""Verify that CLI camera and clip selections bind to the startup asset."""

import argparse
import hashlib
import json
from pathlib import Path
import re

from gltf_animation_runtime_regression import require, write_fixture
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_material_variants_runtime_regression import run


TECHNIQUES = ('deferred-raster', 'forward-raster')
CAMERA = 2
STARTUP = ('--gltf-camera-node', str(CAMERA), '--animation-clip', '0', '--animation-time', '.25')
SETTINGS = ('--sky', 'plain', '--import-scale', '1', '--no-ray-query', '--screenshot-viewport')


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def make_fixtures(output):
    source, static, alias = (output / f'{name}.gltf' for name in ('startup', 'no-camera', 'same-source-index'))
    write_fixture(source)
    document = json.loads(source.read_text(encoding='utf-8'))
    document['scenes'][0]['name'] = 'Startup scene'
    write_json(source, document)
    document['scenes'][0]['name'] = 'Replacement scene'
    document['nodes'][CAMERA].update(name='Replacement camera', translation=[.6, .2, 3.5])
    document['cameras'][0]['perspective']['yfov'] = .85
    document['animations'][0]['name'] = 'Replacement clip'
    write_json(alias, document)
    document['scenes'][0]['name'] = 'Static scene'
    document['nodes'][CAMERA].pop('camera')
    document.pop('cameras')
    document.pop('animations')
    document['extensionsUsed'].remove('KHR_animation_pointer')
    write_json(static, document)

    lifecycle, first_frame = (output / f'{name}.json' for name in ('lifecycle', 'first-frame-reload'))
    write_json(lifecycle, dict(schemaVersion=1, frames=9, sampleFrames=[1, 3, 5, 7, 9], events=[
        dict(frame=2, guiReload=str(static), guiWaitForLoad=True),
        dict(frame=4, guiReload=str(alias), guiWaitForLoad=True),
        dict(frame=6, cameraSelection=dict(nodeName='Replacement camera')),
        dict(frame=8, animationClip=0, animationTime=.75, animationPlaying=False),
    ]))
    write_json(first_frame, dict(schemaVersion=1, frames=1, sampleFrames=[1],
                                 events=[dict(frame=1, guiReload=str(alias), guiWaitForLoad=True)]))
    return source, static, lifecycle, first_frame


def check_state(state, scene, camera, clip, time, playing, camera_count, clip_count):
    animation = state['gltfAnimation']
    authoring = state['gltfCameraAuthoring']
    require(state['gltfScenes']['names'] == [scene] and state['gltfScenes']['selected'] == 0 and
            state['scene']['primaryObjectCount'] == 1, f'Wrong active asset for {scene}')
    require(animation['selectedCameraNode'] == authoring['selectedSourceNode'] == camera and
            len(authoring['cameras']) == camera_count and ('view' in authoring) == (camera >= 0),
            f'Wrong authored camera binding for {scene}: {authoring}')
    require(animation['selectedClip'] == clip and animation['clipCount'] == clip_count and
            animation['playing'] == playing and abs(animation['time'] - time) < 1e-6,
            f'Wrong clip state for {scene}: {animation}')
    if camera >= 0:
        binding, = authoring['cameras']
        node, = (node for node in state['gltfMerge']['authoringNodes'] if node['graphNode'] == binding['graphNode'])
        expected_name = 'Authored camera' if scene == 'Startup scene' else 'Replacement camera'
        require(binding['sourceNode'] == CAMERA and node['active'] and node['name'] == expected_name,
                f'Selected the wrong camera node for {scene}: {node}')


def rejected_startup(capture, output, technique, name, model, sequence, args, diagnostic):
    prefix = f'{technique}-{name}'
    try:
        capture(name, model, sequence, args)
    except RuntimeError as error:
        status = re.match(r'Variant capture failed \((-?\d+)\): ', str(error))
        require(status is not None and int(status[1]) != 0, f'Wrong startup rejection: {error}')
    else:
        raise RuntimeError(f'Invalid startup selector succeeded: {prefix}')
    log = output / f'{prefix}.log'
    content = log.read_text(encoding='utf-8', errors='replace')
    require(diagnostic in content and not re.search(r'VUID-|SYNC-HAZARD|Validation Error', content, re.I) and
            'Insert instance layer "VK_LAYER_KHRONOS_validation"' in content and
            'Inserted device layer "VK_LAYER_KHRONOS_validation"' in content,
            f'Invalid startup rejection diagnostic or Vulkan log: {log}')
    require(not list(output.glob(f'{prefix}*.telemetry.json')) and not list(output.glob(f'{prefix}*.png')),
            'Invalid startup selector rendered a frame')
    return dict(technique=technique, name=name, log=str(log), diagnostic=diagnostic,
                returnCode=int(status[1]), logSha256=hashlib.sha256(log.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--technique', choices=TECHNIQUES, action='append')
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    techniques = tuple(dict.fromkeys(args.technique or TECHNIQUES))
    output = args.output.resolve()
    output.mkdir(parents=True)
    report = dict(schemaVersion=1, passed=False, nativeValidationPerformed=False, generatedOnly=args.generate_only,
                  captures=[], checks=[], rejected=[])
    try:
        source, static, lifecycle, first_frame = make_fixtures(output)
        if args.generate_only:
            report['passed'] = True
            return
        require(args.renderer is not None and args.manifest is not None, 'Native validation needs a renderer and manifest')
        renderer, manifest = args.renderer.resolve(), args.manifest.resolve()
        (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
        report.update(rendererSha256=hashlib.sha256(renderer.read_bytes()).hexdigest(),
                      nativeFreeze=verify_runtime_manifest(manifest, renderer))
        for technique in techniques:
            def capture(name, model, sequence, extra=STARTUP):
                return run(renderer, output, model, f'{technique}-{name}', technique, 'lit', report['captures'],
                           sequence=sequence, extra=(*SETTINGS, *extra), require_validation=True, with_ui=True)

            rows = capture('lifecycle', source, lifecycle)
            expected = (
                ('Startup scene', CAMERA, 0, .25, True, 1, 4),
                ('Static scene', -1, -1, 0, False, 0, 0),
                ('Replacement scene', -1, -1, 0, False, 1, 4),
                ('Replacement scene', CAMERA, -1, 0, False, 1, 4),
                ('Replacement scene', CAMERA, 0, .75, False, 1, 4),
            )
            for row, check in zip(rows, expected, strict=True):
                check_state(row['state'], *check)
                report['checks'].append(dict(technique=technique, capture=row['telemetry']))
            initial, after_static, after_alias, after_select, _ = (row['state'] for row in rows)
            loading = [state['gui']['modelLoading'] for state in (after_static, after_alias)]
            require(all(not state['pending'] and state['completed'] == count for count, state in enumerate(loading, 1)),
                    f'GUI model loads did not finish: {loading}')
            require(len({state['gltfScenes']['graphIdentity'] for state in (initial, after_static, after_alias)}) == 3 and
                    initial['taa']['epoch'] < after_static['taa']['epoch'] < after_alias['taa']['epoch'] < after_select['taa']['epoch'],
                    'Replacement or camera selection retained the previous temporal history')

            first, = capture('first-frame-reload', source, first_frame)
            check_state(first['state'], *expected[2])
            require(first['state']['gui']['modelLoading']['completed'] == 1, 'First-frame model replacement did not finish')
            report['checks'].append(dict(technique=technique, capture=first['telemetry']))
            report['rejected'].extend((
                rejected_startup(capture, output, technique, 'invalid-startup-camera', static, first_frame, STARTUP,
                                 'configured glTF camera node has no active source camera binding'),
                rejected_startup(capture, output, technique, 'invalid-startup-clip', source, first_frame,
                                 ('--gltf-camera-node', str(CAMERA), '--animation-clip', '99'),
                                 'animation clip index out of range'),
            ))
        require(len(report['captures']) == 6 * len(techniques) and len(report['rejected']) == 2 * len(techniques) and
                verify_runtime_manifest(manifest, renderer) == report['nativeFreeze'], 'Incomplete or changed startup selection run')
        report.update(passed=True, nativeValidationPerformed=True)
    finally:
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']),
                      rejectionCount=len(report['rejected']))
        write_json(output / 'results.json', report)


if __name__ == '__main__':
    main()
