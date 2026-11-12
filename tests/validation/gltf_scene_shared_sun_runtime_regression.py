"""Round-trip a scene-local manual sun and an unvisited, scaled glTF light."""

import argparse
import copy
import hashlib
from pathlib import Path
from shutil import copy2

import numpy as np

from gltf_accessor_runtime_regression import Asset, TECHNIQUES, load_asset, mesh, require
from gltf_camera_projection_runtime_regression import local_matrix
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_material_variants_runtime_regression import pixels, run
from light_editor_history_runtime import edit, write_json


PUNCTUAL = 'KHR_lights_punctual'
SCENES = ('Edited sun', 'Untouched sun and point')
SUN = dict(name='Shared authored sun', type='directional', color=[1, .25, .125], intensity=.75)
MANUAL = dict(direction=[0, 0, -1], color=[.125, .5, 1], intensity=1.25)
POINT = dict(name='Inactive point', type='point', color=[.125, 1, .25], intensity=18, range=.875)
POINT_POSITION = [0, 0, .75]
SCALE = 2
CAMERA = [dict(frame=1, position=[0, 0, 6], target=[0, 0, 0])]
CLEAR_SELECTION = dict(action='select', pixel=[4, 4])
FORMATS = ('gltf', 'glb')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixture(output):
    output.mkdir()
    asset = Asset()
    mesh(asset, asset.accessor('VEC3', [[-1.125, -.75, 0], [1.125, -.75, 0],
                                       [1.125, .75, 0], [-1.125, .75, 0]]),
         asset.accessor('VEC3', [[0, 0, 1]] * 4),
         asset.accessor('VEC2', [[0, 0], [1, 0], [1, 1], [0, 1]]))
    asset.model.update(
        materials=[dict(pbrMetallicRoughness=dict(baseColorFactor=[.5, .5, .5, 1],
                                                  metallicFactor=0, roughnessFactor=.875))],
        nodes=[dict(name='Receiver', mesh=0),
               dict(name='Shared sun owner', extensions={PUNCTUAL: dict(light=0)}),
               dict(name='Inactive point owner', translation=POINT_POSITION,
                    extensions={PUNCTUAL: dict(light=1)})],
        scenes=[dict(name=name, nodes=nodes) for name, nodes in zip(SCENES, ([0, 1], [0, 1, 2]))],
        extensionsUsed=[PUNCTUAL], extensions={PUNCTUAL: dict(lights=[SUN, POINT])},
        buffers=[dict(uri='receiver.bin', byteLength=len(asset.data))],
    )
    (output / 'receiver.bin').write_bytes(asset.data)
    source = output / 'source.gltf'
    write_json(source, asset.model)
    wrong_range = copy.deepcopy(asset.model)
    # At scale 2, this control's unscaled cutoff is shorter than the point's
    # world-space distance from the plane, so it cannot light the receiver.
    wrong_range['extensions'][PUNCTUAL]['lights'][1]['range'] /= SCALE
    control = output / 'unscaled-cutoff.gltf'
    write_json(control, wrong_range)
    return source, control


def close(actual, expected):
    np.testing.assert_allclose(actual, expected, rtol=0, atol=1e-6)


def check_lighting(record, selected, *, manual=False, reloaded=False, point_range=None):
    state = record['state']
    scene, lighting = state['gltfScenes'], state['lighting']
    require(scene['names'] == list(SCENES) and scene['selected'] == selected and
            state['scene']['primaryObjectCount'] == 1, 'Wrong live scene or missing receiver')
    require(state['gui']['selectedMeshNodeId'] == 2**32 - 1, 'A selection outline would alter the image comparison')
    edits = state['lightEdits']
    require(not edits['truncated'] and not edits['error'] and all(not row['selected'] for row in edits['lights']),
            'Light publication or clearing the viewport selection failed')
    sun, = (row for row in edits['lights'] if row['type'] == 0)
    require(sun['source'] == (2 if manual and not reloaded else 1), 'Wrong active directional source')
    expected_sun = MANUAL if manual else SUN
    for key in ('color', 'intensity'):
        close(sun[key], expected_sun[key])
        close(lighting['directional' + key.title()], expected_sun[key])
    close(sun['direction'], [0, 0, -1])
    close(lighting['directionalDirection'], [0, 0, -1])
    points = state['gltfAnimation']['pointLights']
    count = int(point_range is not None)
    require(scene['pointLightCount'] == lighting['pointLightCount'] == len(points) == count,
            'Inactive point light leaked into another scene or was lost after reload')
    if points:
        point, = points
        close(point['range'], point_range)
        close(point['position'], np.array(POINT_POSITION) * SCALE)
        close(point['color'], POINT['color'])
        close(point['intensity'], POINT['intensity'])


