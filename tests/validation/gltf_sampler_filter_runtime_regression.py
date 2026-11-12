"""Native glTF filter, shared-image cache and edited-material image regressions.

Generates tiny embedded PNG fixtures, uses both raster paths, and checks
nearest/bilinear magnification plus base/mip minification without TAA or bloom.
Optional round trips verify saved sampler policy and matching reloaded pixels.
The sampler-edit case always exercises a live edit, history, and GLB reload.
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
SAMPLER_DEFAULTS = dict(minFilter=9987, magFilter=9729, wrapS=10497, wrapT=10497)
SHARED_PROBE_NAMES = ('Nearest shared-image probe', 'Linear shared-image probe')


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
        model['nodes'] = [dict(name=SHARED_PROBE_NAMES[0], mesh=0, translation=[-.5, 0, 0], scale=[.5, 1, 1]),
                          dict(name=SHARED_PROBE_NAMES[1], mesh=1, translation=[.5, 0, 0], scale=[.5, 1, 1])]
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
        policies.append(tuple(sampler.get(name, default) for name, default in SAMPLER_DEFAULTS.items()))
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


def mixed_fraction(values):
    return float(np.mean((values[:, :, 0] > 70) & (values[:, :, 1] > 70)))


def same_pixels(first, second):
    with Image.open(first['image']) as left, Image.open(second['image']) as right:
        return left.mode == right.mode and np.array_equal(np.asarray(left), np.asarray(right))


def live_shared_bindings(record, expected, history):
    state = json.loads(Path(record['telemetry']).read_text(encoding='utf-8'))
    catalog = state['gltfMaterialTextures']
    require(state['scene']['primaryObjectCount'] == 2 and not catalog['resourcesTruncated'] and
            not catalog['lastError'] and catalog['archiveImageCount'] == catalog['evaluatedImageCount'] == 1,
            'Sampler edit lost a probe, duplicated the image, or failed to publish')
    cursor = state['gltfMaterialVariants']
    require((cursor.get('undoCount', 0), cursor.get('redoCount', 0)) == history,
            f'Sampler edit did not reach the expected history cursor {history}')
    textures = {row['sourceTexture']: row for row in catalog['textures']}
    resources = {row['runtimeTexture']: row for row in catalog['runtimeResources']}
    slots = {row['sourceMaterial']: row for row in catalog['slots'] if row['slot'] == 'baseColor'}
    bindings, sampler_slots = [], []
    for material, policy in enumerate(expected):
        slot = slots[material]
        texture = textures[slot['authoredTexture']]
        resource = resources[slot['runtimeTexture']]
        authored = {name: texture[name] for name in SAMPLER_DEFAULTS}
        require(authored == policy and texture['image'] == 0 and slot['active'] and slot['srgb'] and
                texture['evaluatedTexture'] == slot['evaluatedTexture'] and
                texture['colorResource'] == slot['runtimeTexture'],
                f'Wrong source policy or evaluated texture for material {material}: {authored}')
        require(0 <= resource['samplerSlot'] < resource['samplerCount'], 'Invalid dense sampler slot')
        sampler_slots.append(resource['samplerSlot'])
        bindings.append(dict(sourceTexture=slot['authoredTexture'], evaluatedTexture=slot['evaluatedTexture'],
                             runtimeTexture=slot['runtimeTexture'], image=texture['image'], policy=authored))
    require(len(set(sampler_slots)) == 2, 'Distinct shared-image policies aliased the same sampler descriptor')
    return bindings, sampler_slots


def verify_saved_shared_policies(path, expected):
    model = read_model(path)
    sources = []
    for name, policy in zip(SHARED_PROBE_NAMES, expected, strict=True):
        node, = (node for node in model['nodes'] if node.get('name') == name)
        primitive, = model['meshes'][node['mesh']]['primitives']
        material = model['materials'][primitive['material']]
        texture = model['textures'][material['pbrMetallicRoughness']['baseColorTexture']['index']]
        sampler = model['samplers'][texture['sampler']]
        for field, value in policy.items():
            matches = field not in sampler if value == -1 else sampler.get(field) == value
            require(matches, f'Save changed authored {field} for {name}')
        sources.append(texture['source'])
    require(len(set(sources)) == 1, 'Save split the shared image into different sources')


def verify_sampler_edit(records, saved, reloaded, original, edited, omitted, untouched):
    policies = (original, edited, original, edited, omitted)
    history = ((0, 0), (1, 0), (0, 1), (1, 0), (2, 0))
    observed = [live_shared_bindings(record, (policy, untouched), cursor)
                for record, policy, cursor in zip(records, policies, history, strict=True)]
    bindings = [binding for binding, _ in observed]
    require(all(binding[1] == bindings[0][1] for binding in bindings[1:]),
            'The untouched material changed its source or runtime texture binding')
    require(bindings[0] == bindings[2] and bindings[1] == bindings[3],
            'Undo or redo changed a shared-image texture binding')
    require(observed[3][1][0] == observed[4][1][0], 'Omitted defaults changed the effective sampler descriptor')
    images = [pixels(record) for record in records]
    left_before, right_before = np.split(images[0], 2, axis=1)
    left_after, _ = np.split(images[1], 2, axis=1)
    nearest, linear = mixed_fraction(left_before), mixed_fraction(left_after)
    require(nearest < .03 and linear > .15, f'Filter-only edit did not change nearest to bilinear: {nearest}, {linear}')
    require(mixed_fraction(right_before) > .15, 'The untouched linear probe is missing or uses the wrong sampler')
    require(all(np.array_equal(np.split(image, 2, axis=1)[1], right_before) for image in images[1:]),
            'Sampler edit changed pixels on the untouched shared-image probe')
    require(same_pixels(records[0], records[2]) and same_pixels(records[1], records[3]),
            'Undo or redo did not restore the expected sampler pixels')
    require(same_pixels(records[3], records[4]), 'Omitted filter defaults changed the rendered pixels')
    verify_saved_shared_policies(saved, (omitted, untouched))
    require(same_pixels(records[-1], reloaded), 'Sampler-only save and reload changed the rendered pixels')
    return [dict(check='sampler-edit-bindings', before=bindings[0], edited=bindings[1], undo=bindings[2],
                 redo=bindings[3], omitted=bindings[4], omittedSamplerSlot=observed[4][1][0]),
            dict(check='sampler-edit-pixels', nearestMixedFraction=nearest, linearMixedFraction=linear,
                 untouchedPixelsExact=True, undoPixelsExact=True, redoPixelsExact=True, omittedPixelsExact=True),
            dict(check='sampler-edit-roundtrip', saved=str(saved), expectedPolicies=[omitted, untouched], pixelsExact=True)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--technique', action='append', choices=('deferred-raster', 'forward-raster'))
    parser.add_argument('--case', action='append', choices=('mag', 'min', 'shared', 'transparent', 'mask', 'edit', 'sampler-edit'))
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
    report = dict(schemaVersion=1, passed=False, generatedOnly=args.generate_only,
                  executableSha256=binary_hash, captures=[], checks=[], fixtures=[])
    def save():
        (output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')

    def capture_frames(source, name, technique, time=None, destination=None, extra=(), sequence=None):
        image = output/(name+'.png')
        if sequence is None:
            samples = [(None, image)]
        else:
            script = json.loads(sequence.read_text(encoding='utf-8'))
            samples = [(frame, image if frame == script['frames'] else image.with_name(f'{name}.frame-{frame:04d}.png'))
                       for frame in script['sampleFrames']]
        for _, sample in samples:
            sample.unlink(missing_ok=True)
            sample.with_suffix('.telemetry.json').unlink(missing_ok=True)
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
        if sequence is not None:
            command += ['--capture-sequence', str(sequence)]
        command += list(extra)
        log = image.with_suffix('.log')
        with log.open('w', encoding='utf-8') as stream:
            process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                timeout=args.timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        text = log.read_text(encoding='utf-8', errors='replace')
        require(process.returncode == 0, f'Native sampler process failed: {log}')
        require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error', text, re.I), f'Vulkan validation failed: {log}')
        records = []
        for frame, sample in samples:
            telemetry = sample.with_suffix('.telemetry.json')
            require(sample.is_file() and telemetry.is_file(), f'Missing fresh sampler capture: {sample}')
            state = json.loads(telemetry.read_text(encoding='utf-8'))
            require(animation.finite_json(state) and state['technique'] == technique and state['validationEnabled'],
                    'Wrong or nonfinite sampler telemetry, or validation disabled')
            require(state['scene']['primaryObjectCount'] >= 1, 'No filter probe was rendered')
            if time is not None:
                actual = state['gltfAnimation']
                require(actual['selectedClip'] == 0 and actual['diagnostics'] == 0 and not actual['playing'] and
                        abs(actual['time']-time) < 1e-6, 'Factor edit was not applied')
            record = dict(image=str(sample), telemetry=str(telemetry), log=str(log), command=command)
            if frame is not None:
                record['frame'] = frame
            report['captures'].append(record); save()
            pixels(record)
            records.append(record)
        return records

    def capture(source, name, technique, time=None, destination=None, extra=()):
        record, = capture_frames(source, name, technique, time, destination, extra)
        return record

    save()
    try:
        for case in args.case or ('mag', 'min', 'shared', 'transparent'):
            if case == 'sampler-edit':
                original = dict(minFilter=9728, magFilter=9728, wrapS=33071, wrapT=33071)
                edited = dict(original, minFilter=9987, magFilter=9729)
                omitted = dict(original, minFilter=-1, magFilter=-1)
                untouched = dict(original, magFilter=9729)
                source, sequence = output/'sampler-edit.gltf', output/'sampler-edit-sequence.json'
                write_fixture(source, original['minFilter'], original['magFilter'], shared=True)

                def sampler_edit(frame, policy):
                    return dict(frame=frame, materialTextureEdit=dict(
                        sourceMaterial=0, slot='baseColor', operation='editSampler', sampler=policy))

                script = dict(schemaVersion=1, frames=20, sampleFrames=[4, 8, 12, 16, 20], events=[
                    sampler_edit(5, edited), dict(frame=9, sceneHistory='undo'), dict(frame=13, sceneHistory='redo'),
                    sampler_edit(17, omitted)])
                sequence.write_text(json.dumps(script, indent=2), encoding='utf-8')
                report['fixtures'].append(dict(case=case, path=str(source), sequence=str(sequence)))
                if args.generate_only:
                    continue
                for technique in args.technique or ('deferred-raster', 'forward-raster'):
                    stem = f'{technique}-sampler-edit'
                    saved = output/(stem+'-saved.glb')
                    records = capture_frames(source, stem, technique, destination=saved, sequence=sequence)
                    reloaded = capture(saved, stem+'-reload', technique)
                    checks = verify_sampler_edit(records, saved, reloaded, original, edited, omitted, untouched)
                    report['checks'].extend(dict(case=case, technique=technique, **check) for check in checks)
                    save()
                    print('PASS', case, technique, flush=True)
                continue
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
                            nearest_fraction, linear_fraction = mixed_fraction(left), mixed_fraction(right)
                            require(nearest_fraction < .03 and linear_fraction > .15,
                                    f'Shared image aliased its two sampler policies: {nearest_fraction}, {linear_fraction}')
                            checks = [dict(check='shared-image-sampler-routing', nearestMixedFraction=nearest_fraction,
                                           linearMixedFraction=linear_fraction)]
                        elif case in ('mag', 'transparent', 'edit'):
                            nearest, linear = pixels(captures['nearest']), pixels(captures['linear'])
                            left, right = mixed_fraction(nearest), mixed_fraction(linear)
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
        report['passed'] = None if args.generate_only else True
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        save()
    if args.generate_only:
        print(f"Generated {len(report['fixtures'])} fixtures in {output}")
    else:
        print(f"PASS: {len(report['checks'])} checks, {len(report['captures'])} captures; {output}")


if __name__ == '__main__':
    main()
