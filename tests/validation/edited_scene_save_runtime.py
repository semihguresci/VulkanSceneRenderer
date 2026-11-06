"""Round-trip edited scene history through portable GLB and glTF exports."""
import argparse
import base64
import json
import math
import struct
import subprocess
from pathlib import Path

from PIL import Image
import numpy as np

from light_editor_history_runtime import POINTER, PUNCTUAL, action, edit, exact, fixture, history, lights, point, run, write_json
from scene_node_history_runtime import columns, node


def prepare_source(directory):
    directory.mkdir()
    path = directory / 'authoring.gltf'
    fixture(path)
    source = json.loads(path.read_text(encoding='utf-8'))
    texture = Image.new('RGBA', (2, 2))
    texture.putdata([(115, 185, 115, 255), (40, 115, 205, 255),
                     (205, 115, 50, 255), (230, 185, 115, 255)])
    texture.save(directory / 'pattern.png')
    source['images'] = [{'uri': 'pattern.png', 'name': 'External pattern'}]
    source['textures'] = [{'source': 0}]
    source['materials'][0]['pbrMetallicRoughness']['baseColorTexture'] = {'index': 0}
    texcoords = source['meshes'][0]['primitives'][0]['attributes']
    if 'TEXCOORD_0' not in texcoords:
        # Reuse the fixture's packed buffer, appending four FLOAT VEC2 values.
        buffer = source['buffers'][0]
        data = base64.b64decode(buffer['uri'].split(',', 1)[1])
        data += b'\x00' * (-len(data) % 4)
        view = len(source['bufferViews'])
        source['bufferViews'].append(dict(buffer=0, byteOffset=len(data), byteLength=32, target=34962))
        data += struct.pack('<8f', 0, 0, 1, 0, 1, 1, 0, 1)
        buffer.update(uri='data:application/octet-stream;base64,' + base64.b64encode(data).decode(),
                      byteLength=len(data))
        texcoords['TEXCOORD_0'] = len(source['accessors'])
        source['accessors'].append(dict(bufferView=view, componentType=5126, count=4, type='VEC2'))
    source['nodes'].extend([
        {'name': 'Hidden mesh', 'mesh': 0, 'translation': [-1.0, 0, .4]},
        {'name': 'Inactive mesh', 'mesh': 0, 'translation': [1.5, 0, .6]},
        {'name': 'New parent', 'translation': [0, -.1, 0]},
    ])
    source['scenes'] = [
        {'name': 'Editable scene', 'nodes': [0, 1, 2, 4, 6]},
        {'name': 'Second scene', 'nodes': [3, 1, 5, 7]},
        {'name': 'Empty scene', 'nodes': []},
    ]
    source['nodes'].append({'name': 'Inactive spot', 'translation': [0, 0, 2],
                            'extensions': {PUNCTUAL: {'light': 1}}})
    source['cameras'] = [dict(type='perspective', perspective=dict(yfov=math.pi / 4, znear=.1))]
    source['nodes'].extend(dict(name=name, camera=0, translation=[0, 0, 6])
                           for name in ('First scene camera', 'Second scene camera'))
    source['extensions'][PUNCTUAL]['lights'].append(dict(
        type='spot', intensity=15, range=3, spot=dict(innerConeAngle=.1, outerConeAngle=.45)))
    source['scene'] = 0
    write_json(path, source)
    return path


def animate_inactive_range(source):
    model = json.loads(source.read_text(encoding='utf-8'))
    data = struct.pack('<4f', 0, 1, 3, 6)
    buffer = len(model['buffers'])
    model['buffers'].append(dict(byteLength=len(data),
        uri='data:application/octet-stream;base64,' + base64.b64encode(data).decode()))
    view = len(model['bufferViews'])
    model['bufferViews'].append(dict(buffer=buffer, byteLength=len(data)))
    accessor = len(model['accessors'])
    model['accessors'].extend(dict(bufferView=view, byteOffset=offset, componentType=5126,
                                   count=2, type='SCALAR') for offset in (0, 8))
    model['accessors'][accessor].update(min=[0], max=[1])
    model['extensionsUsed'].append(POINTER)
    model['animations'] = [dict(samplers=[dict(input=accessor, output=accessor + 1)],
        channels=[dict(sampler=0, target=dict(path='pointer', extensions={POINTER: {
            'pointer': f'/extensions/{PUNCTUAL}/lights/1/range'}}))])]
    write_json(source, model)


