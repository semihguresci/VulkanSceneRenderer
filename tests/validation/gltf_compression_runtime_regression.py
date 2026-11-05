"""Independent native #93 compression fixtures and plain export/reload checks.

Five scenes cover EXT/KHR meshopt ASCII external+embedded encoded buffers and
GLB BIN storage with genuine URI-less destination placeholders, ATTRIBUTES,
TRIANGLES, INDICES, combined animation/morph/skin, and sequential lossless Draco
raw NORMAL/normalized UV/RGB/RGBA plus untouched TANGENT and a lit normal map.
An encoder-only C++ helper compresses independently authored arrays. Reference
and negative-control assets are generated directly; no production decoder or
animation evaluator supplies their expected values. --generate-only never
launches the renderer. Full --roundtrip runs produce 50 captures.
"""
from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess

import numpy as np

from gltf_accessor_runtime_regression import (Asset, NativeRun, TECHNIQUES, compare,
    decode_accessor, f32, load_asset, mesh, normalized, require, textured_material)

CASES = ('ext-ascii', 'ext-glb-deform', 'khr-ascii-indices', 'khr-glb', 'draco-material')
COMPRESSION = {'EXT_meshopt_compression', 'KHR_meshopt_compression', 'KHR_draco_mesh_compression'}
POSITION = np.asarray(f32([[-.85, -.7, 0], [.7, -.7, 0], [.85, .8, 0], [-.65, .65, 0]]))
NORMAL = np.asarray(f32([[-.3, 0, .9539392], [.2, .15, .9682458], [.25, 0, .9682458], [0, -.3, .9539392]]))
# Independent Gram-Schmidt construction makes the untouched tangent a valid
# authored unit frame. Export may normalize it again within FLOAT precision.
_unit_normal = NORMAL / np.linalg.norm(NORMAL, axis=1, keepdims=True)
_tangent = np.tile([1., 0., 0.], (4, 1)) - _unit_normal * _unit_normal[:, :1]
DRACO_TANGENT = np.asarray(f32(np.column_stack((_tangent / np.linalg.norm(_tangent, axis=1, keepdims=True), np.ones(4)))))
UV = np.asarray([[0, 255], [255, 255], [255, 0], [0, 0]], dtype=np.uint8)
RGBA = np.asarray([[255, 155, 110, 0], [190, 255, 125, 255], [155, 190, 255, 255], [255, 255, 175, 0]], dtype=np.uint8)
INDICES = np.asarray([0, 1, 2, 0, 2, 3], dtype='<u2')
DELTA = np.asarray(f32([[0, 0, 0], [.3, .06, 0], [.35, .1, 0], [0, .05, 0]]))
SHIFT = np.asarray(f32([.15, .1, 0]))


def uri(data):
    return 'data:application/octet-stream;base64,' + base64.b64encode(data).decode()


def write_glb(path, model, payload):
    model = copy.deepcopy(model)
    text = json.dumps(model, separators=(',', ':')).encode('utf-8')
    text += b' ' * (-len(text) % 4)
    payload += bytes(-len(payload) % 4)
    data = struct.pack('<5I', 0x46546c67, 2, 28 + len(text) + len(payload), len(text), 0x4e4f534a)
    data += text + struct.pack('<2I', len(payload), 0x004e4942) + payload
    path.write_bytes(data)


def raw_rows(asset, accessor_index):
    accessor = asset.model['accessors'][accessor_index]
    view = asset.model['bufferViews'][accessor['bufferView']]
    count = accessor['count']
    width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[accessor['type']]
    size = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}[accessor['componentType']]
    element, stride = width * size, view.get('byteStride', width * size)
    offset = view.get('byteOffset', 0) + accessor.get('byteOffset', 0)
    return b''.join(asset.data[offset + row * stride:offset + row * stride + element] for row in range(count))


