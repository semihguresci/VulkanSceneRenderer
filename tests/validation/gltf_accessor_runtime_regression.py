"""Generated native sparse/quantized/DOUBLE accessor checks against FLOAT assets.

The reference arrays and morph-before-skin arithmetic are independent of the
renderer. No production evaluator or downloaded asset is used. --generate-only
does not launch the renderer; native execution requires an explicitly supplied
binary. Requires NumPy, Pillow and a Vulkan display/runtime for native runs.
"""
from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import zlib

import numpy as np
from PIL import Image

COMPONENT = {5120: ('b', 1), 5121: ('B', 1), 5122: ('h', 2), 5123: ('H', 2),
             5125: ('I', 4), 5126: ('f', 4), 5130: ('d', 8)}
WIDTH = dict(SCALAR=1, VEC2=2, VEC3=3, VEC4=4, MAT4=16)
TECHNIQUES = ('deferred-raster', 'forward-raster')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def f32(values):
    return np.asarray(values, dtype=np.float32).astype(float).tolist()


def normalized(values, component):
    values = np.asarray(values, dtype=np.float64)
    if component in (5120, 5122):
        result = np.maximum(values / (127 if component == 5120 else 32767), -1)
    else:
        result = values / (255 if component == 5121 else 65535)
    return f32(result)


def png_bytes(values):
    values = np.asarray(values, dtype=np.uint8)
    require(values.ndim == 3 and values.shape[2] == 4, 'Expected an RGBA PNG array')
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + row.tobytes() for row in values)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', values.shape[1], values.shape[0], 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


class Asset:
    def __init__(self):
        self.data = bytearray()
        self.model = dict(asset=dict(version='2.0', generator='Container independent accessor regression'),
                          scene=0, scenes=[dict(nodes=[0])], nodes=[dict(name='Accessor probe', mesh=0)],
                          meshes=[], materials=[], bufferViews=[], accessors=[])

    def view(self, data, *, stride=0, target=None):
        while len(self.data) % 8:
            self.data.append(0)
        view = dict(buffer=0, byteOffset=len(self.data), byteLength=len(data))
        if stride:
            view['byteStride'] = stride
        if target:
            view['target'] = target
        self.data.extend(data)
        self.model['bufferViews'].append(view)
        return len(self.model['bufferViews']) - 1

    def accessor(self, kind, values, component=5126, *, normalize=False,
                 stride=0, prefix=0, sparse=None, sparse_only=False, indices=False):
        values = np.asarray(values).reshape((-1, WIDTH[kind])).tolist()
        code, size = COMPONENT[component]
        packed = lambda row: struct.pack('<' + code * len(row), *row)
        result = dict(componentType=component, count=len(values), type=kind)
        if normalize:
            result['normalized'] = True
        if not sparse_only:
            payload = bytearray(prefix)
            for row in values:
                payload.extend(packed(row))
                payload.extend(bytes(max(stride - size * WIDTH[kind], 0)))
            result['bufferView'] = self.view(payload, stride=stride, target=34963 if indices else None)
            if prefix:
                result['byteOffset'] = prefix
        effective = np.zeros_like(values) if sparse_only else np.asarray(values).copy()
        if sparse:
            replacements = sorted(sparse.items())
            sparse_indices = b'\0' * 4 + struct.pack('<' + 'H' * len(replacements), *(index for index, _ in replacements))
            sparse_values = b'\0' * 8 + b''.join(packed(row) for _, row in replacements)
            result['sparse'] = dict(count=len(replacements),
                indices=dict(bufferView=self.view(sparse_indices), byteOffset=4, componentType=5123),
                values=dict(bufferView=self.view(sparse_values), byteOffset=8))
            for index, row in replacements:
                effective[index] = row
        if component == 5126:
            effective = np.asarray(effective, dtype=np.float32)
        if kind == 'VEC3':
            result.update(min=np.min(effective, axis=0).tolist(), max=np.max(effective, axis=0).tolist())
        elif kind == 'SCALAR' and component in (5126, 5130):
            result.update(min=[float(np.min(effective))], max=[float(np.max(effective))])
        self.model['accessors'].append(result)
        return len(self.model['accessors']) - 1

    def image(self, pixels):
        images = self.model.setdefault('images', [])
        images.append(dict(uri='data:image/png;base64,' + base64.b64encode(png_bytes(pixels)).decode()))
        self.model.setdefault('samplers', []).append(dict(minFilter=9729, magFilter=9729, wrapS=33071, wrapT=33071))
        self.model.setdefault('textures', []).append(dict(source=len(images)-1, sampler=len(images)-1))
        return len(images)-1

    def write(self, path):
        self.model['buffers'] = [dict(byteLength=len(self.data), uri='data:application/octet-stream;base64,' + base64.b64encode(self.data).decode())]
        path.write_text(json.dumps(self.model, indent=2), encoding='utf-8')


