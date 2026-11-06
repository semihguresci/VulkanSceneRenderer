"""Round-trip edited scene history through portable GLB and glTF exports."""
import argparse
import base64
import json
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
        {'name': 'Editable scene', 'nodes': [0, 1, 2, 4]},
        {'name': 'Second scene', 'nodes': [3, 1, 5]},
        {'name': 'Empty scene', 'nodes': []},
    ]
    source['nodes'].append({'name': 'Inactive spot', 'translation': [0, 0, 2],
                            'extensions': {PUNCTUAL: {'light': 1}}})
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
    assert original[1]['scene']['gltfScene'] == dict(index=1, count=3, name='Second scene')
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
        scaledInactiveRoundTrips=2, emptyScenePreserved=True, invalidSelectionPreservesDestination=True))


if __name__ == '__main__':
    main()