def base_asset(case, kind):
    animated = case == 'ext-glb-deform'
    draco = case == 'draco-material'
    asset = Asset()
    p = POSITION.copy()
    if animated and kind != 'source':
        p = np.asarray(f32(p + DELTA * np.float32(.8) + SHIFT))
    if kind == 'dropped' and not draco:
        p = np.asarray(f32(p * [.45, .6, 1]))
    normal = NORMAL.copy() if draco else np.asarray([[0., 0., 1.]] * 4)
    uv = normalized(UV, 5121)
    rgba = normalized(RGBA, 5121)
    if kind == 'dropped' and draco:
        uv, rgba, normal = [[0., 0.]] * 4, [[1., 1., 1., 1.]] * 4, [[0., 0., 1.]] * 4
    pos = asset.accessor('VEC3', f32(p))
    norm = asset.accessor('VEC3', f32(normal))
    tex = asset.accessor('VEC2', UV if draco and kind == 'source' else f32(uv),
                         5121 if draco and kind == 'source' else 5126,
                         **(dict(normalize=True, stride=4) if draco and kind == 'source' else {}))
    tangent = asset.accessor('VEC4', DRACO_TANGENT if draco else [[1., 0., 0., 1.]] * 4)
    color = asset.accessor('VEC4', RGBA if kind == 'source' else f32(rgba),
                           5121 if kind == 'source' else 5126, normalize=kind == 'source') if draco else None
    primitive = mesh(asset, pos, norm, tex, tangent=tangent, colors=color)
    textured_material(asset, lit=draco)
    if draco:
        asset.model['materials'][0].update(alphaMode='MASK', alphaCutoff=.35, doubleSided=True)
        # A second independent compressed primitive exercises 3-byte RGB values
        # while sharing the source POSITION/NORMAL/UV/TANGENT metadata.
        second = copy.deepcopy(primitive)
        second['attributes']['COLOR_0'] = asset.accessor('VEC3', RGBA[:, :3] if kind == 'source' else f32(np.asarray(rgba)[:, :3]),
            5121 if kind == 'source' else 5126, **(dict(normalize=True, stride=4) if kind == 'source' else {}))
        second['material'] = 1
        asset.model['materials'].append(copy.deepcopy(asset.model['materials'][0]))
        asset.model['materials'][1]['alphaMode'] = 'OPAQUE'
        asset.model['meshes'].append(dict(primitives=[second]))
        asset.model['nodes'] = [dict(name='Draco RGBA mask', mesh=0, scale=[.55, .75, .75], translation=[-.52, 0, 0]),
                                dict(name='Draco RGB opaque', mesh=1, scale=[.55, .75, .75], translation=[.52, 0, 0])]
        asset.model['scenes'][0]['nodes'] = [0, 1]
    if animated and kind == 'source':
        primitive['targets'] = [dict(POSITION=asset.accessor('VEC3', f32(DELTA)))]
        primitive['attributes']['JOINTS_0'] = asset.accessor('VEC4', [[0, 0, 0, 0]] * 4, 5121)
        primitive['attributes']['WEIGHTS_0'] = asset.accessor('VEC4', [[65535, 0, 0, 0]] * 4, 5123, normalize=True)
        asset.model['meshes'][0]['weights'] = [0.]
        asset.model['nodes'] = [dict(name='Compressed deformed owner', mesh=0, skin=0, translation=[2, 0, 0]),
                                dict(name='Moving joint')]
        asset.model['scenes'][0]['nodes'] = [0, 1]
        asset.model['skins'] = [dict(joints=[1], inverseBindMatrices=asset.accessor('MAT4', [np.eye(4).T.ravel().tolist()]))]
        times = asset.accessor('SCALAR', [0., 1.])
        weights = asset.accessor('SCALAR', [0., .8])
        translations = asset.accessor('VEC3', [[0., 0., 0.], SHIFT.tolist()])
        asset.model['animations'] = [dict(name='Compressed morph then skin', samplers=[
            dict(input=times, output=weights, interpolation='LINEAR'),
            dict(input=times, output=translations, interpolation='LINEAR')], channels=[
            dict(sampler=0, target=dict(node=0, path='weights')),
            dict(sampler=1, target=dict(node=1, path='translation'))])]
    return asset


