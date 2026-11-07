"""Native glTF filter, shared-image cache and edited-material image regressions.

Generates tiny embedded PNG fixtures, uses both raster paths, and checks
nearest/bilinear magnification plus base/mip minification without TAA or bloom.
Optional round trips verify saved sampler policy and matching reloaded pixels.
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

import gltf_animation_runtime_regression as animation

FILTERS = dict(nearest=9728, linear=9729, nearest_mipmap_nearest=9984,
               linear_mipmap_nearest=9985, nearest_mipmap_linear=9986, linear_mipmap_linear=9987)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def pattern_png(size, alpha_mode):
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            check = (x // (2 if size > 4 else 1) + y // (2 if size > 4 else 1)) % 2
            rows.extend((255 if check else 0, 0 if check else 255, 0,
                         0 if alpha_mode == 'MASK' and not check else 255))
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(rows))) + chunk(b'IEND', b''))


def write_fixture(path, min_filter, mag_filter, repeat=1, alpha_mode='OPAQUE', edited=False, shared=False):
    data, views, accessors = bytearray(), [], []
    def accessor(kind, values, component=5126):
        while len(data) % 4:
            data.append(0)
        offset = len(data)
        flat = [x for value in values for x in value] if isinstance(values[0], tuple) else values
        data.extend(struct.pack('<' + ('f' if component == 5126 else 'H') * len(flat), *flat))
        views.append(dict(buffer=0, byteOffset=offset, byteLength=len(data)-offset))
        result = dict(bufferView=len(views)-1, componentType=component, count=len(values), type=kind)
        if kind == 'VEC3':
            result.update(min=[min(value[i] for value in values) for i in range(3)],
                          max=[max(value[i] for value in values) for i in range(3)])
        elif kind == 'SCALAR' and component == 5126:
            result.update(min=[min(values)], max=[max(values)])
        accessors.append(result)
        return len(accessors)-1
    positions = accessor('VEC3', [(-1., -1., 0.), (1., -1., 0.), (1., 1., 0.), (-1., 1., 0.)])
    normals = accessor('VEC3', [(0., 0., 1.)] * 4)
    uv = accessor('VEC2', [(0., float(repeat)), (float(repeat), float(repeat)), (float(repeat), 0.), (0., 0.)])
    indices = accessor('SCALAR', [0, 1, 2, 0, 2, 3], 5123)
    material = dict(name='Authored filter material', extensions=dict(KHR_materials_unlit={}),
                    alphaMode=alpha_mode, alphaCutoff=.3,
                    pbrMetallicRoughness=dict(baseColorFactor=[1, 1, 1, .65 if alpha_mode == 'BLEND' else 1],
                                             baseColorTexture=dict(index=0), metallicFactor=0, roughnessFactor=1))
    model = dict(asset=dict(version='2.0', generator='Container sampler filter regression'),
                 extensionsUsed=['KHR_materials_unlit'], scene=0, scenes=[dict(nodes=[0])],
                 nodes=[dict(name='Filter probe', mesh=0)], materials=[material],
                 meshes=[dict(primitives=[dict(attributes=dict(POSITION=positions, NORMAL=normals, TEXCOORD_0=uv),
                                              indices=indices, material=0)])],
                 images=[dict(uri='data:image/png;base64,' + base64.b64encode(pattern_png(64 if repeat > 1 else 4, alpha_mode)).decode())],
                 samplers=[dict(minFilter=min_filter, magFilter=mag_filter,
                                wrapS=10497 if repeat > 1 else 33071, wrapT=10497 if repeat > 1 else 33071)],
                 textures=[dict(source=0, sampler=0)])
    if shared:
        # Same decoded image, two different sampler policies, visible together.
        model['nodes'] = [dict(name='Nearest shared-image probe', mesh=0, translation=[-.5, 0, 0], scale=[.5, 1, 1]),
                          dict(name='Linear shared-image probe', mesh=1, translation=[.5, 0, 0], scale=[.5, 1, 1])]
        model['scenes'][0]['nodes'] = [0, 1]
        model['meshes'].append(copy.deepcopy(model['meshes'][0]))
        model['meshes'][1]['primitives'][0]['material'] = 1
        model['materials'].append(copy.deepcopy(material))
        model['materials'][1]['pbrMetallicRoughness']['baseColorTexture']['index'] = 1
        model['samplers'].append(dict(model['samplers'][0], magFilter=9729))
        model['textures'].append(dict(source=0, sampler=1))
    if edited:
        times = accessor('SCALAR', [0., 1.])
        colors = accessor('VEC4', [(1., 1., 1., 1.), (.5, .5, .5, 1.)])
        model['extensionsUsed'].append('KHR_animation_pointer')
        model['animations'] = [dict(name='Factor edit retains sampler',
            samplers=[dict(input=times, output=colors, interpolation='LINEAR')],
            channels=[dict(sampler=0, target=dict(path='pointer', extensions=dict(KHR_animation_pointer=dict(
                pointer='/materials/0/pbrMetallicRoughness/baseColorFactor'))))])]
    model.update(bufferViews=views, accessors=accessors,
                 buffers=[dict(byteLength=len(data), uri='data:application/octet-stream;base64,' + base64.b64encode(data).decode())])
    path.write_text(json.dumps(model, indent=2), encoding='utf-8')


def read_model(path):
    data = path.read_bytes()
    if data[:4] == b'glTF':
        size, kind = struct.unpack_from('<2I', data, 12)
        require(kind == 0x4e4f534a, 'GLB has no leading JSON chunk')
        data = data[20:20+size]
    return json.loads(data)


def texture_sampler_policies(model):
    policies = []
    for texture in model.get('textures', []):
        index = texture.get('sampler', -1)
        sampler = model.get('samplers', [])[index] if 0 <= index < len(model.get('samplers', [])) else {}
        policies.append(tuple(sampler.get(name, default) for name, default in (
            ('minFilter', 9987), ('magFilter', 9729), ('wrapS', 10497), ('wrapT', 10497))))
    return policies


def pixels(record):
    with Image.open(record['image']) as image:
        values = np.asarray(image.convert('RGB'), dtype=float)
    require(values.shape == (360, 640, 3), 'Wrong sampler capture extent')
    # Pinhole camera z3/fov60 projects the quad to x216..424, y76..284.
    # Exclude geometric edges so antialiasing/coverage cannot mimic filtering.
    values = values[84:276, 224:416]
    require(values.std() > 3, 'Sampler comparison is vacuous')
    return values


def difference(first, second):
    delta = np.abs(pixels(first)-pixels(second))
    return dict(meanAbsolutePixelError=float(delta.mean()),
                changedPixelFraction=float(np.mean(np.max(delta, axis=2) >= 8)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--technique', action='append', choices=('deferred-raster', 'forward-raster'))
    parser.add_argument('--case', action='append', choices=('mag', 'min', 'shared', 'transparent', 'mask', 'edit'))
    parser.add_argument('--roundtrip', action='store_true')
    parser.add_argument('--ray-smoke', action='store_true', help='Also validate hard ray-shadow and DDGI sampler layouts on a shared-image alpha fixture')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    require(renderer.is_file(), f'Missing renderer: {renderer}')
    output.mkdir(parents=True, exist_ok=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
    binary_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report = dict(schemaVersion=1, passed=False, executableSha256=binary_hash, captures=[], checks=[], fixtures=[])
    def save():
        (output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')

    def capture(source, name, technique, time=None, destination=None, extra=()):
        image = output/(name+'.png')
        telemetry = image.with_suffix('.telemetry.json')
        image.unlink(missing_ok=True); telemetry.unlink(missing_ok=True)
        command = [str(renderer), '--model', str(source), '--hidden', '--no-ui', '--validation',
                   '--width', '640', '--height', '360', '--msaa', '1', '--no-taa', '--no-bloom',
                   '--environment-intensity', '0', '--directional-intensity', '0', '--exposure', '1',
                   '--camera-position', '0', '0', '3', '--camera-target', '0', '0', '0', '--camera-fov', '60',
                   '--warmup-frames', '3', '--capture-frame', '4', '--fixed-dt', '.016666667',
                   '--render-technique', technique, '--display-mode', 'lit', '--screenshot', str(image)]
        if time is not None:
            command += ['--animation-clip', '0', '--animation-time', str(time), '--animation-paused']
        if destination is not None:
            destination.unlink(missing_ok=True)
            command += ['--save-scene', str(destination)]
        command += list(extra)
        log = image.with_suffix('.log')
        with log.open('w', encoding='utf-8') as stream:
            process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                timeout=args.timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        text = log.read_text(encoding='utf-8', errors='replace')
        require(process.returncode == 0, f'Native sampler process failed: {log}')
        require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error', text, re.I), f'Vulkan validation failed: {log}')
        require(image.is_file() and telemetry.is_file(), f'Missing fresh sampler capture: {image}')
        state = json.loads(telemetry.read_text(encoding='utf-8'))
        require(animation.finite_json(state) and state['technique'] == technique, 'Wrong or nonfinite sampler telemetry')
        require(state['scene']['primaryObjectCount'] >= 1, 'No filter probe was rendered')
        if time is not None:
            actual = state['gltfAnimation']
            require(actual['selectedClip'] == 0 and actual['diagnostics'] == 0 and not actual['playing'] and
                    abs(actual['time']-time) < 1e-6, 'Factor edit was not applied')
        record = dict(image=str(image), telemetry=str(telemetry), log=str(log), command=command)
        report['captures'].append(record); save()
        pixels(record)
        return record

    save()
    try:
        for case in args.case or ('mag', 'min', 'shared', 'transparent'):
            for repeat in ((4, 8) if case == 'min' else (1,)):
                configs = [(name, value, 9728) for name, value in FILTERS.items()] if case == 'min' else [
                    ('nearest', 9987, 9728), ('linear', 9987, 9729)]
                if case == 'shared':
                    configs = [('shared', 9987, 9728)]
                fixtures = []
                for name, min_filter, mag_filter in configs:
                    source = output/f'{case}-{repeat}-{name}.gltf'
                    write_fixture(source, min_filter, mag_filter, repeat,
                                  'BLEND' if case == 'transparent' else 'MASK' if case == 'mask' else 'OPAQUE',
                                  case == 'edit', case == 'shared')
                    fixtures.append((name, source))
                    report['fixtures'].append(dict(case=case, repeat=repeat, path=str(source)))
                if args.generate_only:
                    continue
                for technique in args.technique or ('deferred-raster', 'forward-raster'):
                    for time in ((0., 1.) if case == 'edit' else (None,)):
                        captures = {}
                        for name, source in fixtures:
                            stem = f'{technique}-{case}-{repeat}-{name}-time{time}'
                            destination = output/(stem+'-saved.glb') if args.roundtrip else None
                            captures[name] = capture(source, stem, technique, time, destination)
                            if destination is not None:
                                source_model, saved = read_model(source), read_model(destination)
                                expected = texture_sampler_policies(source_model)
                                actual = texture_sampler_policies(saved)
                                require(actual[:len(expected)] == expected, 'Save changed authored texture sampler policies')
                                reloaded = capture(destination, stem+'-reload', technique, time)
                                delta = difference(captures[name], reloaded)
                                require(delta['meanAbsolutePixelError'] <= .1, f'Saved sampler appearance changed: {delta}')
                                report['checks'].append(dict(case=case, check='roundtrip', name=name, technique=technique, **delta))
                        if case == 'shared':
                            image = pixels(captures['shared'])
                            left, right = image[:, :image.shape[1]//2], image[:, image.shape[1]//2:]
                            state = json.loads(Path(captures['shared']['telemetry']).read_text(encoding='utf-8'))
                            require(state['scene']['primaryObjectCount'] == 2 and left.std() > 3 and right.std() > 3,
                                    'Both shared-image sampler probes must be visible')
                            mixed = lambda values: float(np.mean((values[:,:,0] > 70) & (values[:,:,1] > 70)))
                            nearest_fraction, linear_fraction = mixed(left), mixed(right)
                            require(nearest_fraction < .03 and linear_fraction > .15,
                                    f'Shared image aliased its two sampler policies: {nearest_fraction}, {linear_fraction}')
                            checks = [dict(check='shared-image-sampler-routing', nearestMixedFraction=nearest_fraction,
                                           linearMixedFraction=linear_fraction)]
                        elif case in ('mag', 'transparent', 'edit'):
                            nearest, linear = pixels(captures['nearest']), pixels(captures['linear'])
                            mixed = lambda values: float(np.mean((values[:,:,0] > 70) & (values[:,:,1] > 70)))
                            left, right = mixed(nearest), mixed(linear)
                            require(left < .03 and right > .15, f'Magnification did not retain pixel-art vs bilinear edges: {left}, {right}')
                            checks = [dict(check='magnification', nearestMixedFraction=left, linearMixedFraction=right,
                                           **difference(captures['nearest'], captures['linear']))]
                        elif case == 'mask':
                            delta = difference(captures['nearest'], captures['linear'])
                            require(delta['changedPixelFraction'] > .01, f'Alpha-mask filter had no coverage effect: {delta}')
                            checks = [dict(check='mask-coverage', **delta)]
                        else:
                            checks = []
                            pairs = [('nearest', 'nearest_mipmap_nearest', False),
                                     ('linear', 'linear_mipmap_nearest', False)] if repeat == 4 else [
                                     ('nearest_mipmap_nearest', 'nearest_mipmap_linear', True),
                                     ('linear_mipmap_nearest', 'linear_mipmap_linear', True),
                                     ('linear', 'linear_mipmap_linear', True), ('nearest', 'linear', True)]
                            for first, second, distinct in pairs:
                                delta = difference(captures[first], captures[second])
                                require(delta['meanAbsolutePixelError'] > .5 and delta['changedPixelFraction'] > .01 if distinct else
                                        delta['meanAbsolutePixelError'] <= .1,
                                        f'Unexpected base/mip sampling behavior for {first}/{second}: {delta}')
                                checks.append(dict(check='minification', first=first, second=second, distinct=distinct, **delta))
                        for check in checks:
                            report['checks'].append(dict(case=case, repeat=repeat, time=time, technique=technique, **check))
                        save()
                        print('PASS', case, repeat, technique, time, flush=True)
        if args.ray_smoke:
            source = output/'ray-alpha-shared.gltf'
            write_fixture(source, 9987, 9728, alpha_mode='MASK', shared=True)
            report['fixtures'].append(dict(case='ray-alpha-smoke', path=str(source)))
            if not args.generate_only:
                for technique in args.technique or ('deferred-raster', 'forward-raster'):
                    record = capture(source, technique+'-ray-alpha-shared', technique, extra=(
                        '--directional-intensity', '3', '--ray-shadows', 'hard', '--no-ray-shadow-denoise',
                        '--ddgi', '--ddgi-fit-scene', '--ddgi-fit-probes', '64', '--ddgi-rays', '32',
                        '--ddgi-probes-per-frame', '64', '--ddgi-ray-budget', '2048', '--ddgi-hysteresis', '0'))
                    state = json.loads(Path(record['telemetry']).read_text(encoding='utf-8'))
                    ray, ddgi = state['rayShadows'], state['ddgi']
                    require(ray['supported'] and ray['active'], 'Hard ray-shadow sampler smoke did not execute')
                    require(ddgi['supported'] and ddgi['active'] and ddgi['tracedRays'] > 0,
                            'DDGI sampler smoke did not trace rays')
                    report['checks'].append(dict(case='ray-alpha-smoke', technique=technique,
                                                  rayShadowActive=True, ddgiTracedRays=ddgi['tracedRays']))
                    save()
                    print('PASS ray-alpha-smoke', technique, flush=True)
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == binary_hash, 'Renderer changed during sampler regression')
        report['passed'] = True
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        save()
    print(f"PASS: {len(report['checks'])} checks, {len(report['captures'])} captures; {output}")


if __name__ == '__main__':
    main()
