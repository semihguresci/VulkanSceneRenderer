"""Round-trip edited scene history through portable GLB and glTF exports."""
import argparse
import base64
import json
import struct
from pathlib import Path

from PIL import Image
import numpy as np

from light_editor_history_runtime import action, edit, exact, fixture, history, point, run, write_json
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
        {'name': 'Original parent', 'children': [0]},
    ])
    source['nodes'][0]['name'] = 'Moved mesh'
    source['scenes'] = [
        {'name': 'Editable scene', 'nodes': [5, 1, 2, 4]},
        {'name': 'Second scene', 'nodes': [3, 1]},
    ]
    source['scene'] = 0
    write_json(path, source)
    return path


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
        dict(frame=5, addPrimitive=dict(kind='cube', name='Cube')),
        action(6, 'undo'), action(7, 'redo'),
        edit(8, dict(intensity=40)),
        dict(frame=9, viewportPointer=dict(action='select', pixel=[16, 16])),
    ]
    tests = []
    for technique, extension in (('deferred-raster', 'glb'), ('forward-raster', 'gltf')):
        destination = saved / ('edited.' + extension)
        original = run(renderer, output, technique + '-edited', source, technique, [10], events,
                       ('--save-scene', str(destination), '--no-ray-query'), fix_camera=True)[10]
        history(original, 5, 0)
        model = document(destination)
        assert model['scene'] == 0
        assert [scene['name'] for scene in model['scenes']] == ['Editable scene', 'Second scene']
        nodes = model['nodes']
        assert any(item.get('name') == 'Cube' and 'mesh' in item for item in nodes)
        assert any(item.get('name') == 'Inactive mesh' for item in nodes)
        hidden = next(item for item in nodes if item.get('name') == 'Hidden mesh')
        assert hidden['extensions']['KHR_node_visibility']['visible'] is False
        parent = next(item for item in nodes if item.get('name') == 'New parent')
        child, = parent['children']
        assert nodes[child]['name'] == 'Moved mesh'
        assert all('bufferView' in image and not image.get('uri') for image in model['images'])
        if extension == 'gltf':
            dependency = Path(model['buffers'][0]['uri'])
            assert dependency.name == str(dependency) and (saved / dependency).is_file()
        else:
            assert all('uri' not in buffer for buffer in model['buffers'])
        tests.append((technique, destination, original))
    # Remove access to the source resource through its original path.
    texture = source.parent / 'pattern.png'
    texture.rename(texture.with_name('source-only.png'))
    for technique, destination, original in tests:
        clear_selection = [dict(frame=1, viewportPointer=dict(action='select', pixel=[16, 16]))]
        reloaded = run(renderer, output, technique + '-reloaded', destination, technique,
                       [10], clear_selection, ('--no-ray-query',), fix_camera=True)[10]
        assert point(reloaded)['intensity'] == 40
        exact(original, reloaded)
        print(f'PASS {technique} edited {destination.suffix} reload without source textures', flush=True)
    write_json(output / 'results.json', dict(passed=True, pixelExactRoundTrips=len(tests)))


if __name__ == '__main__':
    main()
