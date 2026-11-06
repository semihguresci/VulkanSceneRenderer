"""Generated native material-variant/history/export checks with analytical UV frames.

Static, skinned and morphed missing-tangent geometry switches from UV0 to UV1,
undoes, redoes and resets inside one session. Independent authored tangents are
the pixel oracle. No downloaded assets or production tangent code form oracle.
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

import numpy as np
from PIL import Image

import gltf_tangent_uv_runtime_regression as uv

CASES = ('static', 'skin', 'morph')
SAMPLES = {1: -1, 3: 0, 5: -1, 7: 0, 9: -1, 11: 0}


def morph(model):
    data = bytearray(base64.b64decode(model['buffers'][0]['uri'].split(',', 1)[1]))
    while len(data) % 4:
        data.append(0)
    begin = len(data)
    data.extend(struct.pack('<9f', *([0., .15, 0.] * 3)))
    model['bufferViews'].append(dict(buffer=0, byteOffset=begin, byteLength=36))
    model['accessors'].append(dict(bufferView=len(model['bufferViews'])-1, componentType=5126,
                                   count=3, type='VEC3'))
    model['meshes'][0]['primitives'][0]['targets'] = [dict(POSITION=len(model['accessors'])-1)]
    model['meshes'][0]['weights'] = [.5]
    model['buffers'][0].update(byteLength=len(data), uri='data:application/octet-stream;base64,' + base64.b64encode(data).decode())


def fixtures(output, case):
    base_case = 'skin-override-uv1' if case == 'skin' else 'static-uv1'
    paths, models = {}, []
    for frame in ('generated', 'analytical', 'wrong-uv0'):
        path = output / f'{case}-{frame}.gltf'
        model = uv.write_fixture(path, base_case, frame)
        if case == 'morph':
            morph(model)
        if frame == 'generated':
            alternate = copy.deepcopy(model['materials'][0])
            model['materials'][0]['normalTexture'] = dict(index=0, texCoord=0)
            alternate['name'] = 'Alternate UV1 normal map'
            model['materials'].append(alternate)
            model.setdefault('extensionsUsed', []).append('KHR_materials_variants')
            model['extensions'] = {'KHR_materials_variants': dict(variants=[dict(name='UV1', extras=dict(sku='keep'))],
                                                                 extras=dict(retain=True))}
            model['meshes'][0]['primitives'][0].setdefault('extensions', {})['KHR_materials_variants'] = dict(
                mappings=[dict(material=1, variants=[0], extras=dict(retain='mapping'))])
        elif frame == 'wrong-uv0':
            model['materials'][0]['normalTexture'] = dict(index=0, texCoord=0)
        path.write_text(json.dumps(model, indent=2), encoding='utf-8')
        paths[frame] = path
        models.append(model)
    uv.verify_source_order(models)
    return paths


def classification_fixtures(output):
    path = output/'classification-source.gltf'
    model = uv.write_fixture(path, 'static-uv1', 'analytical')
    data = bytearray(base64.b64decode(model['buffers'][0]['uri'].split(',', 1)[1]))
    prototype = copy.deepcopy(model['meshes'][0]['primitives'][0])
    primitives = []
    for side, shift in enumerate((-.7, .7)):
        while len(data) % 4:
            data.append(0)
        begin = len(data)
        positions = [(x*.55+shift, y*.55, z) for x, y, z in uv.POSITIONS]
        data.extend(struct.pack('<9f', *(v for row in positions for v in row)))
        model['bufferViews'].append(dict(buffer=0, byteOffset=begin, byteLength=36))
        model['accessors'].append(dict(bufferView=len(model['bufferViews'])-1, componentType=5126, count=3, type='VEC3',
                                       min=[min(p[i] for p in positions) for i in range(3)],
                                       max=[max(p[i] for p in positions) for i in range(3)]))
        primitive = copy.deepcopy(prototype)
        primitive['attributes']['POSITION'] = len(model['accessors'])-1
        primitive['material'] = side
        primitives.append(primitive)
    model['buffers'][0].update(byteLength=len(data), uri='data:application/octet-stream;base64,'+base64.b64encode(data).decode())
    model['meshes'][0]['primitives'] = primitives
    model['nodes'] = [dict(mesh=0, name='Front shared instance'),
                      dict(mesh=0, name='Back shared instance', translation=[0., -.8, 0.], rotation=[0., 1., 0., 0.], scale=[.65]*3)]
    model['scenes'][0]['nodes'] = [0, 1]

    def material(color, alpha='OPAQUE', double=False):
        return dict(pbrMetallicRoughness=dict(baseColorFactor=color, metallicFactor=0., roughnessFactor=1.),
                    alphaMode=alpha, alphaCutoff=.5, doubleSided=double, extensions={'KHR_materials_unlit': {}})

    model['materials'] = [material([.9, .05, .05, 1.]), material([.05, .1, .9, 1.]),
                          material([.05, .9, .05, .35], 'BLEND', True), material([.9, .05, .9, 0.], 'MASK')]
    model['extensionsUsed'] = ['KHR_materials_unlit', 'KHR_materials_variants']
    model['extensions'] = {'KHR_materials_variants': dict(variants=[dict(name='Translucent backfaces'), dict(name='Masked away')])}
    primitives[0]['extensions'] = {'KHR_materials_variants': dict(mappings=[dict(material=2, variants=[0]), dict(material=3, variants=[1])])}
    path.write_text(json.dumps(model, indent=2), encoding='utf-8')
    oracles = {}
    for name, material_index in [('default', 0), ('blend', 2), ('mask', 3)]:
        oracle = copy.deepcopy(model)
        oracle['extensions'].pop('KHR_materials_variants')
        oracle['extensionsUsed'].remove('KHR_materials_variants')
        oracle['meshes'][0]['primitives'][0].pop('extensions')
        oracle['meshes'][0]['primitives'][0]['material'] = material_index
        oracle_path = output/f'classification-{name}-oracle.gltf'
        oracle_path.write_text(json.dumps(oracle, indent=2), encoding='utf-8')
        oracles[name] = oracle_path
    return path, oracles


def pixels(first, second):
    a = np.asarray(Image.open(first).convert('RGB'), dtype=np.float64)
    b = np.asarray(Image.open(second).convert('RGB'), dtype=np.float64)
    uv.require(a.shape == b.shape, 'Mismatched native image size')
    diff = np.abs(a-b)
    return dict(meanAbsoluteError=float(diff.mean()), maximumError=float(diff.max()),
                changedPixelFraction=float(np.any(diff > 0, axis=2).mean()))


def run(renderer, output, fixture, prefix, technique, display, records, *, sequence=None, save=None, timeout=180, ray=False, variant=None, gltf_scene=None, extra=()):
    image = output / (prefix+'.png')
    log = output / (prefix+'.log')
    for pattern in (prefix+'*.png', prefix+'*.telemetry.json'):
        for stale in output.glob(pattern):
            stale.unlink()
    command = [str(renderer), '--hidden', '--no-ui', '--validation', '--width', '640', '--height', '360',
               '--msaa', '1', '--no-taa', '--warmup-frames', '1', '--capture-frame', '2', '--fixed-dt', '.016666667',
               '--render-technique', technique, '--display-mode', display, '--no-bloom',
               '--environment-intensity', '0', '--directional-intensity', '3', '--directional-direction', '0', '1', '-1',
               '--exposure', '1', '--camera-position', '0', '0', '4', '--camera-target', '0', '0', '0',
               '--camera-fov', '45', '--model', str(fixture), '--screenshot', str(image)]
    if sequence:
        command.extend(['--capture-sequence', str(sequence)])
    if save:
        command.extend(['--save-scene', str(save)])
    if variant is not None:
        command.extend(['--gltf-variant', str(variant)])
    if gltf_scene is not None:
        command.extend(['--gltf-scene',str(gltf_scene)])
    command.extend(extra)
    if ray:
        command.extend(['--ray-shadows', 'hard', '--no-ray-shadow-denoise', '--ddgi', '--ddgi-counts', '3', '3', '3',
                        '--ddgi-rays', '4', '--ddgi-probes-per-frame', '27', '--ddgi-ray-budget', '108', '--ddgi-intensity', '0'])
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)),
                                 creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    text = log.read_text(encoding='utf-8', errors='replace')
    uv.require(process.returncode == 0, f'Variant capture failed ({process.returncode}): {log}')
    uv.require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error', text, re.I), f'Vulkan error: {log}')
    paths = sorted(output.glob(prefix+'*.telemetry.json'))
    uv.require(paths, f'No fresh capture telemetry: {log}')
    result = []
    for telemetry in paths:
        image_path = telemetry.with_suffix('').with_suffix('.png')
        state = json.loads(telemetry.read_text(encoding='utf-8'))
        uv.require(image_path.is_file() and uv.finite_json(state), f'Invalid capture {telemetry}')
        uv.require(state['technique'] == technique, f'Wrong technique: {telemetry}')
        record = dict(image=str(image_path), telemetry=str(telemetry), log=str(log), command=command)
        records.append(record)
        result.append(dict(**record, state=state))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--skip-classification', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    uv.require(renderer.is_file(), f'Missing renderer: {renderer}')
    output.mkdir(parents=True, exist_ok=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
    binary_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report = dict(schemaVersion=1, passed=False, generatedOnly=args.generate_only,
                  nativeValidationPerformed=False, renderer=str(renderer), rendererSha256=binary_hash,
                  captures=[], checks=[], cases=[])

    def persist():
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')

    try:
        for case in args.case or CASES:
            paths = fixtures(output, case)
            sequence = output / f'{case}-sequence.json'
            sequence.write_text(json.dumps(dict(schemaVersion=1, frames=11, sampleFrames=list(SAMPLES), events=[
                dict(frame=3, materialVariant=0), dict(frame=5, sceneHistory='undo'),
                dict(frame=7, sceneHistory='redo'), dict(frame=9, materialVariant=-1), dict(frame=11, materialVariant=0)
            ]), indent=2), encoding='utf-8')
            report['cases'].append(dict(case=case, fixtures={k:str(v) for k,v in paths.items()}, sequence=str(sequence)))
            persist()
            if args.generate_only:
                continue
            for technique, display in [('deferred-raster', 'lit'), ('forward-raster', 'lit'), ('deferred-raster', 'normals')]:
                name = f'{case}-{technique}-{display}'
                default = run(renderer, output, paths['wrong-uv0'], name+'-default-oracle', technique, display, report['captures'])[0]
                alternate = run(renderer, output, paths['analytical'], name+'-uv1-oracle', technique, display, report['captures'])[0]
                contrast = pixels(default['image'], alternate['image'])
                uv.require(contrast['meanAbsoluteError'] > .05, f'UV oracle is vacuous: {name}')
                saved = output / (name+'-saved.glb')
                captured = run(renderer, output, paths['generated'], name+'-history', technique, display, report['captures'], sequence=sequence, save=saved)
                uv.require(len(captured) == len(SAMPLES), f'Wrong sequence capture count: {name}')
                # Application names sequence samples with the simulation frame.
                for record, (frame, selection) in zip(captured, sorted(SAMPLES.items())):
                    state = record['state']['gltfMaterialVariants']
                    uv.require(state['selected'] == selection and state['count'] == 1, f'Selection/history mismatch {record["telemetry"]}')
                    expected = alternate if selection == 0 else default
                    error = pixels(record['image'], expected['image'])
                    uv.require(error['maximumError'] == 0, f'Variant analytical mismatch: {name} frame{frame}: {error}')
                    report['checks'].append(dict(case=case, technique=technique, display=display, frame=frame,
                                                 selected=selection, **error))
                uv.require(saved.is_file(), f'No portable selected-appearance export: {saved}')
                reload = run(renderer, output, saved, name+'-reload', technique, display, report['captures'])[0]
                uv.require(reload['state']['gltfMaterialVariants']['selected'] == -1, 'Saved variant must reload at Default')
                error = pixels(reload['image'], alternate['image'])
                uv.require(error['maximumError'] == 0, f'Saved selected appearance changed: {name}: {error}')
                report['checks'].append(dict(case=case, technique=technique, display=display, roundtrip=True, **error))
                if case == 'static' and technique == 'deferred-raster' and display == 'lit':
                    ascii_saved = output/(name+'-cli.gltf')
                    direct = run(renderer, output, paths['generated'], name+'-cli', technique, display, report['captures'], variant=0, save=ascii_saved)[0]
                    uv.require(direct['state']['gltfMaterialVariants']['selected']==0, 'CLI variant selection was not applied')
                    error = pixels(direct['image'], alternate['image'])
                    uv.require(error['maximumError']==0, f'CLI selected appearance differs: {error}')
                    report['checks'].append(dict(case=case, cli=True, **error))
                    portable = run(renderer, output, ascii_saved, name+'-ascii-reload', technique, display, report['captures'])[0]
                    uv.require(portable['state']['gltfMaterialVariants']['selected']==-1, 'ASCII selected appearance did not reload at Default')
                    error = pixels(portable['image'], alternate['image'])
                    uv.require(error['maximumError']==0, f'ASCII selected appearance differs: {error}')
                    report['checks'].append(dict(case=case, asciiRoundtrip=True, **error))
                report['nativeValidationPerformed'] = True
                persist()
                print('PASS', name, flush=True)
        if not args.skip_classification:
            source, oracle_paths = classification_fixtures(output)
            samples = {1: -1, 3: 0, 5: -1, 7: 0, 9: 1, 11: -1, 13: 0}
            sequence = output/'classification-sequence.json'
            sequence.write_text(json.dumps(dict(schemaVersion=1, frames=13, sampleFrames=list(samples), events=[
                dict(frame=3, materialVariant=0), dict(frame=5, sceneHistory='undo'), dict(frame=7, sceneHistory='redo'),
                dict(frame=9, materialVariant=1), dict(frame=11, materialVariant=-1), dict(frame=13, materialVariant=0)
            ]), indent=2), encoding='utf-8')
            report['cases'].append(dict(case='classification', source=str(source), oracles={k:str(v) for k,v in oracle_paths.items()}, sequence=str(sequence)))
            if not args.generate_only:
                for technique in uv.TECHNIQUES:
                    prefix = f'classification-{technique}'
                    ray = technique == 'deferred-raster'
                    oracles = {name:run(renderer, output, path, prefix+'-'+name+'-reference', technique, 'lit', report['captures'], ray=ray)[0]
                               for name, path in oracle_paths.items()}
                    uv.require(pixels(oracles['default']['image'], oracles['blend']['image'])['meanAbsoluteError']>.1,
                               'Classification oracle has no visible alpha/backface difference')
                    saved = output/(prefix+'-saved.glb')
                    captures = run(renderer, output, source, prefix+'-history', technique, 'lit', report['captures'], sequence=sequence, save=saved, ray=ray)
                    uv.require(len(captures)==len(samples), 'Classification sequence capture count mismatch')
                    baseline_materials = None
                    previous_epoch = None
                    previous_ray_generation = None
                    for capture, (frame, selection) in zip(captures, sorted(samples.items())):
                        variants = capture['state']['gltfMaterialVariants']
                        uv.require(variants['count']==2 and variants['selected']==selection, 'Classification variant selection differs')
                        uv.require(len(variants['assignments'])==4, 'Shared instances or primitive assignments were lost')
                        unmapped = [a['material'] for a in variants['assignments'] if a['primitive']==1]
                        baseline_materials = unmapped if baseline_materials is None else baseline_materials
                        uv.require(unmapped==baseline_materials, 'Unmapped primitive materials changed')
                        for assignment in variants['assignments']:
                            mapped = assignment['primitive']==0
                            uv.require(assignment['transparent']==(mapped and selection==0), 'Blend draw classification differs')
                            uv.require(assignment['alphaMasked']==(mapped and selection==1), 'Mask draw classification differs')
                            uv.require(assignment['doubleSided']==(mapped and selection==0), 'Double-sided draw classification differs')
                        epoch = capture['state']['taa']['epoch']
                        uv.require(previous_epoch is None or epoch>previous_epoch, 'Variant/history did not invalidate temporal epoch')
                        previous_epoch = epoch
                        if ray:
                            ray_state, gi = capture['state']['rayShadows'], capture['state']['ddgi']
                            uv.require(ray_state['supported'] and ray_state['active'] and gi['supported'] and gi['active'],
                                       'Deferred classification fixture did not exercise ray queries and DDGI')
                            uv.require(ray_state['build']['instances'] == 4, 'Ray scene lost shared primitive instances')
                            generation = ray_state['generation']
                            uv.require(previous_ray_generation is None or generation > previous_ray_generation,
                                       'Material variant/history did not refresh the ray scene')
                            previous_ray_generation = generation
                        reference = oracles['blend' if selection==0 else 'mask' if selection==1 else 'default']
                        error = pixels(capture['image'], reference['image'])
                        uv.require(error['maximumError']==0, f'Classification pixels differ: {prefix} frame{frame}: {error}')
                        report['checks'].append(dict(case='classification', technique=technique, frame=frame, selected=selection, **error))
                    reload = run(renderer, output, saved, prefix+'-reload', technique, 'lit', report['captures'], ray=ray)[0]
                    uv.require(reload['state']['gltfMaterialVariants']['selected']==-1, 'Selected appearance did not reload as Default')
                    error = pixels(reload['image'], oracles['blend']['image'])
                    uv.require(error['maximumError']==0, f'Saved classification pixels differ: {prefix}: {error}')
                    report['checks'].append(dict(case='classification', technique=technique, roundtrip=True, **error))
                    persist()
                    print('PASS', prefix, flush=True)
        uv.require(hashlib.sha256(renderer.read_bytes()).hexdigest() == binary_hash, 'Renderer changed during validation')
        report['passed'] = None if args.generate_only else True
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        persist()
    print(f'{"GENERATED" if args.generate_only else "PASS"}: {report["captureCount"]} captures, {report["checkCount"]} checks; {output}')


if __name__ == '__main__':
    main()