def verify_other_scenes(renderer, output, source, saved):
    animate_inactive_range(source)
    selected = ('--gltf-scene', '1', '--no-ray-query')
    scale = ('--import-scale', '2')
    pose = ('--animation-clip', '0', '--animation-time', '1', '--animation-paused')
    technique = 'deferred-raster'
    edited = [edit(2, dict(intensity=40))]
    original = run(renderer, output, 'scaled-original-second', source, technique,
                   [2], edited, (*selected, *scale, *pose), fix_camera=True)[2]
    view = original[1]['scene']['gltfScene']
    assert (view['index'], view['count'], view['name']) == (1, 3, 'Second scene')
    assert point(original)['intensity'] == 40 and point(original)['range'] == 16
    assert sorted(light['range'] for light in lights(original) if light['source'] == 1) == [12, 16]
    for suffix in ('glb', 'gltf'):
        destination = saved / f'scaled.{suffix}'
        run(renderer, output, f'scaled-save-{suffix}', source, technique, [2], edited,
            (*scale, '--no-ray-query', '--save-scene', str(destination)), fix_camera=True)
        assert document(destination)['scene'] == 0
        reloaded = run(renderer, output, f'scaled-reload-{suffix}', destination, technique,
                       [2], (), (*selected, *pose), fix_camera=True)[2]
        assert point(reloaded)['intensity'] == 40 and point(reloaded)['range'] == 16
        assert sorted(light['range'] for light in lights(reloaded) if light['source'] == 1) == [12, 16]
        exact(original, reloaded)
    empty = saved / 'empty-default.glb'
    result = run(renderer, output, 'empty-scene', source, technique, [1], (),
                 ('--gltf-scene', '2', '--no-ray-query', '--save-scene', str(empty)), fix_camera=True)[1]
    assert result[1]['scene']['primaryObjectCount'] == 0
    assert result[1]['scene']['gltfScene']['index'] == 2
    assert not any(light['source'] == 1 for light in lights(result))
    model = document(empty)
    assert model['scene'] == 2 and not model['scenes'][2].get('nodes')
    assert len(model['scenes']) == 3
    original_bytes = empty.read_bytes()
    invalid = subprocess.run([str(renderer), '--model', str(source), '--gltf-scene', '3',
        '--hidden', '--no-ui', '--save-scene', str(empty)], cwd=renderer.parent,
        capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120)
    assert invalid.returncode != 0 and 'outside the asset' in invalid.stdout + invalid.stderr
    assert empty.read_bytes() == original_bytes
    print('PASS scaled inactive spot animation, shared light edits, and empty scene selection', flush=True)


def verify_live_switching(renderer, output, source, saved):
    events = [
        edit(2, dict(intensity=40)),
        node(3, 'visibility', 2, visible=False),
        dict(frame=4, gltfScene=1),
        dict(frame=5, lightAdd=dict(type='point')),
        dict(frame=6, addPrimitive='Cube'),
        node(7, 'transform', 3, matrix=columns(np.eye(4))),
        dict(frame=8, gltfScene=2),
        dict(frame=9, gltfScene=0),
        dict(frame=10, gltfScene=1),
        dict(frame=11, gltfScene=0),
    ]
    for technique in ('deferred-raster', 'forward-raster'):
        destination = saved / ('switch-' + technique + '.glb')
        states = run(renderer, output, 'switch-' + technique, source, technique,
                     [1, 3, 4, 5, 6, 7, 8, 9, 10, 11], events,
                     ('--no-ray-query', '--animation-clip', '0', '--save-scene', str(destination)),
                     fix_camera=True)
        views = [state['scene']['gltfScene'] for _, state in states.values()]
        assert len({view['graphIdentity'] for view in views}) == 1
        assert len({view['sourceRevision'] for view in views}) == 1
        assert [view['index'] for view in views] == [0, 0, 1, 1, 1, 1, 2, 0, 1, 0]
        assert sorted(states[4][1]['scene']['gltfScene']['activeSourceNodes']) == [1, 3, 5, 7]
        assert not states[8][1]['scene']['gltfScene']['activeSourceNodes']
        assert states[1][1]['gltfAnimation']['playing']
        assert not states[4][1]['gltfAnimation']['playing']
        assert states[4][1]['gltfAnimation']['time'] == states[10][1]['gltfAnimation']['time']
        history(states[4], 0, 0)
        history(states[10], 0, 0)
        assert point(states[4])['intensity'] == point(states[9])['intensity'] == point(states[10])['intensity'] == 40
        assert states[8][1]['scene']['primaryObjectCount'] == 0
        assert not any(light['source'] in (1, 2) for light in lights(states[8]))
        assert states[4][1]['scene']['primaryObjectCount'] == 1
        assert states[10][1]['scene']['primaryObjectCount'] == 2
        assert any(light['source'] == 2 for light in lights(states[10]))
        assert not any(light['source'] == 2 for light in lights(states[11]))
        exact(states[3], states[9])
        exact(states[7], states[10])
        model = document(destination)
        assert model['scene'] == 0
        assert len(model['scenes']) == 3
        reload = run(renderer, output, 'switch-reload-' + technique, destination, technique,
                     [2], (), ('--no-ray-query', '--gltf-scene', '1'), fix_camera=True)[2]
        assert reload[1]['scene']['primaryObjectCount'] == 2
        exact(states[10], reload)
        print(f'PASS {technique} live switching, scene-local edits, and inactive edit reload', flush=True)
    states = run(renderer, output, 'authored-camera-switch', source, 'deferred-raster',
                 [1, 2], [dict(frame=2, gltfScene=1)],
                 ('--no-ray-query', '--gltf-camera-node', '6'), fix_camera=True)
    assert states[1][1]['gltfAnimation']['selectedCameraNode'] == 6
    assert states[2][1]['gltfAnimation']['selectedCameraNode'] == -1
    assert 6 not in states[2][1]['scene']['gltfScene']['activeSourceNodes']
    print('PASS authored camera selection clears on switch', flush=True)