def textured_material(asset, *, lit=False):
    texture = np.zeros((8, 8, 4), dtype=np.uint8)
    texture[:, :, :] = [35, 180, 235, 255]
    texture[:4, :3] = [245, 45, 25, 255]
    texture[4:, 3:] = [230, 210, 30, 255]
    material = dict(pbrMetallicRoughness=dict(baseColorFactor=[1, 1, 1, 1], baseColorTexture=dict(index=asset.image(texture)),
                                            metallicFactor=0, roughnessFactor=1))
    if lit:
        normal = np.tile([210, 165, 220, 255], (2, 2, 1)).astype(np.uint8)
        material['normalTexture'] = dict(index=asset.image(normal))
    else:
        material['extensions'] = dict(KHR_materials_unlit={})
        asset.model.setdefault('extensionsUsed', []).append('KHR_materials_unlit')
    asset.model['materials'] = [material]


def mesh(asset, positions, normals, uv, *, tangent=None, colors=None, indices=None, extras=None):
    attrs = dict(POSITION=positions, NORMAL=normals, TEXCOORD_0=uv)
    if tangent is not None:
        attrs['TANGENT'] = tangent
    if colors is not None:
        attrs['COLOR_0'] = colors
    primitive = dict(attributes=attrs, indices=indices if indices is not None else asset.accessor('SCALAR', [0, 1, 2, 0, 2, 3], 5123, indices=True), material=0)
    if extras:
        primitive.update(extras)
    asset.model['meshes'] = [dict(primitives=[primitive])]
    return primitive