def visible_lights(model, scene):
    nodes = model['nodes']
    definitions = model['extensions'][PUNCTUAL]['lights']
    bindings, visited = [], set()
    work = [(index, np.eye(4)) for index in reversed(scene['nodes'])]
    while work:
        index, parent = work.pop()
        require(0 <= index < len(nodes) and index not in visited, 'Malformed portable scene reachability')
        visited.add(index)
        node = nodes[index]
        extensions = node.get('extensions', {})
        if not extensions.get('KHR_node_visibility', {}).get('visible', True):
            continue
        world = parent @ local_matrix(node)
        if PUNCTUAL in extensions:
            light = extensions[PUNCTUAL]['light']
            require(0 <= light < len(definitions), 'Invalid portable light binding')
            bindings.append((index, light, definitions[light], world))
        work.extend((child, world) for child in reversed(node.get('children', [])))
    return bindings


def check_export(path):
    model, buffers = load_asset(path)
    require(model['scene'] == 0 and [scene['name'] for scene in model['scenes']] == list(SCENES),
            'Portable output changed the scene catalog or default selection')
    require(len(buffers) == 1 and len(buffers[0]) >= model['buffers'][0]['byteLength'],
            'Portable output is missing packed geometry')
    active, inactive = (visible_lights(model, scene) for scene in model['scenes'])
    require([light[2]['type'] for light in active] == ['directional', 'directional'] and
            [light[2]['type'] for light in inactive] == ['directional', 'point'],
            'The exported scenes lost their ordered directional or inactive point bindings')
    manual, shared = active
    require(manual[0] == model['scenes'][0]['nodes'][0] and manual[0] != shared[0] and shared[:2] == inactive[0][:2],
            'The manual root is not scene-local or the original imported binding is no longer shared')
    require(shared[2]['name'] == SUN['name'], 'The shared authored light lost its source name')
    for binding, expected in ((manual, MANUAL), (shared, SUN)):
        close(binding[2]['color'], expected['color'])
        close(binding[2]['intensity'], expected['intensity'])
        forward = -binding[3][:3, 2]
        close(forward / np.linalg.norm(forward), [0, 0, -1])
    point = inactive[1]
    require(point[2]['name'] == POINT['name'], 'The inactive point lost its source name')
    close(point[2]['range'], POINT['range'] * SCALE)
    close(point[2]['color'], POINT['color'])
    close(point[2]['intensity'], POINT['intensity'])
    close(point[3][:3, 3], np.array(POINT_POSITION) * SCALE)
    return dict(manualNode=manual[0], sharedSunNode=shared[0], inactivePointNode=point[0],
                inactivePointRange=point[2]['range'])


def portable_copy(source, output):
    output.mkdir(parents=True)
    model, _ = load_asset(source)
    paths = [source]
    for buffer in model['buffers']:
        uri = buffer.get('uri')
        if source.suffix == '.gltf':
            require(uri and Path(uri).name == uri and uri.startswith('container-'),
                    'ASCII export does not use an adjacent portable dependency')
            paths.append(source.parent / uri)
        else:
            require(not uri, 'GLB export still depends on an external resource')
    for path in paths:
        copy2(path, output / path.name)
        require(sha(output / path.name) == sha(path), 'Portable copy differs from the written output')
    return output / source.name


def sequence(path, frames, events):
    write_json(path, dict(schemaVersion=1, frames=frames[-1], sampleFrames=frames, camera=CAMERA, events=events))
    return path


