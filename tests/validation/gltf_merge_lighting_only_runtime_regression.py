#!/usr/bin/env python3
"""Verify bufferless light and camera imports, history, and source-free export on Vulkan."""

import argparse
import json
import math
import shutil
import struct
from pathlib import Path

from gltf_accessor_runtime_regression import Asset, load_asset, mesh, require
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_material_variants_runtime_regression import pixels, run
from gltf_merge_runtime_regression import placement


TECHNIQUES = ('deferred-raster', 'forward-raster')
EXTENSION = 'KHR_lights_punctual'
PLACEMENT = (0.6, 0.2, 0.0)
IMPORTED_LIGHT = (0.2, 0.1, 1.0)
IMPORTED_CAMERA = (0.1, 0.2, 3.0)


def lamp(name, color, intensity):
    return dict(name=name, type='point', color=color, intensity=intensity, range=4)


def camera(name, yfov):
    return dict(name=name, type='perspective', perspective=dict(yfov=yfov, znear=0.1, zfar=50))


def light_node(name, index, position):
    return dict(name=name, translation=list(position), extensions={EXTENSION: dict(light=index)})


def translated(position):
    return [value + offset for value, offset in zip(position, PLACEMENT)]


def primary(path, *, merged=False):
    asset = Asset()
    pos = asset.accessor('VEC3', [(-1.3, -0.9, 0), (1.3, -0.9, 0), (1.3, 0.9, 0), (-1.3, 0.9, 0)])
    normal = asset.accessor('VEC3', [(0, 0, 1)] * 4)
    uv = asset.accessor('VEC2', [(0, 0), (1, 0), (1, 1), (0, 1)])
    mesh(asset, pos, normal, uv)
    asset.model['materials'] = [dict(pbrMetallicRoughness=dict(
        baseColorFactor=[0.7, 0.7, 0.7, 1], metallicFactor=0, roughnessFactor=1))]
    model = asset.model
    model['extensionsUsed'] = [EXTENSION]
    lights = [lamp('Primary lamp', [0.25, 0.4, 1], 12)]
    model['extensions'] = {EXTENSION: dict(lights=lights)}
    model['cameras'] = [camera('Primary camera', 0.65)]
    model['nodes'].extend([light_node('Primary lamp node', 0, (-0.9, 0.2, 1.4)),
                           dict(name='Primary camera node', camera=0, translation=[0, 0, 4])])
    if merged:
        lights.append(lamp('Imported lamp', [1, 0.28, 0.12], 18))
        model['cameras'].append(camera('Imported camera', 0.9))
        model['nodes'].extend([light_node('Imported lamp node', 1, translated(IMPORTED_LIGHT)),
                               dict(name='Imported camera node', camera=1, translation=translated(IMPORTED_CAMERA))])
    model['scenes'][0]['nodes'] = list(range(len(model['nodes'])))
    asset.write(path)


def incoming(path):
    model = dict(asset=dict(version='2.0'), scene=0, scenes=[dict(nodes=[0])],
                 nodes=[dict(name='Incoming rig', children=[1, 2]), light_node('Imported lamp node', 0, IMPORTED_LIGHT),
                        dict(name='Imported camera node', camera=0, translation=list(IMPORTED_CAMERA))],
                 cameras=[camera('Imported camera', 0.9)], extensionsUsed=[EXTENSION], extensionsRequired=[EXTENSION],
                 extensions={EXTENSION: dict(lights=[lamp('Imported lamp', [1, 0.28, 0.12], 18)])})
    payload = json.dumps(model, indent=2).encode('utf-8')
    if path.suffix == '.glb':
        payload += b' ' * (-len(payload) % 4)
        payload = struct.pack('<5I', 0x46546C67, 2, len(payload) + 20, len(payload), 0x4E4F534A) + payload
    path.write_bytes(payload)
    restored, buffers = load_asset(path)
    require(restored == model and not buffers, 'Fixture must contain only the JSON document')


def check_portable(path):
    model, _ = load_asset(path)
    nodes = model['nodes']
    pending = list(model['scenes'][model['scene']]['nodes'])
    lights, cameras = [], []
    while pending:
        node = nodes[pending.pop()]
        pending.extend(node.get('children', []))
        light = node.get('extensions', {}).get(EXTENSION)
        if light is not None:
            lights.append(model['extensions'][EXTENSION]['lights'][light['light']]['name'])
        if 'camera' in node:
            cameras.append(model['cameras'][node['camera']]['name'])
    require(sorted(lights) == ['Imported lamp', 'Primary lamp'], f'Wrong exported light bindings: {lights}')
    require(sorted(cameras) == ['Imported camera', 'Primary camera'], f'Wrong exported camera bindings: {cameras}')