def write_accessor_case(path, case, *, reference=False, dropped=False):
    asset = Asset()
    p = np.array([[-.85, -.7, 0], [.7, -.7, 0], [.85, .8, 0], [-.65, .65, 0]], dtype=float)
    n = [[0., 0., 1.]] * 4
    uv = [[0., 1.], [1., 1.], [1., 0.], [0., 0.]]
    tangent = [[1., 0., 0., 1.]] * 4
    animated = case == 'sparse-deform'
    position_options, normal_options, uv_options, tangent_options = {}, {}, {}, {}
    component = 5126
    if case in ('sparse-dense', 'sparse-deform'):
        dense = p.copy()
        dense[1] = [.05, -.25, 0]
        dense[2] = [.15, .3, 0]
        if dropped:
            p = dense
        elif not reference:
            position_options = dict(stride=16, prefix=4, sparse={1: p[1].tolist(), 2: p[2].tolist()})
            p = dense
    elif case == 'sparse-only':
        if not reference and not dropped:
            position_options = dict(sparse_only=True, sparse={i: row.tolist() for i, row in enumerate(p)})
            normal_options = dict(sparse_only=True, sparse={i: row for i, row in enumerate(n)})
            uv_options = dict(sparse_only=True, sparse={i: row for i, row in enumerate(uv) if any(row)})
        elif dropped:
            p *= [.45, .5, 1]
    elif case == 'quantized':
        encoded = [[-32768, -22937, 0], [22937, -22937, 0], [27852, 26214, 0], [-21299, 21299, 0]]
        encoded_n = [[64, 0, 110]] * 4
        encoded_t = [[110, 0, -64, -128]] * 4
        encoded_uv = [[0, 65535], [65535, 65535], [65535, 0], [0, 0]]
        p, n, tangent, uv = (normalized(encoded, 5122), normalized(encoded_n, 5120),
                              normalized(encoded_t, 5120), normalized(encoded_uv, 5123))
        if dropped:
            p = (np.asarray(p) * [.45, .7, 1]).tolist()
        elif not reference:
            p, n, tangent, uv = encoded, encoded_n, encoded_t, encoded_uv
            component = 5122
            position_options = dict(normalize=True, stride=8)
            normal_options = dict(normalize=True, stride=4)
            uv_options = dict(normalize=True)
            tangent_options = dict(normalize=True)
            asset.model['extensionsRequired'] = ['KHR_mesh_quantization']
            asset.model['extensionsUsed'] = ['KHR_mesh_quantization']
    elif case == 'double':
        p += [[.000000001, 0, 0]] * 4
        uv = [[.05, .95], [.9, .95], [.9, .1], [.05, .1]]
        if dropped:
            p *= [.4, .6, 1]
        elif not reference:
            component = 5130
            position_options = dict(prefix=8)
            asset.model['extensionsRequired'] = ['KHR_accessor_float64']
            asset.model['extensionsUsed'] = ['KHR_accessor_float64']
    else:
        require(case in ('sparse-deform', 'sparse-dense', 'sparse-only'), f'Unknown accessor fixture {case}')
    if reference and animated:
        delta = np.array([[0, 0, 0], [.35, .1, 0], [.35, .1, 0], [0, 0, 0]])
        p = np.asarray(p) + delta * .75 + [.2, .1, 0]
    if component == 5126:
        p, n, uv, tangent = map(f32, (p, n, uv, tangent))
    pos = asset.accessor('VEC3', p, component, **position_options)
    norm = asset.accessor('VEC3', n, 5120 if case == 'quantized' and not (reference or dropped) else component, **normal_options)
    tex = asset.accessor('VEC2', uv, 5123 if case == 'quantized' and not (reference or dropped) else component, **uv_options)
    tan = asset.accessor('VEC4', tangent, 5120 if case == 'quantized' and not (reference or dropped) else component, **tangent_options)
    index = None
    if case == 'sparse-dense' and not (reference or dropped):
        index = asset.accessor('SCALAR', [0, 1, 2, 0, 0, 3], 5123, indices=True, sparse={4: [2]})
    primitive = mesh(asset, pos, norm, tex, tangent=tan, indices=index)
    textured_material(asset, lit=case == 'quantized')
    if case == 'quantized':
        # A second visible primitive exercises unnormalized signed coordinates
        # with authored node decoding scale, without adding native captures.
        raw = [[-4, -3, 0], [4, -3, 0], [4, 3, 0], [-4, 3, 0]]
        raw_position = asset.accessor('VEC3', raw, 5126 if reference or dropped else 5122,
                                      **({} if reference or dropped else dict(stride=8)))
        second = copy.deepcopy(primitive)
        second['attributes']['POSITION'] = raw_position
        asset.model['meshes'].append(dict(primitives=[second]))
        asset.model['nodes'] = [dict(name='Normalized signed positions', mesh=0, scale=[.48, .48, .48], translation=[-.5, 0, 0]),
                                dict(name='Raw signed positions with authored scale', mesh=1, scale=[.125, .125, .125], translation=[.55, 0, 0])]
        asset.model['scenes'][0]['nodes'] = [0, 1]
    if animated and not reference:
        delta = [[0, 0, 0], [.35, .1, 0], [.35, .1, 0], [0, 0, 0]]
        target = asset.accessor('VEC3', delta, sparse_only=True, sparse={1: delta[1], 2: delta[2]})
        # A valid bufferless, nonsparse accessor denotes an all-zero array.
        # Zero normal deltas must retain the authored base frame through morphing.
        zero_normals = asset.accessor('VEC3', [[0, 0, 0]]*4, sparse_only=True)
        primitive['targets'] = [dict(POSITION=target, NORMAL=zero_normals)]
        primitive['attributes']['JOINTS_0'] = asset.accessor('VEC4', [[0, 0, 0, 0]]*4, 5121)
        primitive['attributes']['WEIGHTS_0'] = asset.accessor('VEC4', [[65535, 0, 0, 0]]*4, 5123, normalize=True)
        asset.model['meshes'][0]['weights'] = [0.]
        asset.model['nodes'] = [dict(name='Sparse deformed owner', mesh=0, skin=0, translation=[2, 0, 0]),
                                dict(name='Moving joint')]
        asset.model['scenes'][0]['nodes'] = [0, 1]
        ibm = asset.accessor('MAT4', [np.eye(4).T.ravel().tolist()], 5130)
        asset.model['extensionsRequired'] = ['KHR_accessor_float64']
        asset.model.setdefault('extensionsUsed', []).append('KHR_accessor_float64')
        asset.model['skins'] = [dict(joints=[1], inverseBindMatrices=ibm)]
        times = asset.accessor('SCALAR', [0., 1.])
        weight = asset.accessor('SCALAR', [0., .75])
        translation = asset.accessor('VEC3', [[0., 0., 0.], [.2, .1, 0.]])
        asset.model['animations'] = [dict(name='Sparse morph then skin', samplers=[
            dict(input=times, output=weight, interpolation='LINEAR'), dict(input=times, output=translation, interpolation='LINEAR')],
            channels=[dict(sampler=0, target=dict(node=0, path='weights')), dict(sampler=1, target=dict(node=1, path='translation'))])]
    asset.write(path)
    return dict(case=case, animated=animated and not reference, firstPosition=f32(np.asarray(p).reshape((-1, 3)))[0])