def run_encoder(generator, directory, jobs):
    manifest = directory/'encoder-jobs.json'
    manifest.write_text(json.dumps(jobs, indent=2), encoding='utf-8')
    result = subprocess.run([str(generator), str(manifest)], cwd=directory, capture_output=True, text=True,
                            timeout=120, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    (directory/'encoder.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    require(result.returncode == 0, f'Official fixture encoder failed: {directory / "encoder.log"}')
    require('no decoder called' in result.stdout, 'Unexpected fixture generator executable')


def compressed_meshopt(asset, directory, case, generator):
    extension = 'EXT_meshopt_compression' if case.startswith('ext-') else 'KHR_meshopt_compression'
    binary = 'glb' in case
    model = copy.deepcopy(asset.model)
    original = bytes(asset.data)
    model['buffers'] = [dict(byteLength=len(original), uri=uri(original))]
    jobs, records = [], []
    # Compress every original view, including deformation/animation/IBM arrays.
    # Each view has an independent buffer, preventing accidental fallback reads.
    for index, view in enumerate(model['bufferViews']):
        index_accessors = [a for a in model['accessors'] if a.get('bufferView') == index]
        accessor = index_accessors[0]
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[accessor['type']]
        size = {5121: 1, 5123: 2, 5126: 4}[accessor['componentType']]
        is_index = view.get('target') == 34963
        mode = ('INDICES' if 'indices' in case else 'TRIANGLES') if is_index else 'ATTRIBUTES'
        stride = size * width if is_index else max(4, view.get('byteStride', size * width))
        count = accessor['count']
        payload = raw_rows(asset, model['accessors'].index(accessor))
        if stride > size * width:
            payload = b''.join(payload[row * size * width:(row + 1) * size * width] + bytes(stride - size * width)
                               for row in range(count))
        # Meshopt ATTRIBUTES can store animation SCALAR values, joints, IBM,
        # morphs and all ordinary arrays without changing their consumer role.
        raw, encoded = directory/f'view-{index}.raw', directory/f'view-{index}.encoded'
        raw.write_bytes(payload)
        jobs.append(dict(codec='meshopt', input=str(raw), output=str(encoded), count=count, stride=stride,
                         mode=mode, version=0 if case.startswith('ext-') else 1))
        records.append((index, encoded, payload, mode, stride, count, size * width))
    run_encoder(generator, directory, jobs)
    # GLB uses buffer0 for the encoded BIN payload; ASCII alternates real
    # external .bin and data URI encoded sources in a single scene.
    encoded_bin = bytearray()
    model['buffers'] = [dict(byteLength=0)] if binary else []
    for index, encoded, payload, mode, stride, count, element in records:
        data = encoded.read_bytes()
        if binary:
            while len(encoded_bin) % 4:
                encoded_bin.append(0)
            source_buffer, source_offset = 0, len(encoded_bin)
            encoded_bin.extend(data)
        else:
            source_buffer, source_offset = len(model['buffers']), 0
            location = uri(data)
            if index % 2 == 0:
                external = directory/f'view-{index}.bin'
                external.write_bytes(data)
                location = external.name
            model['buffers'].append(dict(byteLength=len(data), uri=location))
        destination = len(model['buffers'])
        model['buffers'].append(dict(byteLength=len(payload), extensions={extension: dict(fallback=True)}))
        view = model['bufferViews'][index]
        view.update(buffer=destination, byteOffset=0, byteLength=len(payload),
                    extensions={extension: dict(buffer=source_buffer, byteOffset=source_offset,
                        byteLength=len(data), byteStride=stride, count=count, mode=mode, filter='NONE')})
        if mode == 'ATTRIBUTES' and (view.get('byteStride') or stride != element):
            view['byteStride'] = stride
    model.setdefault('extensionsUsed', []).append(extension)
    model.setdefault('extensionsRequired', []).append(extension)
    path = directory/('source.glb' if binary else 'source.gltf')
    if binary:
        model['buffers'][0]['byteLength'] = len(encoded_bin)
        write_glb(path, model, bytes(encoded_bin))
    else:
        path.write_text(json.dumps(model, indent=2), encoding='utf-8')
    return path


def compressed_draco(asset, directory, generator):
    model = copy.deepcopy(asset.model)
    jobs, records = [], []
    indices = directory/'faces.raw'
    indices.write_bytes(np.asarray(INDICES, dtype='<u4').tobytes())
    for mesh_index, entry in enumerate(model['meshes']):
        primitive = entry['primitives'][0]
        attrs = []
        mapping = {}
        for slot, semantic in enumerate(('POSITION', 'NORMAL', 'TEXCOORD_0', 'COLOR_0')):
            accessor = model['accessors'][primitive['attributes'][semantic]]
            raw = directory/f'draco-{mesh_index}-{semantic}.raw'
            raw.write_bytes(raw_rows(asset, primitive['attributes'][semantic]))
            unique = 7 + slot * 11
            mapping[semantic] = unique
            attrs.append(dict(semantic=semantic, input=str(raw), id=unique, component=accessor['componentType'],
                components=int(accessor['type'][-1]), normalized=accessor.get('normalized', False)))
        encoded = directory/f'draco-{mesh_index}.encoded'
        jobs.append(dict(codec='draco', count=4, faces=2, indices=str(indices), attributes=attrs, output=str(encoded)))
        records.append((primitive, mapping, encoded))
    run_encoder(generator, directory, jobs)
    model['buffers'] = [dict(byteLength=len(asset.data), uri=uri(asset.data))]
    for primitive, mapping, encoded in records:
        data = encoded.read_bytes()
        source = len(model['buffers'])
        model['buffers'].append(dict(byteLength=len(data), uri=uri(data)))
        view = len(model['bufferViews'])
        model['bufferViews'].append(dict(buffer=source, byteLength=len(data)))
        primitive['extensions'] = dict(KHR_draco_mesh_compression=dict(bufferView=view, attributes=mapping))
        for semantic in mapping:
            # Retain shared metadata while making compressed attributes truly
            # bufferless. TANGENT and its source view remain ordinary.
            accessor = model['accessors'][primitive['attributes'][semantic]]
            accessor.pop('bufferView', None)
            accessor.pop('byteOffset', None)
        model['accessors'][primitive['indices']].pop('bufferView', None)
    model.setdefault('extensionsUsed', []).append('KHR_draco_mesh_compression')
    model.setdefault('extensionsRequired', []).append('KHR_draco_mesh_compression')
    path = directory/'source.gltf'
    path.write_text(json.dumps(model, indent=2), encoding='utf-8')
    return path


def fixture_cpu_checks(case, paths):
    source, buffers = load_asset(paths['source'])
    reference, _ = load_asset(paths['reference'])
    require(not any(name in COMPRESSION for name in reference.get('extensionsUsed', [])), 'Reference is compressed')
    for mesh_index, entry in enumerate(reference['meshes']):
        primitive = entry['primitives'][0]
        actual = decode_accessor(paths['reference'], primitive['attributes']['POSITION'])
        expected = POSITION + DELTA * np.float32(.8) + SHIFT if case == 'ext-glb-deform' else POSITION
        require(np.max(np.abs(actual - expected)) < 3e-7, 'Independent reference pose is wrong')
        require(np.array_equal(decode_accessor(paths['reference'], primitive['indices']).ravel(), INDICES), 'Reference topology differs')
        if case == 'draco-material':
            for semantic, values in [('NORMAL', NORMAL), ('TEXCOORD_0', normalized(UV, 5121)),
                                      ('COLOR_0', normalized(RGBA[:, :3] if mesh_index else RGBA, 5121))]:
                require(np.max(np.abs(decode_accessor(paths['reference'], primitive['attributes'][semantic]) - values)) < 1e-7,
                        f'Independent {semantic} normalized oracle differs')
    if case != 'draco-material':
        locations, modes = set(), set()
        for view in source['bufferViews']:
            extension = next(value for name, value in view['extensions'].items() if name in COMPRESSION)
            destination = source['buffers'][view['buffer']]
            require('uri' not in destination, 'Destination placeholder accidentally has a fallback URI')
            require(destination['byteLength'] == view['byteLength'], 'Placeholder declared destination size differs')
            encoded = buffers[extension['buffer']]
            require(encoded is not None and extension.get('byteOffset', 0) + extension['byteLength'] <= len(encoded),
                    'Encoded source range is invalid')
            location = source['buffers'][extension['buffer']].get('uri', '')
            locations.add('embedded' if location.startswith('data:') else 'external' if location else 'BIN')
            modes.add(extension['mode'])
        require(locations == ({'BIN'} if 'glb' in case else {'embedded', 'external'}), 'Required storage mix is absent')
        require({'ATTRIBUTES', 'INDICES' if 'indices' in case else 'TRIANGLES'} <= modes, 'Required codec modes are absent')
    else:
        for entry in source['meshes']:
            primitive = entry['primitives'][0]
            mapping = primitive['extensions']['KHR_draco_mesh_compression']['attributes']
            require(set(mapping) == {'POSITION', 'NORMAL', 'TEXCOORD_0', 'COLOR_0'}, 'Draco attribute map differs')
            require('bufferView' in source['accessors'][primitive['attributes']['TANGENT']], 'Ordinary tangent missing')
            tangent = decode_accessor(paths['source'], primitive['attributes']['TANGENT'])
            require(np.max(np.abs(tangent - DRACO_TANGENT)) < 1e-7 and
                    np.max(np.abs(np.sum(tangent[:, :3] * NORMAL, axis=1))) < 1e-7,
                    'Independent ordinary tangent is not the authored perpendicular unit frame')
            require(all('bufferView' not in source['accessors'][primitive['attributes'][name]] for name in mapping),
                    'Draco fixture unexpectedly has decoded fallback arrays')
    validate_encoder_inputs(case, paths, source)
    return dict(storage='GLB BIN' if 'glb' in case else 'ASCII embedded+external' if case != 'draco-material' else 'ASCII Draco',
                independentReference=True, untouchedTangent=case == 'draco-material', animated=case == 'ext-glb-deform')


def validate_encoder_inputs(case, paths, source):
    """Decode original raw encoder inputs using only Python struct/NumPy."""
    directory = paths['source'].parent
    jobs = json.loads((directory/'encoder-jobs.json').read_text(encoding='utf-8'))
    if case == 'draco-material':
        types = {5121: 'u1', 5126: '<f4'}
        for mesh_index, job in enumerate(jobs):
            require(job['codec'] == 'draco' and job['count'] == 4 and job['faces'] == 2, 'Wrong Draco encoder input shape')
            require(np.array_equal(np.frombuffer(Path(job['indices']).read_bytes(), dtype='<u4'), INDICES),
                    'Draco raw input topology differs')
            for attribute in job['attributes']:
                actual = np.frombuffer(Path(attribute['input']).read_bytes(), dtype=types[attribute['component']]).reshape(4, -1)
                expected = {'POSITION': POSITION, 'NORMAL': NORMAL, 'TEXCOORD_0': UV,
                            'COLOR_0': RGBA[:, :3] if mesh_index else RGBA}[attribute['semantic']]
                require(np.array_equal(actual, expected), 'Draco raw encoder input differs from independent constants')
                require(attribute['normalized'] == (attribute['semantic'] in ('TEXCOORD_0', 'COLOR_0')),
                        'Draco raw input normalization flag differs')
        return
    # Reconstruct an ordinary raw scene from the *input* of each encoder job.
    # No encoded bytes are decoded for this fixture/oracle sanity check.
    plain = copy.deepcopy(source)
    plain['buffers'] = []
    require(len(jobs) == len(plain['bufferViews']), 'Meshopt raw stream count differs')
    for index, (view, job) in enumerate(zip(plain['bufferViews'], jobs)):
        data = Path(job['input']).read_bytes()
        require(job['codec'] == 'meshopt' and len(data) == job['count'] * job['stride'] == view['byteLength'],
                'Meshopt raw encoder input byte count differs')
        plain['buffers'].append(dict(byteLength=len(data), uri=uri(data)))
        view.update(buffer=index, byteOffset=0)
        view.pop('extensions', None)
    plain['extensionsUsed'] = [name for name in plain.get('extensionsUsed', []) if name not in COMPRESSION]
    plain['extensionsRequired'] = [name for name in plain.get('extensionsRequired', []) if name not in COMPRESSION]
    path = directory/'encoder-input-plain.gltf'
    path.write_text(json.dumps(plain, indent=2), encoding='utf-8')
    primitive = plain['meshes'][0]['primitives'][0]
    for semantic, expected in [('POSITION', POSITION), ('NORMAL', [[0., 0., 1.]] * 4),
                              ('TEXCOORD_0', normalized(UV, 5121)), ('TANGENT', [[1., 0., 0., 1.]] * 4)]:
        require(np.max(np.abs(decode_accessor(path, primitive['attributes'][semantic]) - expected)) < 1e-7,
                f'Meshopt raw {semantic} input differs from independent constants')
    require(np.array_equal(decode_accessor(path, primitive['indices']).ravel(), INDICES), 'Meshopt raw input topology differs')
    if case == 'ext-glb-deform':
        require(np.max(np.abs(decode_accessor(path, primitive['targets'][0]['POSITION']) - DELTA)) < 1e-7, 'Raw morph input differs')
        require(np.array_equal(decode_accessor(path, primitive['attributes']['JOINTS_0']), np.zeros((4, 4))), 'Raw joint input differs')
        require(np.array_equal(decode_accessor(path, primitive['attributes']['WEIGHTS_0']), np.tile([1, 0, 0, 0], (4, 1))),
                'Raw normalized weight input differs')
        require(np.array_equal(decode_accessor(path, plain['skins'][0]['inverseBindMatrices']), np.eye(4).T.reshape(1, 16)),
                'Raw inverse-bind matrix differs')
        samplers = plain['animations'][0]['samplers']
        for sampler in samplers:
            require(np.array_equal(decode_accessor(path, sampler['input']).ravel(), [0, 1]), 'Raw animation times differ')
        require(np.max(np.abs(decode_accessor(path, samplers[0]['output']).ravel() - [0, np.float32(.8)])) < 1e-7,
                'Raw animation morph weights differ')
        require(np.max(np.abs(decode_accessor(path, samplers[1]['output']) - [[0, 0, 0], SHIFT])) < 1e-7,
                'Raw animation joint translations differ')


def reject_compression_declarations(value):
    if isinstance(value, dict):
        for key, child in value.items():
            require(key not in COMPRESSION, 'Saved scene retained a compression extension object')
            if key in ('extensionsUsed', 'extensionsRequired'):
                require(not (set(child) & COMPRESSION), 'Saved scene retained compression declarations')
            reject_compression_declarations(child)
    elif isinstance(value, list):
        for child in value:
            reject_compression_declarations(child)


def verify_roundtrip(path, reference_path, case):
    model, buffers = load_asset(path)
    reference, _ = load_asset(reference_path)
    reject_compression_declarations(model)
    require(all(buffer is not None for buffer in buffers), 'Saved plain scene has unresolved buffers')
    # Compare decoded exported values to independent raw constants, including
    # narrow normalized data. For deformation, export preserves the base pose.
    for mesh_index, entry in enumerate(model['meshes'][:len(reference['meshes'])]):
        primitive = entry['primitives'][0]
        expected = reference['meshes'][mesh_index]['primitives'][0]
        for semantic in ('POSITION', 'NORMAL', 'TANGENT', 'TEXCOORD_0', 'COLOR_0'):
            if semantic not in expected['attributes']:
                continue
            left = decode_accessor(path, primitive['attributes'][semantic])
            right = decode_accessor(reference_path, expected['attributes'][semantic])
            if case == 'ext-glb-deform' and semantic == 'POSITION':
                right = POSITION
            if semantic == 'COLOR_0' and left.shape[1] == 4 and right.shape[1] == 3:
                # Ordinary RGB colors have implicit alpha=1. The renderer's
                # plain export consistently materializes COLOR_0 as RGBA.
                right = np.column_stack((right, np.ones(len(right))))
            require(left.shape == right.shape and np.max(np.abs(left-right)) < 3e-7,
                    f'Plain export lost independently expected {semantic} values')
    if case == 'ext-glb-deform':
        primitive = model['meshes'][0]['primitives'][0]
        require(np.max(np.abs(decode_accessor(path, primitive['targets'][0]['POSITION']) - DELTA)) < 1e-7,
                'Plain export changed uncompressed morph deltas')
        require(len(model.get('skins', [])) == 1 and len(model.get('animations', [])) == 1,
                'Plain export lost skin/animation consumers')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--generator', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--roundtrip', action='store_true')
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    generator = args.generator.resolve()
    require(generator.is_file(), f'Missing encoder-only fixture generator: {generator}')
    generator_hash = hashlib.sha256(generator.read_bytes()).hexdigest()
    run = NativeRun(args.renderer, args.output, args.timeout)
    run.report.update(mode='generate-only' if args.generate_only else 'native', generatorSha256=generator_hash)
    try:
        for case in args.case or CASES:
            directory = run.output/case
            directory.mkdir(parents=True, exist_ok=True)
            paths = {kind: directory/(kind+'.gltf') for kind in ('reference', 'dropped')}
            for kind, path in paths.items():
                base_asset(case, kind).write(path)
            source = base_asset(case, 'source')
            paths['source'] = (compressed_draco(source, directory, generator) if case == 'draco-material' else
                               compressed_meshopt(source, directory, case, generator))
            info = fixture_cpu_checks(case, paths)
            fixture_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in directory.iterdir() if path.is_file()}
            run.report['fixtures'].append(dict(case=case, paths={key: str(path) for key, path in paths.items()},
                                              sourceSha256=fixture_hashes, **info))
            run.report['checks'].append(dict(case=case, check='independent-cpu-fixture-oracle', passed=True))
            if args.generate_only:
                continue
            for technique in args.technique or TECHNIQUES:
                stem = technique+'-'+case
                lit, animated = case == 'draco-material', case == 'ext-glb-deform'
                destination = directory/(stem+'-saved.glb') if args.roundtrip else None
                actual = run.capture(paths['source'], stem+'-source', technique, animated=animated, lit=lit, destination=destination)
                reference = run.capture(paths['reference'], stem+'-reference', technique, lit=lit)
                dropped = run.capture(paths['dropped'], stem+'-dropped', technique, lit=lit)
                run.report['checks'].append(dict(case=case, technique=technique, check='independent-plain-reference', **compare(actual, reference)))
                run.report['checks'].append(dict(case=case, technique=technique, check='deliberately-dropped-control', **compare(actual, dropped, exact=False)))
                if destination:
                    verify_roundtrip(destination, paths['reference'], case)
                    text_destination = directory/(stem+'-saved.gltf')
                    reloaded = run.capture(destination, stem+'-glb-reload', technique, animated=animated, lit=lit, destination=text_destination)
                    verify_roundtrip(text_destination, paths['reference'], case)
                    text_reloaded = run.capture(text_destination, stem+'-gltf-reload', technique, animated=animated, lit=lit)
                    run.report['checks'].append(dict(case=case, technique=technique, check='plain-glb-reload', **compare(actual, reloaded)))
                    run.report['checks'].append(dict(case=case, technique=technique, check='plain-gltf-reload', **compare(actual, text_reloaded)))
                for path, expected_hash in fixture_hashes.items():
                    require(hashlib.sha256(Path(path).read_bytes()).hexdigest() == expected_hash, 'Loading/export mutated source fixture')
                run.save()
                print('PASS', case, technique, flush=True)
        require(hashlib.sha256(generator.read_bytes()).hexdigest() == generator_hash, 'Fixture generator changed during regression')
        run.finish()
    except Exception as error:
        run.report['failure'] = str(error)
        raise
    finally:
        run.save()
    print(f"PASS: {len(run.report['checks'])} checks, {len(run.report['captures'])} captures; {run.output}")


if __name__ == '__main__':
    main()