def check_imported_camera(state):
    graph = {node['graphNode']: node for node in state['gltfMerge']['authoringNodes']}
    authoring = state['gltfCameraAuthoring']
    active = {authoring['evaluated'][binding['evaluatedCamera']]['name']: binding for binding in authoring['cameras']}
    binding = active['Imported camera']
    definition = authoring['evaluated'][binding['evaluatedCamera']]
    require(definition['type'] == 'perspective' and definition['values'][0] == 0.9,
            f'Wrong imported camera projection: {definition}')
    position = graph[binding['graphNode']]['worldColumns'][3][:3]
    require(all(math.isclose(value, expected, abs_tol=1e-6)
                for value, expected in zip(position, translated(IMPORTED_CAMERA), strict=True)),
            f'Wrong imported camera position: {position}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--technique', choices=TECHNIQUES, action='append')
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    techniques = args.technique or TECHNIQUES
    report = dict(schemaVersion=1, passed=False, nativeValidationPerformed=False, generatedOnly=args.generate_only,
                  expectedCaptureCount=6 * len(techniques), captures=[], checks=[])
    try:
        source = output / 'sources'
        source.mkdir()
        base, control = source / 'primary.gltf', output / 'independent-control.gltf'
        primary(base)
        primary(control, merged=True)
        (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
        fixtures = []
        for technique in techniques:
            suffix = '.gltf' if technique == 'deferred-raster' else '.glb'
            rig = source / ('rig' + suffix)
            incoming(rig)
            sequence = output / (technique + '-sequence.json')
            sequence.write_text(json.dumps(dict(schemaVersion=1, frames=39, sampleFrames=[3, 13, 26, 39], events=[
                dict(frame=4, guiWaitForLoad=True, merge=dict(path=str(rig), parentName='Rig placement',
                                                             placementMatrix=placement(PLACEMENT))),
                dict(frame=14, sceneHistory='undo'), dict(frame=27, sceneHistory='redo')])), encoding='utf-8')
            fixtures.append((technique, suffix, sequence))
        if args.generate_only:
            report['passed'] = True
            return

        require(args.manifest is not None, '--manifest is required for native validation')
        report['manifest'] = verify_runtime_manifest(args.manifest, renderer)
        report['nativeValidationPerformed'] = True
        exports, portable = output / 'exports', output / 'portable'
        exports.mkdir()
        portable.mkdir()
        settings = ('--sky', 'plain', '--directional-intensity', '0', '--import-scale', '1', '--no-ray-query')

        def capture(path, label, technique, **options):
            return run(renderer, output, path, technique + '-' + label, technique, 'lit', report['captures'],
                       extra=settings, require_validation=True, use_authored_sun=True, **options)

        merged_captures = {}
        for technique, suffix, sequence in fixtures:
            directory = exports / technique
            directory.mkdir()
            saved = directory / ('rigged' + suffix)
            states = capture(base, 'live', technique, sequence=sequence, save=saved)
            require(len(states) == 4, f'Wrong sample count: {technique}')
            for index, (entry, count, publications, history) in enumerate(zip(
                    states, (1, 2, 1, 2), (0, 1, 2, 3), ((0, 0), (1, 0), (0, 1), (1, 0)))):
                state = entry['state']
                merge = state['gltfMerge']
                cursor = state.get('sceneEditHistory', dict(undo=0, redo=0)) if index == 0 else state['sceneEditHistory']
                require(state['scene']['primaryObjectCount'] == 1 and state['lighting']['pointLightCount'] == count and
                        len(state['gltfCameraAuthoring']['cameras']) == count and
                        merge['outcome'] == ('idle' if index == 0 else 'committed') and
                        merge['generationPublications'] == publications and merge['sourceReadCount'] == min(index, 1) and
                        (cursor['undo'], cursor['redo']) == history,
                        f'Wrong merge, camera, light, or history state: {entry["telemetry"]}')
                if count == 2:
                    check_imported_camera(state)
            before, merged, undone, redone = states
            oracle, = capture(control, 'control', technique)
            metrics = {name: pixels(first['image'], second['image']) for name, first, second in (
                ('lightChangesImage', before, merged), ('undo', before, undone), ('redo', merged, redone),
                ('independentControl', merged, oracle))}
            require(metrics['lightChangesImage']['changedPixelFraction'] > 0.05, f'Imported lamp has no visible effect: {technique}')
            require(all(metrics[name]['maximumError'] == 0 for name in ('undo', 'redo', 'independentControl')),
                    f'Merge/history differs from independent control: {technique} {metrics}')
            check_portable(saved)
            shutil.copytree(directory, portable / technique)
            merged_captures[technique] = merged
            report['checks'].append(dict(technique=technique, metrics=metrics, exportedBindings=True))

        source.rename(output / 'offline-sources')
        exports.rename(output / 'offline-exports')
        for technique, suffix, _ in fixtures:
            reopened, = capture(portable / technique / ('rigged' + suffix), 'reopened', technique)
            match = pixels(merged_captures[technique]['image'], reopened['image'])
            require(match['maximumError'] == 0 and reopened['state']['lighting']['pointLightCount'] == 2 and
                    len(reopened['state']['gltfCameraAuthoring']['cameras']) == 2, f'Portable reload changed: {technique} {match}')
            check_imported_camera(reopened['state'])
            report['checks'].append(dict(technique=technique, portableReload=match))
        require(len(report['captures']) == report['expectedCaptureCount'], 'Unexpected native capture count')
        verify_runtime_manifest(args.manifest, renderer)
        report['passed'] = True
    finally:
        report['captureCount'] = len(report['captures'])
        report['checkCount'] = len(report['checks'])
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