def load_asset(path):
    data = Path(path).read_bytes()
    binary = None
    if data[:4] == b'glTF':
        length, kind = struct.unpack_from('<2I', data, 12)
        require(kind == 0x4e4f534a, 'GLB JSON chunk missing')
        model = json.loads(data[20:20+length])
        offset = 20 + length
        if offset + 8 <= len(data):
            length, kind = struct.unpack_from('<2I', data, offset)
            require(kind == 0x004e4942, 'GLB BIN chunk missing')
            binary = data[offset+8:offset+8+length]
    else:
        model = json.loads(data)
    buffers = []
    for buffer in model.get('buffers', []):
        uri = buffer.get('uri', '')
        buffers.append(base64.b64decode(uri.split(',', 1)[1]) if uri.startswith('data:') else
                       (Path(path).parent/uri).read_bytes() if uri else binary)
    return model, buffers


def source_accessor_signature(path):
    model, buffers = load_asset(path)
    result = []
    for accessor in model['accessors']:
        entry = copy.deepcopy(accessor)
        def view_bytes(index):
            view = model['bufferViews'][index]
            offset = view.get('byteOffset', 0)
            return dict(stride=view.get('byteStride', 0), data=buffers[view.get('buffer', 0)][offset:offset+view['byteLength']].hex())
        if 'bufferView' in entry:
            entry['bufferView'] = view_bytes(entry['bufferView'])
        if 'sparse' in entry:
            for name in ('indices', 'values'):
                entry['sparse'][name]['bufferView'] = view_bytes(entry['sparse'][name]['bufferView'])
        result.append(entry)
    return result


def decode_accessor(path, index):
    """Small independent fixture decoder; never calls renderer code."""
    model, buffers = load_asset(path)
    accessor = model['accessors'][index]
    code, size = COMPONENT[accessor['componentType']]
    count, width = accessor['count'], WIDTH[accessor['type']]
    result = np.zeros((count, width), dtype=float)
    if 'bufferView' in accessor:
        view = model['bufferViews'][accessor['bufferView']]
        data = buffers[view.get('buffer', 0)]
        offset = view.get('byteOffset', 0) + accessor.get('byteOffset', 0)
        stride = view.get('byteStride', width*size)
        for row in range(count):
            result[row] = struct.unpack_from('<'+code*width, data, offset+row*stride)
    if 'sparse' in accessor:
        sparse = accessor['sparse']
        iview = model['bufferViews'][sparse['indices']['bufferView']]
        vview = model['bufferViews'][sparse['values']['bufferView']]
        icode, isize = COMPONENT[sparse['indices']['componentType']]
        ibase = iview.get('byteOffset', 0)+sparse['indices'].get('byteOffset', 0)
        vbase = vview.get('byteOffset', 0)+sparse['values'].get('byteOffset', 0)
        for row in range(sparse['count']):
            destination = struct.unpack_from('<'+icode, buffers[iview.get('buffer', 0)], ibase+row*isize)[0]
            result[destination] = struct.unpack_from('<'+code*width, buffers[vview.get('buffer', 0)], vbase+row*width*size)
    if accessor.get('normalized', False):
        return np.asarray(normalized(result, accessor['componentType']), dtype=np.float32)
    return result.astype(np.float32)