def changed(before, after):
    delta = pixels(before['image'], after['image'])
    require(delta['changedPixelFraction'] >= .002 and delta['meanAbsoluteError'] >= .1,
            f'The authored lighting difference did not affect the viewport: {delta}')
    return delta


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source, wrong_range = fixture(output / 'original')
    if args.generate_only:
        return
    require(args.renderer and args.manifest, 'Native verification requires --renderer and --manifest')
    renderer = args.renderer.resolve(strict=True)
    manifest = verify_runtime_manifest(args.manifest, renderer)
    (output / 'vk_layer_settings.txt').write_bytes(b'khronos_validation.validate_sync = true\n')
    inputs = {path.name: sha(path) for path in source.parent.iterdir()}
    untouched = sequence(output / 'untouched.json', [3], [dict(frame=1, viewportPointer=CLEAR_SELECTION)])
    edited = sequence(output / 'edited.json', [3, 8], [
        dict(frame=1, viewportPointer=CLEAR_SELECTION), dict(frame=4, lightAdd=dict(type='directional')),
        edit(5, MANUAL, target=dict(type='directional', source='manual', index=0)),
        dict(frame=6, viewportPointer=CLEAR_SELECTION)])
    reload = sequence(output / 'reload.json', [3, 8], [dict(frame=1, viewportPointer=CLEAR_SELECTION),
                                                      dict(frame=5, gltfScene=1)])
    report = dict(schemaVersion=1, passed=False, rendererSha256=sha(renderer), captures=[], checks=[])

    def capture(path, name, technique, script, *, scale=1, save=None, scene=None):
        return run(renderer, output, path, name, technique, 'lit', report['captures'], sequence=script, save=save,
                   gltf_scene=scene, extra=('--sky', 'plain', '--no-ray-query', '--import-scale', str(scale)),
                   require_validation=True, use_authored_sun=True)

    techniques = args.technique or TECHNIQUES
    try:
        portable, untouched_views = {}, {}
        for technique in techniques:
            original, = capture(source, f'{technique}-inactive', technique, untouched, scale=SCALE, scene=1)
            clipped, = capture(wrong_range, f'{technique}-unscaled-cutoff', technique, untouched, scale=SCALE, scene=1)
            check_lighting(original, 1, point_range=POINT['range'] * SCALE)
            check_lighting(clipped, 1, point_range=POINT['range'])
            untouched_views[technique] = original
            report['checks'].append(dict(technique=technique, check='inactive-point-cutoff', **changed(clipped, original)))
            for suffix in FORMATS:
                label = f'{technique}-{suffix}'
                destination = output / 'exports' / label / f'scene.{suffix}'
                destination.parent.mkdir(parents=True)
                before, after = capture(source, label + '-live', technique, edited, scale=SCALE, save=destination)
                check_lighting(before, 0)
                check_lighting(after, 0, manual=True)
                report['checks'].append(dict(technique=technique, format=suffix, check='active-manual-sun',
                                             **changed(before, after)))
                relocated = portable_copy(destination, output / 'portable' / label)
                report['checks'].append(dict(technique=technique, format=suffix, check='serialized-light-bindings',
                                             **check_export(relocated)))
                portable[technique, suffix] = (relocated, after)
        offline = output / 'offline-original'
        source.parent.rename(offline)
        require(not source.parent.exists() and all(sha(offline / name) == digest for name, digest in inputs.items()),
                'The original geometry dependency remains available or was modified')
        for (technique, suffix), (path, expected_edited) in portable.items():
            edited_view, inactive_view = capture(path, f'{technique}-{suffix}-reload', technique, reload)
            check_lighting(edited_view, 0, manual=True, reloaded=True)
            check_lighting(inactive_view, 1, reloaded=True, point_range=POINT['range'] * SCALE)
            for name, actual, expected in (('active', edited_view, expected_edited),
                                           ('inactive', inactive_view, untouched_views[technique])):
                error = pixels(actual['image'], expected['image'])
                require(error['maximumError'] == 0, f'Portable {suffix} changed the {name} scene pixels: {error}')
                report['checks'].append(dict(technique=technique, format=suffix, check=f'reloaded-{name}', **error))
        require(len(report['captures']) == 10 * len(techniques) and len(report['checks']) == 9 * len(techniques),
                'Incomplete native scene-light evidence')
        require(verify_runtime_manifest(args.manifest, renderer) == manifest and sha(renderer) == report['rendererSha256'],
                'The frozen runtime inputs changed during the regression')
        report.update(passed=True, nativeValidationPerformed=True)
    finally:
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        write_json(output / 'results.json', report)
    print('PASS shared sun isolation, scaled inactive light, and source-free glTF/GLB reloads', flush=True)


if __name__ == '__main__':
    main()
