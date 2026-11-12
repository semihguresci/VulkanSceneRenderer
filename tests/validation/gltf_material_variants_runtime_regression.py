"""Generated native material-variant/history/export checks with analytical UV frames.

Static, skinned and morphed missing-tangent geometry switches from UV0 to UV1,
undoes, redoes and resets inside one session. Independent authored tangents are
the pixel oracle. No downloaded assets or production tangent code form oracle.
Shared-instance fixtures exercise alpha classification, named looks after
portable export, and recovery from a malformed replacement.
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
from gltf_animated_export_runtime_regression import read_model

CASES = ('static', 'skin', 'morph')
SAMPLES = {1: -1, 3: 0, 5: -1, 7: 0, 9: -1, 11: 0}
EXTENSION = 'KHR_materials_variants'
CLASSIFICATION_LOOKS = {-1: 'default', 0: 'blend', 1: 'mask'}


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
    model['extensionsUsed'] = ['KHR_materials_unlit', EXTENSION]
    model['extensions'] = {EXTENSION: dict(
        variants=[dict(name='Translucent backfaces', extras=dict(sku='blend')), dict(name='Masked away')],
        extras=dict(source='classification'))}
    primitives[0]['extensions'] = {EXTENSION: dict(
        mappings=[dict(material=2, variants=[0], extras=dict(look='blend')),
                  dict(material=3, variants=[1], extras=dict(look='mask'))])}
    path.write_text(json.dumps(model, indent=2), encoding='utf-8')
    oracles = {}
    for name, material_index in [('default', 0), ('blend', 2), ('mask', 3)]:
        oracle = copy.deepcopy(model)
        oracle['extensions'].pop(EXTENSION)
        oracle['extensionsUsed'].remove(EXTENSION)
        oracle['meshes'][0]['primitives'][0].pop('extensions')
        oracle['meshes'][0]['primitives'][0]['material'] = material_index
        oracle_path = output/f'classification-{name}-oracle.gltf'
        oracle_path.write_text(json.dumps(oracle, indent=2), encoding='utf-8')
        oracles[name] = oracle_path
    return path, oracles


def classification_reload_fixture(output, source, oracles):
    model = read_model(source)
    invalid = copy.deepcopy(model)
    invalid['meshes'][0]['primitives'][0]['extensions'][EXTENSION]['mappings'].append(
        dict(material=3, variants=[0]))
    malformed = output/'classification-duplicate-mapping.gltf'
    malformed.write_text(json.dumps(invalid, indent=2), encoding='utf-8')
    replacement = output/'classification-replacement.gltf'
    replacement.write_text(json.dumps(model, indent=2), encoding='utf-8')

    camera = [dict(frame=1, position=[0, 0, 4], target=[0, 0, 0])]
    sequence = output/'classification-live-reload.json'
    sequence.write_text(json.dumps(dict(
        schemaVersion=1, frames=9, sampleFrames=[1, 3, 5, 7, 9], camera=camera,
        events=[dict(frame=3, materialVariant=0), dict(frame=5, reload=str(malformed)),
                dict(frame=7, reload=str(replacement)), dict(frame=9, materialVariant=1)]),
        indent=2), encoding='utf-8')
    # A successful reload clears the editor selection and its viewport outline.
    # Reload each independent core-material oracle to match that editor state.
    oracle_sequence = output/'classification-live-reload-oracles.json'
    oracle_sequence.write_text(json.dumps(dict(
        schemaVersion=1, frames=5, sampleFrames=[3, 5], camera=camera,
        events=[dict(frame=2, reload=str(oracles['default'])), dict(frame=4, reload=str(oracles['mask']))]),
        indent=2), encoding='utf-8')
    return malformed, replacement, sequence, oracle_sequence


def mapped_primitives(model):
    return {(mesh_id, primitive_id): primitive
            for mesh_id, mesh in enumerate(model['meshes'])
            for primitive_id, primitive in enumerate(mesh['primitives'])
            if EXTENSION in primitive.get('extensions', {})}


def material_signature(model, index, path):
    materials = model['materials']
    uv.require(type(index) is int and 0 <= index < len(materials), f'Invalid material {index}: {path}')
    material = materials[index]
    normal = material.get('normalTexture')
    normal_uv = None
    if normal is not None:
        transform = normal.get('extensions', {}).get('KHR_texture_transform', {})
        normal_uv = transform.get('texCoord', normal.get('texCoord', 0))
    return material.get('alphaMode', 'OPAQUE'), material.get('doubleSided', False), normal_uv


def check_exported_variants(source, saved, selected):
    authored, exported = read_model(source), read_model(saved)
    root = exported.get('extensions', {}).get(EXTENSION)
    uv.require(EXTENSION in exported.get('extensionsUsed', []) and root == authored['extensions'][EXTENSION],
               f'Exported variant names or root metadata changed: {saved}')
    original = mapped_primitives(authored)
    emitted = mapped_primitives(exported)
    # These fixtures have one mapped primitive definition. Both instances of
    # the classification mesh share it, so the exported mesh may be reused.
    uv.require(len(original) == 1 and len(emitted) == 1, f'Exported primitive mappings changed: {saved}')
    original_primitive = next(iter(original.values()))
    exported_primitive = next(iter(emitted.values()))
    original_extension = original_primitive['extensions'][EXTENSION]
    exported_extension = exported_primitive['extensions'][EXTENSION]
    original_mappings = original_extension['mappings']
    exported_mappings = exported_extension.get('mappings', [])
    uv.require(len(original_mappings) == len(exported_mappings), f'Exported mapping count changed: {saved}')
    restored = copy.deepcopy(exported_extension)
    selected_material = None
    for expected, mapped in zip(original_mappings, restored['mappings']):
        signature = material_signature(exported, mapped['material'], saved)
        uv.require(signature == material_signature(authored, expected['material'], source),
                   f'Exported mapping points to the wrong material: {saved}')
        if selected in expected['variants']:
            selected_material = mapped['material']
        mapped['material'] = expected['material']
    uv.require(restored == original_extension, f'Exported mapping indices or metadata changed: {saved}')
    uv.require(selected_material is not None and exported_primitive.get('material') == selected_material,
               f'Exported Default did not retain the selected material: {saved}')
    return set(emitted)


def check_classification(capture, selection, appearance, mapped_primitives):
    variants = capture['state']['gltfMaterialVariants']
    location = capture['telemetry']
    uv.require(variants['count'] == 2 and variants['selected'] == selection,
               f'Classification variant selection differs: {location}')
    mapped_materials, unmapped = [], {}
    assignments = variants['assignments']
    for assignment in assignments:
        primitive = assignment['mesh'], assignment['primitive']
        mapped = primitive in mapped_primitives
        uv.require(assignment['transparent'] == (mapped and appearance == 'blend'),
                   f'Blend draw classification differs: {location}')
        uv.require(assignment['alphaMasked'] == (mapped and appearance == 'mask'),
                   f'Mask draw classification differs: {location}')
        uv.require(assignment['doubleSided'] == (mapped and appearance == 'blend'),
                   f'Double-sided draw classification differs: {location}')
        if mapped:
            mapped_materials.append(assignment['material'])
        else:
            unmapped[assignment['node']] = assignment['material']
    uv.require(len(assignments) == 4 and len(mapped_materials) == len(unmapped) == 2
               and len(set(mapped_materials)) == 1 and len(set(unmapped.values())) == 1,
               f'Shared instances or primitive assignments were lost: {location}')
    return unmapped


def check_live_reload(captures, mapped, malformed, replacement, oracles, reloaded_oracles, checks):
    expected = [(1, -1, 'default'), (3, 0, 'blend'), (5, 0, 'blend'), (7, -1, 'default'), (9, 1, 'mask')]
    uv.require(len(captures) == len(expected), 'Live reload capture count mismatch')
    selected, rejected, recovered = (record['state'] for record in captures[1:4])
    uv.require(rejected['scene'] == selected['scene']
               and rejected['gltfMaterialVariants'] == selected['gltfMaterialVariants'],
               f'Malformed replacement changed the active scene or variant history: {malformed}')
    uv.require(Path(recovered['scene']['primary']).resolve() == replacement,
               f'Valid replacement was not installed: {replacement}')
    uv.require(recovered['gltfMaterialVariants']['undoCount'] == 0
               and recovered['gltfMaterialVariants']['redoCount'] == 0,
               f'Valid replacement kept the old variant history: {replacement}')
    log = Path(captures[0]['log'])
    diagnostic = 'glTF material variants: mesh 0 primitive 0 mapping 2: variant 0 is already mapped by this primitive'
    uv.require(any(malformed.name in line and diagnostic in line
                   for line in log.read_text(encoding='utf-8', errors='replace').splitlines()),
               f'Missing malformed asset and mapping diagnostic: {log}')
    for record, (frame, selection, appearance) in zip(captures, expected):
        check_classification(record, selection, appearance, mapped)
        reference = (reloaded_oracles if frame >= 7 else oracles)[appearance]
        uv.require(record['state']['gui']['selectedMeshNodeId'] == reference['state']['gui']['selectedMeshNodeId'],
                   f'Live reload and oracle selection differ at frame {frame}')
        error = pixels(record['image'], reference['image'])
        uv.require(error['maximumError'] == 0, f'Live reload pixels differ at frame {frame}: {error}')
        checks.append(dict(case='classification', technique=record['state']['technique'],
                           liveReload=True, frame=frame, selected=selection, **error))


def pixels(first, second):
    a = np.asarray(Image.open(first).convert('RGB'), dtype=np.float64)
    b = np.asarray(Image.open(second).convert('RGB'), dtype=np.float64)
    uv.require(a.shape == b.shape, 'Mismatched native image size')
    diff = np.abs(a-b)
    return dict(meanAbsoluteError=float(diff.mean()), maximumError=float(diff.max()),
                changedPixelFraction=float(np.any(diff > 0, axis=2).mean()))


def run(renderer, output, fixture, prefix, technique, display, records, *, sequence=None, save=None, timeout=180, ray=False, variant=None, gltf_scene=None, extra=(), require_validation=False):
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
    environment = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))
    if require_validation:
        environment['VK_LOADER_DEBUG'] = 'layer'
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=renderer.parent, stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=timeout, env=environment,
                                 creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    text = log.read_text(encoding='utf-8', errors='replace')
    uv.require(process.returncode == 0, f'Variant capture failed ({process.returncode}): {log}')
    uv.require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error', text, re.I), f'Vulkan error: {log}')
    if require_validation:
        uv.require('Insert instance layer "VK_LAYER_KHRONOS_validation"' in text,
                   f'Vulkan validation layer did not load: {log}')
    paths = sorted(output.glob(prefix+'*.telemetry.json'))
    uv.require(paths, f'No fresh capture telemetry: {log}')
    result = []
    for telemetry in paths:
        image_path = telemetry.with_suffix('').with_suffix('.png')
        state = json.loads(telemetry.read_text(encoding='utf-8'))
        uv.require(image_path.is_file() and uv.finite_json(state), f'Invalid capture {telemetry}')
        uv.require(state['technique'] == technique, f'Wrong technique: {telemetry}')
        uv.require(not require_validation or state['validationEnabled'], f'Validation was disabled: {telemetry}')
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

    def capture(fixture, prefix, technique, display, **options):
        if save := options.get('save'):
            save.unlink(missing_ok=True)
        return run(renderer, output, fixture, prefix, technique, display, report['captures'],
                   require_validation=True, **options)

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
                default = capture(paths['wrong-uv0'], name+'-default-oracle', technique, display)[0]
                alternate = capture(paths['analytical'], name+'-uv1-oracle', technique, display)[0]
                contrast = pixels(default['image'], alternate['image'])
                uv.require(contrast['meanAbsoluteError'] > .05, f'UV oracle is vacuous: {name}')
                saved = output / (name+'-saved.glb')
                captured = capture(paths['generated'], name+'-history', technique, display, sequence=sequence, save=saved)
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
                check_exported_variants(paths['generated'], saved, 0)
                reload = capture(saved, name+'-reload', technique, display)[0]
                state = reload['state']['gltfMaterialVariants']
                uv.require(state['selected'] == -1 and state['count'] == 1, 'Saved variant did not reload at Default with its named look')
                error = pixels(reload['image'], alternate['image'])
                uv.require(error['maximumError'] == 0, f'Saved selected appearance changed: {name}: {error}')
                report['checks'].append(dict(case=case, technique=technique, display=display, roundtrip=True, **error))
                if case == 'static' and technique == 'deferred-raster' and display == 'lit':
                    ascii_saved = output/(name+'-cli.gltf')
                    direct = capture(paths['generated'], name+'-cli', technique, display, variant=0, save=ascii_saved)[0]
                    uv.require(direct['state']['gltfMaterialVariants']['selected']==0, 'CLI variant selection was not applied')
                    error = pixels(direct['image'], alternate['image'])
                    uv.require(error['maximumError']==0, f'CLI selected appearance differs: {error}')
                    report['checks'].append(dict(case=case, cli=True, **error))
                    check_exported_variants(paths['generated'], ascii_saved, 0)
                    portable = capture(ascii_saved, name+'-ascii-reload', technique, display)[0]
                    state = portable['state']['gltfMaterialVariants']
                    uv.require(state['selected'] == -1 and state['count'] == 1, 'ASCII did not reload at Default with its named look')
                    error = pixels(portable['image'], alternate['image'])
                    uv.require(error['maximumError']==0, f'ASCII selected appearance differs: {error}')
                    report['checks'].append(dict(case=case, asciiRoundtrip=True, **error))
                report['nativeValidationPerformed'] = True
                persist()
                print('PASS', name, flush=True)
        if not args.skip_classification:
            source, oracle_paths = classification_fixtures(output)
            source_mapped = set(mapped_primitives(read_model(source)))
            samples = {1: -1, 3: 0, 5: -1, 7: 0, 9: 1, 11: -1, 13: 0}
            sequence = output/'classification-sequence.json'
            sequence.write_text(json.dumps(dict(schemaVersion=1, frames=13, sampleFrames=list(samples), events=[
                dict(frame=3, materialVariant=0), dict(frame=5, sceneHistory='undo'), dict(frame=7, sceneHistory='redo'),
                dict(frame=9, materialVariant=1), dict(frame=11, materialVariant=-1), dict(frame=13, materialVariant=0)
            ]), indent=2), encoding='utf-8')
            roundtrip_sequence = output/'classification-portable-sequence.json'
            roundtrip_sequence.write_text(json.dumps(dict(
                schemaVersion=1, frames=5, sampleFrames=[1, 3, 5],
                events=[dict(frame=3, materialVariant=1), dict(frame=5, materialVariant=-1)]),
                indent=2), encoding='utf-8')
            malformed, replacement, live_sequence, oracle_sequence = classification_reload_fixture(output, source, oracle_paths)
            report['cases'].append(dict(case='classification', source=str(source),
                                        oracles={k:str(v) for k,v in oracle_paths.items()}, sequence=str(sequence),
                                        roundtripSequence=str(roundtrip_sequence), liveReloadSequence=str(live_sequence),
                                        liveReloadOracleSequence=str(oracle_sequence),
                                        malformedSource=str(malformed), replacementSource=str(replacement)))
            if not args.generate_only:
                for technique in uv.TECHNIQUES:
                    prefix = f'classification-{technique}'
                    ray = technique == 'deferred-raster'
                    oracles = {name:capture(path, prefix+'-'+name+'-reference', technique, 'lit', ray=ray)[0]
                               for name, path in oracle_paths.items()}
                    for first, second in [('default', 'blend'), ('blend', 'mask'), ('default', 'mask')]:
                        uv.require(pixels(oracles[first]['image'], oracles[second]['image'])['meanAbsoluteError'] > .1,
                                   f'Classification oracles lack visible contrast: {prefix} {first} and {second}')
                    saved = output/(prefix+'-saved.glb')
                    captures = capture(source, prefix+'-history', technique, 'lit', sequence=sequence, save=saved, ray=ray)
                    uv.require(len(captures)==len(samples), 'Classification sequence capture count mismatch')
                    baseline_materials = None
                    previous_epoch = None
                    previous_ray_generation = None
                    previous_probe_epoch = None
                    for record, (frame, selection) in zip(captures, sorted(samples.items())):
                        appearance = CLASSIFICATION_LOOKS[selection]
                        unmapped = check_classification(record, selection, appearance, source_mapped)
                        baseline_materials = unmapped if baseline_materials is None else baseline_materials
                        uv.require(unmapped==baseline_materials, 'Unmapped primitive materials changed')
                        epoch = record['state']['taa']['epoch']
                        uv.require(previous_epoch is None or epoch>previous_epoch, 'Variant/history did not invalidate temporal epoch')
                        previous_epoch = epoch
                        if ray:
                            ray_state, gi = record['state']['rayShadows'], record['state']['ddgi']
                            uv.require(ray_state['supported'] and ray_state['active'] and gi['supported'] and gi['active']
                                       and gi['tracedRays'] > 0,
                                       f'Deferred classification fixture did not exercise ray queries and DDGI: {record["telemetry"]}')
                            uv.require(ray_state['build']['instances'] == 4, 'Ray scene lost shared primitive instances')
                            generation = ray_state['generation']
                            probe_epoch = gi['contentEpoch']
                            uv.require(previous_ray_generation is None or generation > previous_ray_generation,
                                       'Material variant/history did not refresh the ray scene')
                            uv.require(previous_probe_epoch is None or probe_epoch > previous_probe_epoch,
                                       'Material variant/history did not refresh DDGI probe contents')
                            previous_ray_generation = generation
                            previous_probe_epoch = probe_epoch
                        error = pixels(record['image'], oracles[appearance]['image'])
                        uv.require(error['maximumError']==0, f'Classification pixels differ: {prefix} frame{frame}: {error}')
                        report['checks'].append(dict(case='classification', technique=technique, frame=frame, selected=selection, **error))
                    exports = [saved]
                    if technique == 'deferred-raster':
                        ascii_saved = output/(prefix+'-saved.gltf')
                        direct = capture(source, prefix+'-ascii-save', technique, 'lit', ray=ray, variant=0, save=ascii_saved)[0]
                        check_classification(direct, 0, 'blend', source_mapped)
                        error = pixels(direct['image'], oracles['blend']['image'])
                        uv.require(error['maximumError'] == 0, f'ASCII export input differed from blend oracle: {error}')
                        report['checks'].append(dict(case='classification', technique=technique, asciiSave=True, **error))
                        exports.append(ascii_saved)
                    for portable in exports:
                        mapped = check_exported_variants(source, portable, 0)
                        format_name = portable.suffix[1:]
                        reloaded = capture(portable, prefix+'-'+format_name+'-reload', technique, 'lit',
                                           sequence=roundtrip_sequence, ray=ray)
                        uv.require(len(reloaded) == 3, f'Portable reload capture count mismatch: {portable}')
                        unmapped_baseline = None
                        for record, (frame, selection, appearance) in zip(reloaded, [(1, -1, 'blend'), (3, 1, 'mask'), (5, -1, 'blend')]):
                            unmapped = check_classification(record, selection, appearance, mapped)
                            uv.require(unmapped_baseline is None or unmapped == unmapped_baseline,
                                       f'Portable variant changed an unmapped primitive: {record["telemetry"]}')
                            unmapped_baseline = unmapped
                            error = pixels(record['image'], oracles[appearance]['image'])
                            uv.require(error['maximumError'] == 0, f'Saved classification pixels differ: {portable} frame {frame}: {error}')
                            report['checks'].append(dict(case='classification', technique=technique, roundtrip=True,
                                                         format=format_name, frame=frame, selected=selection, **error))
                    if technique == 'deferred-raster':
                        reloaded = capture(oracle_paths['default'], prefix+'-live-oracles', technique, 'lit',
                                           sequence=oracle_sequence, ray=ray)
                        uv.require(len(reloaded) == 2, 'Live reload oracle capture count mismatch')
                        reloaded_oracles = dict(zip(('default', 'mask'), reloaded))
                        for appearance, record in reloaded_oracles.items():
                            uv.require(Path(record['state']['scene']['primary']).resolve() == oracle_paths[appearance],
                                       f'Live reload did not install the independent {appearance} oracle')
                        live = capture(source, prefix+'-live-reload', technique, 'lit', sequence=live_sequence, ray=ray)
                        check_live_reload(live, source_mapped, malformed, replacement, oracles, reloaded_oracles, report['checks'])
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