def validate_accessor_fixture(files, case):
    source, _ = load_asset(files['source'])
    reference, _ = load_asset(files['float'])
    left = source['meshes'][0]['primitives'][0]
    right = reference['meshes'][0]['primitives'][0]
    for semantic in ('POSITION', 'NORMAL', 'TANGENT', 'TEXCOORD_0'):
        actual = decode_accessor(files['source'], left['attributes'][semantic])
        expected = decode_accessor(files['float'], right['attributes'][semantic])
        if case == 'sparse-deform' and semantic == 'POSITION':
            delta = decode_accessor(files['source'], left['targets'][0]['POSITION'])
            actual = (actual + delta*np.float32(.75) + np.asarray([.2, .1, 0], dtype=np.float32)).astype(np.float32)
        require(np.max(np.abs(actual-expected)) < 3e-7, f'Invalid independent {case}/{semantic} fixture oracle')
    require(np.array_equal(decode_accessor(files['source'], left['indices']), decode_accessor(files['float'], right['indices'])),
            'Sparse index reference differs from its replacements')
    if case == 'quantized':
        left = source['meshes'][1]['primitives'][0]
        right = reference['meshes'][1]['primitives'][0]
        require(np.array_equal(decode_accessor(files['source'], left['attributes']['POSITION']),
                               decode_accessor(files['float'], right['attributes']['POSITION'])),
                'Raw signed quantized positions were not preserved by the FLOAT reference')
        require(source['nodes'] == reference['nodes'], 'Authored quantized node transform differs in the reference')


def finite_json(value):
    if isinstance(value, float):
        return np.isfinite(value)
    if isinstance(value, dict):
        return all(finite_json(child) for child in value.values())
    if isinstance(value, list):
        return all(finite_json(child) for child in value)
    return True