def verify_reference_switching(renderer, output, source):
    states = run(renderer, output, 'switch-reference', source, 'path-tracing',
                 [2, 4, 6, 8], [dict(frame=3, gltfScene=1), dict(frame=5, gltfScene=2),
                                dict(frame=7, gltfScene=0)],
                 ('--path-spp', '16', '--path-samples', '32', '--path-bounces', '3', '--path-seed', '7'),
                 fix_camera=True)
    for frame, selected in ((2, 0), (4, 1), (6, 2), (8, 0)):
        state = states[frame][1]
        assert state['scene']['gltfScene']['index'] == selected
        path = state['referencePath']
        assert path['attemptedSamples'] == 32 and path['unsupportedSamples'] == path['rejectedSamples'] == 0
    exact(states[2], states[8])
    print('PASS path-tracing switches and deterministic accumulation reset', flush=True)


def document(path):
    if path.suffix == '.gltf':
        return json.loads(path.read_text(encoding='utf-8'))
    data = path.read_bytes()
    magic, version, size, json_size, chunk = struct.unpack_from('<5I', data)
    assert (magic, version, size, chunk) == (0x46546c67, 2, len(data), 0x4e4f534a)
    return json.loads(data[20:20 + json_size])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reference-path', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True)
    (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    source = prepare_source(output / 'original')
    saved = output / 'portable scene'
    saved.mkdir()
    initial_matrix = np.eye(4)
    initial_matrix[0, 3] = .2
    events = [
        node(2, 'transform', 0, matrix=columns(initial_matrix)),
        node(3, 'visibility', 2, visible=False),
        node(4, 'reparent', 0, parent=4),
        dict(frame=5, addPrimitive='Cube'),
        action(6, 'undo'), action(7, 'redo'),
        edit(8, dict(intensity=40)),
    ]
    tests = []
    for technique, extension in (('deferred-raster', 'glb'), ('forward-raster', 'gltf')):
        destination = saved / ('edited.' + extension)
        original = run(renderer, output, technique + '-edited', source, technique, [8], events,
                       ('--save-scene', str(destination), '--no-ray-query'), fix_camera=True)[8]
        history(original, 5, 0)
        model = document(destination)
        assert model['scene'] == 0
        assert [scene['name'] for scene in model['scenes']] == ['Editable scene', 'Second scene', 'Empty scene']
        nodes = model['nodes']
        assert any(item.get('name') == 'Cube' and 'mesh' in item for item in nodes)
        assert any(item.get('name') == 'Inactive mesh' for item in nodes)
        hidden = next(item for item in nodes if item.get('name') == 'Hidden mesh')
        assert hidden['extensions']['KHR_node_visibility']['visible'] is False
        parent = next(item for item in nodes if item.get('name') == 'New parent')
        assert len(parent['children']) == 1
        assert all('bufferView' in image and not image.get('uri') for image in model['images'])
        if extension == 'gltf':
            dependency = Path(model['buffers'][0]['uri'])
            assert dependency.name == str(dependency) and (saved / dependency).is_file()
        else:
            assert all('uri' not in buffer for buffer in model['buffers'])
        tests.append((technique, destination, original))
    verify_other_scenes(renderer, output, source, saved)
    verify_live_switching(renderer, output, source, saved)
    if args.reference_path:
        verify_reference_switching(renderer, output, source)
    # Remove access to the source resource through its original path.
    texture = source.parent / 'pattern.png'
    texture.rename(texture.with_name('source-only.png'))
    for technique, destination, original in tests:
        reloaded = run(renderer, output, technique + '-reloaded', destination, technique,
                       [8], (), ('--no-ray-query',), fix_camera=True)[8]
        assert point(reloaded)['intensity'] == 40
        exact(original, reloaded)
        print(f'PASS {technique} edited {destination.suffix} reload without source textures', flush=True)
    write_json(output / 'results.json', dict(passed=True, pixelExactRoundTrips=len(tests),
        scaledInactiveRoundTrips=2, liveTechniques=2 + int(args.reference_path),
        emptyScenePreserved=True, invalidSelectionPreservesDestination=True))


if __name__ == '__main__':
    main()