class NativeRun:
    def __init__(self, renderer, output, timeout=180):
        self.renderer, self.output, self.timeout = Path(renderer).resolve(), Path(output).resolve(), timeout
        require(self.renderer.is_file(), f'Missing renderer {self.renderer}')
        self.output.mkdir(parents=True, exist_ok=True)
        (self.output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
        self.binary_hash = hashlib.sha256(self.renderer.read_bytes()).hexdigest()
        self.report = dict(schemaVersion=1, passed=False, executableSha256=self.binary_hash, fixtures=[], captures=[], checks=[])
        self.save()

    def save(self):
        (self.output/'results.json').write_text(json.dumps(self.report, indent=2), encoding='utf-8')

    def capture(self, source, name, technique, *, animated=False, destination=None, extra=(), lit=False):
        image = self.output/(name+'.png')
        telemetry = image.with_suffix('.telemetry.json')
        image.unlink(missing_ok=True)
        telemetry.unlink(missing_ok=True)
        command = [str(self.renderer), '--model', str(source), '--hidden', '--no-ui', '--validation',
            '--width', '640', '--height', '360', '--msaa', '1', '--no-taa', '--no-bloom', '--environment-intensity', '0',
            '--directional-intensity', '3' if lit else '0', '--directional-direction', '-.6', '.3', '-1', '--exposure', '1',
            '--camera-position', '0', '0', '3', '--camera-target', '0', '0', '0', '--camera-fov', '60',
            '--warmup-frames', '3', '--capture-frame', '4', '--fixed-dt', '.016666667', '--display-mode', 'lit',
            '--render-technique', technique, '--screenshot', str(image)]
        if animated:
            command += ['--animation-clip', '0', '--animation-time', '1', '--animation-paused']
        if destination:
            destination.unlink(missing_ok=True)
            command += ['--save-scene', str(destination)]
        command += list(extra)
        log = image.with_suffix('.log')
        with log.open('w', encoding='utf-8') as stream:
            process = subprocess.run(command, cwd=self.renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                timeout=self.timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(self.output)),
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        text = log.read_text(encoding='utf-8', errors='replace')
        require(process.returncode == 0, f'Native process failed ({process.returncode}): {log}')
        require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error', text, re.I), f'Vulkan validation failed: {log}')
        require(image.is_file() and telemetry.is_file(), f'Missing fresh native capture {image}')
        state = json.loads(telemetry.read_text(encoding='utf-8'))
        require(finite_json(state) and state['technique'] == technique, f'Wrong/nonfinite telemetry {telemetry}')
        require(state['scene']['primaryObjectCount'] >= 1, 'No geometry was rendered')
        if animated:
            animation = state['gltfAnimation']
            require(animation['selectedClip'] == 0 and abs(animation['time']-1) < 1e-6 and not animation['playing'] and
                    animation['diagnostics'] == 0, 'Expected paused deformation clip did not execute')
            require(state['gltfDeformation']['active'] and state['gltfDeformation']['gpuReady'], 'Expected deformation is inactive')
        record = dict(image=str(image), telemetry=str(telemetry), log=str(log), command=command)
        self.report['captures'].append(record)
        self.save()
        return dict(record, state=state)

    def finish(self):
        require(hashlib.sha256(self.renderer.read_bytes()).hexdigest() == self.binary_hash, 'Renderer changed during native regression')
        self.report['passed'] = True
        self.save()


def pixels(record):
    values = np.asarray(Image.open(record['image']).convert('RGB'), dtype=float)
    require(values.shape == (360, 640, 3), 'Wrong native capture extent')
    return values


def compare(first, second, *, exact=True, tolerance=.03):
    a, b = pixels(first), pixels(second)
    require(a.std() > 3 and b.std() > 3, 'Image comparison is vacuous')
    delta = np.abs(a-b)
    result = dict(meanAbsolutePixelError=float(delta.mean()), changedPixelFraction=float(np.mean(np.max(delta, axis=2) >= 8)))
    require(result['meanAbsolutePixelError'] <= tolerance if exact else
            result['meanAbsolutePixelError'] > .25 and result['changedPixelFraction'] > .003,
            f'Unexpected image equivalence: {result}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--case', action='append', choices=('sparse-dense', 'sparse-only', 'quantized', 'double', 'sparse-deform'))
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--roundtrip', action='store_true')
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    run = NativeRun(args.renderer, args.output, args.timeout)
    run.report['mode'] = 'generate-only' if args.generate_only else 'native'
    try:
        for case in args.case or ('sparse-dense', 'sparse-only', 'quantized', 'double', 'sparse-deform'):
            files = {kind: run.output/f'{case}-{kind}.gltf' for kind in ('source', 'float', 'dropped')}
            info = write_accessor_case(files['source'], case)
            write_accessor_case(files['float'], case, reference=True)
            write_accessor_case(files['dropped'], case, reference=True, dropped=True)
            validate_accessor_fixture(files, case)
            run.report['fixtures'].append(dict(case=case, paths={key: str(path) for key, path in files.items()}))
            if args.generate_only:
                continue
            for technique in args.technique or TECHNIQUES:
                stem = technique+'-'+case
                destination = run.output/(stem+'-saved.glb') if args.roundtrip else None
                actual = run.capture(files['source'], stem+'-source', technique, animated=info['animated'], destination=destination, lit=case=='quantized')
                reference = run.capture(files['float'], stem+'-float', technique, lit=case=='quantized')
                dropped = run.capture(files['dropped'], stem+'-dropped', technique, lit=case=='quantized')
                run.report['checks'].append(dict(case=case, technique=technique, check='float-reference', **compare(actual, reference)))
                run.report['checks'].append(dict(case=case, technique=technique, check='negative-control', **compare(actual, dropped, exact=False)))
                if destination:
                    require(source_accessor_signature(destination)[:len(source_accessor_signature(files['source']))] == source_accessor_signature(files['source']),
                            'Export changed original accessor declaration or encoded source bytes')
                    reloaded = run.capture(destination, stem+'-reload', technique, animated=info['animated'], lit=case=='quantized')
                    run.report['checks'].append(dict(case=case, technique=technique, check='roundtrip-source-and-pixels', **compare(actual, reloaded)))
                run.save()
                print('PASS', case, technique, flush=True)
        run.finish()
    except Exception as error:
        run.report['failure'] = str(error)
        raise
    finally:
        run.save()
    print(f"PASS: {len(run.report['checks'])} checks, {len(run.report['captures'])} captures; {run.output}")


if __name__ == '__main__':
    main()
