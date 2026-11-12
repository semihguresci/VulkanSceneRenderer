"""Exercise viewport GPU selection across glTF merge, history, and portable reload."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, load_asset, require
from gltf_compression_runtime_regression import write_glb
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_material_variants_runtime_regression import pixels, run
from gltf_merge_runtime_regression import (
    COLORS, TECHNIQUES, TRANSFORM, UNLIT, add_quad, add_resource, document,
    image_pixels, placement, portable_copy, sha, write_asset,
)


NAMES = {'A': 'Original A', 'B': 'Incoming B'}
PICK = {'A': [239, 180], 'B': [401, 180]}
PARENT = 'B placement'
LOCAL = [[-.375, -.4375, 0], [.375, -.4375, 0], [.375, .4375, 0], [-.375, .4375, 0]]
# Independently authored world vertices for the two control scenes.
BAKED = {
    'A': [[-1.125, -.4375, 0], [-.375, -.4375, 0], [-.375, .4375, 0], [-1.125, .4375, 0]],
    'B': [[.375, -.4375, 0], [1.125, -.4375, 0], [1.125, .4375, 0], [.375, .4375, 0]],
}
CONTROL_SAMPLES = ((3, 'A'), (6, 'B'))
LIVE_SAMPLES = ((3, 'A'), (11, 'A'), (15, 'B'), (22, 'B'), (30, 'B'), (35, 'A'))
SOURCE_TABLES = ('meshes', 'materials', 'images', 'textures', 'samplers', 'buffers')
NO_SELECTION = 0xffffffff


def source(path, origin, translation):
    asset = Asset()
    material = add_resource(asset, path.parent, origin, image_name='texture.png')
    mesh = add_quad(asset, LOCAL, material, NAMES[origin], origin=origin)
    model = asset.model
    model['asset'].update(copyright=f'{origin} copyright', extras={'document': origin, 'assetData': [1, False]})
    model['extras'] = {'document': origin, 'rootData': {'texture': 0, 'label': NAMES[origin]}}
    model['nodes'] = [dict(name=NAMES[origin], mesh=mesh, translation=[translation, 0, 0])]
    if path.suffix == '.glb':
        model['asset']['generator'] = 'Independent incoming GLB'
        model['extensionsUsed'] = [UNLIT, TRANSFORM]
        model['buffers'] = [dict(byteLength=len(asset.data))]
        write_glb(path, model, bytes(asset.data))
    else:
        write_asset(path, asset)
    return model


def control(path, origins):
    asset = Asset()
    nodes = []
    for origin in origins:
        material = add_resource(asset, path.parent, origin, image_name=f'{origin}-texture.png')
        mesh = add_quad(asset, BAKED[origin], material, NAMES[origin], origin=origin)
        nodes.append(dict(name=NAMES[origin], mesh=mesh))
    asset.model['nodes'] = nodes
    asset.model['scenes'][0]['nodes'] = list(range(len(nodes)))
    write_asset(path, asset)


def sequence(path, samples, events=(), *, probe_only=()):
    events = list(events)
    for frame, origin in samples:
        pixel = PICK[origin]
        if frame not in probe_only:
            events.append(dict(frame=frame - 2, viewportPointer=dict(action='select', pixel=pixel)))
        events.append(dict(frame=frame, depthProbe=pixel))
    document(path, dict(schemaVersion=1, frames=samples[-1][0],
                        sampleFrames=[frame for frame, _ in samples],
                        events=sorted(events, key=lambda event: event['frame'])))


def one(rows, key, value, description):
    matches = [row for row in rows if row[key] == value]
    require(len(matches) == 1, f'Expected one {description} with {key}={value!r}, found {len(matches)}')
    return matches[0]


def check_pick(row, origin, *, hit_expected=True, placement_expected=False, exported=False):
    state = row['state']
    probe = state['depthProbe']
    interaction = probe['sceneInteraction']
    raw_id = interaction['rawGpuId']
    gpu, cpu = interaction['gpu'], interaction['cpu']
    selected = state['gui']['selectedMeshNodeId']
    location = row['telemetry']
    require(probe['pixel'] == PICK[origin] and type(raw_id) is int and gpu is not None,
            f'Missing GPU readback at {PICK[origin]} in {location}')
    hit = gpu['hit']
    if not hit_expected:
        require(raw_id == 0 and all(route[kind] is None for route in (gpu, cpu)
                                   for kind in ('hit', 'selection', 'hover')) and selected == NO_SELECTION,
                f'Empty viewport retained a GPU or GUI selection in {location}')
        return dict(pixel=PICK[origin], rawGpuId=raw_id, selection=None)

    require(raw_id > 0 and probe['opaque'] > 0 and hit is not None and
            hit == cpu['hit'] == gpu['selection'] == cpu['selection'] and selected == hit['graphNode'],
            f'Viewport selection disagrees with rendered GPU and CPU hits in {location}')
    require(hit['sourceNode'] >= 0 and hit['sourceMesh'] >= 0 and
            hit['sourcePrimitive'] == 0 and hit['sourceInstance'] == -1,
            f'Viewport selected the wrong source primitive in {location}: {hit}')

    flags = state['gltfNodeFlags']
    require(not flags['truncated'], f'Incomplete node ancestry in {location}')
    nodes = {node['graphNode']: node for node in flags['nodes']}
    ancestry = []
    current = hit['graphNode']
    while current != -1:
        node = nodes[current]
        ancestry.append(node['name'])
        current = node['parentGraphNode']
    # The portable exporter writes a mesh leaf under the authored node, so reload can report the leaf name.
    require(hit['name'] in (ancestry if exported else (NAMES[origin],)) and NAMES[origin] in ancestry and
            NAMES['B' if origin == 'A' else 'A'] not in ancestry and
            (PARENT in ancestry) == placement_expected, f'Incorrect viewport ancestry in {location}: {ancestry}')

    bindings = state['gltfMaterialBindings']
    require(not bindings['drawsTruncated'] and not bindings['materialsTruncated'],
            f'Incomplete material routing in {location}')
    draw = one(bindings['draws'], 'graphNode', hit['graphNode'], 'draw')
    material = one(bindings['materials'], 'runtimeMaterial', draw['runtimeMaterial'], 'material')
    texture_index = material['authored']['pbrMetallicRoughness']['baseColorTexture']['index']
    require(material['active'] and material['authored']['extras']['fixtureOrigin'] == origin,
            f'Viewport draw selected the wrong material in {location}')
    resources = state['gltfMaterialTextures']
    require(not resources['resourcesTruncated'], f'Incomplete texture routing in {location}')
    texture = one(resources['textures'], 'sourceTexture', texture_index, 'texture')
    slots = [slot for slot in resources['slots'] if slot['runtimeMaterial'] == draw['runtimeMaterial']]
    slot = one(slots, 'slot', 'baseColor', 'material slot')
    require(slot['active'] and slot['authoredTexture'] == texture_index and
            slot['evaluatedTexture'] == texture['evaluatedTexture'] and slot['runtimeTexture'] is not None and
            slot['texCoord'] == (0 if origin == 'A' else 1) and slot['srgb'] and
            texture['wrapS'] == (33071 if origin == 'A' else 33648),
            f'Viewport draw resolved the wrong image/sampler route in {location}')
    return dict(**hit, pixel=PICK[origin], rawGpuId=raw_id, ancestry=ancestry, firstIndex=draw['firstIndex'],
                sourceMaterial=material['sourceMaterial'], runtimeMaterial=draw['runtimeMaterial'],
                sourceTexture=texture_index, image=texture['image'], sampler=texture['sampler'],
                runtimeTexture=slot['runtimeTexture'])


def check_history(state, initial, publications, reads, undo, redo):
    merge, baseline = state['gltfMerge'], initial['gltfMerge']
    require(merge['requestId'] - baseline['requestId'] == reads and
            merge['sourceReadCount'] - baseline['sourceReadCount'] == reads and
            merge['generationPublications'] - baseline['generationPublications'] == publications and
            merge['commitGeneration'] - baseline['commitGeneration'] == publications and
            merge['descriptorGeneration'] == merge['commitGeneration'] and
            merge['outcome'] == ('committed' if reads else 'idle') and not merge['error'],
            f'Merge history published a wrong generation or re-read the incoming asset: {merge}')
    cursor = state.get('sceneEditHistory')
    # Headless mode creates the edit journal on the first merge request.
    if cursor is None and reads == 0:
        cursor = dict(undo=0, redo=0)
    require(merge['graphIdentity'] == baseline['graphIdentity'] and cursor == dict(undo=undo, redo=redo) and
            all(merge['sourceTables'][kind] == 1 + undo for kind in SOURCE_TABLES),
            f'Merge history changed graph identity, cursor, or resource membership: {merge}')
    fields = ('requestId', 'sourceReadCount', 'generationPublications', 'commitGeneration',
              'descriptorGeneration', 'graphIdentity')
    return dict(**{key: merge[key] for key in fields}, history=cursor)


def compare(row, oracle):
    error = pixels(row['image'], oracle['image'])
    require(error['maximumError'] == 0, f'{row["image"]} differs from {oracle["image"]}: {error}')
    return dict(reference=oracle['image'], **error)


def surface_palette(row, origin):
    x, y = PICK[origin]
    with Image.open(row['image']) as image:
        # Stay inside the quad, away from the selection outline.
        patch = np.asarray(image.convert('RGB').crop((x - 30, y - 35, x + 30, y + 35)))
    palette = {tuple(color) for color in np.unique(patch.reshape(-1, 3), axis=0)}
    require(len(palette) >= 2, f'{origin} control has no texture variation: {row["image"]}')
    return palette


def check_export(path, primary, incoming):
    model, buffers = load_asset(path)
    require(model['extras'] == primary['extras'] and all(model['asset'][key] == primary['asset'][key]
            for key in ('copyright', 'extras')), f'Export changed primary document provenance: {path}')
    parent = one(model['nodes'], 'name', PARENT, 'placement')
    provenance = parent['extras']['containerMergeSource']
    require(provenance == dict(asset=incoming['asset'], extras=incoming['extras']),
            f'Export changed incoming document provenance: {path}')
    for origin in NAMES:
        material = next(m for m in model['materials'] if m.get('extras', {}).get('fixtureOrigin') == origin)
        texture = model['textures'][material['pbrMetallicRoughness']['baseColorTexture']['index']]
        require(np.array_equal(image_pixels(path, model, buffers, texture['source']), COLORS[origin]),
                f'Export changed {origin} texture payload: {path}')
    return provenance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    techniques = args.technique or TECHNIQUES
    require(len(set(techniques)) == len(techniques), 'Duplicate raster technique')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = dict(schemaVersion=1, passed=False, nativeValidationPerformed=False,
                  generatedOnly=args.generate_only, expectedCaptureCount=12 * len(techniques),
                  expectedCheckCount=12 * len(techniques), captures=[], checks=[], portable={})

    try:
        primary_path = output / 'sources' / 'A' / 'scene.gltf'
        incoming_path = output / 'sources' / 'B' / 'scene.glb'
        primary, incoming = source(primary_path, 'A', -.75), source(incoming_path, 'B', .125)
        single_path = output / 'controls' / 'single' / 'scene.gltf'
        baked_path = output / 'controls' / 'merged' / 'scene.gltf'
        control(single_path, ('A',))
        control(baked_path, ('A', 'B'))
        controls, history = output / 'control-picks.json', output / 'merge-history-picks.json'
        sequence(controls, CONTROL_SAMPLES)
        sequence(history, LIVE_SAMPLES, (
            # The click before merge saves an empty selection. Probe after Undo without clicking again.
            dict(frame=4, viewportPointer=dict(action='select', pixel=PICK['B'])),
            dict(frame=5, guiWaitForLoad=True, merge=dict(path=str(incoming_path), parentName=PARENT,
                 incomingScene=0, placementMatrix=placement([.625, 0, 0]))),
            dict(frame=17, sceneHistory='undo'), dict(frame=24, sceneHistory='redo'),
        ), probe_only=(22,))
        report['fixtures'] = dict(primary=str(primary_path), incoming=str(incoming_path),
                                  singleControl=str(single_path), mergedControl=str(baked_path),
                                  controlSequence=str(controls), historySequence=str(history))
        if args.generate_only:
            report['passed'] = None
            return

        require(args.renderer is not None and args.manifest is not None, 'Native execution requires renderer and manifest')
        renderer = args.renderer.resolve()
        require(renderer.is_file(), f'Missing renderer: {renderer}')
        manifest = args.manifest.resolve()
        report.update(renderer=str(renderer), rendererSha256=sha(renderer), nativeFreeze=verify_runtime_manifest(manifest, renderer))
        (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')

        def capture(technique, fixture, label, sequence_path, count, *, save=None):
            rows = run(renderer, output, fixture, f'{technique}-{label}', technique, 'lit', report['captures'],
                       sequence=sequence_path, save=save, require_validation=True,
                       extra=('--import-scale', '1', '--sky', 'plain', '--no-ray-query', '--ray-shadows', 'raster'))
            require(len(rows) == count, f'Wrong number of {technique} {label} samples')
            return rows

        def verify(technique, label, row, origin, *, hit=True, placed=False, exported=False,
                   oracle=None, history_check=None):
            pick = check_pick(row, origin, hit_expected=hit, placement_expected=placed, exported=exported)
            check = dict(technique=technique, sample=label, telemetry=row['telemetry'], pick=pick)
            if oracle is not None:
                check['pixels'] = compare(row, oracle)
            if history_check is not None:
                check['merge'] = history_check
            report['checks'].append(check)
            return pick

        source_folder, export_folder = output / 'sources', output / 'exports'
        source_hashes = {path.relative_to(source_folder): sha(path) for path in source_folder.rglob('*') if path.is_file()}
        portable = {}
        merged_controls = {}
        for technique in techniques:
            single = capture(technique, single_path, 'single-control', controls, 2)
            baked = capture(technique, baked_path, 'merged-control', controls, 2)
            merged_controls[technique] = baked
            for idx, origin in enumerate(NAMES):
                verify(technique, f'single-{origin}', single[idx], origin, hit=origin == 'A')
                verify(technique, f'baked-{origin}', baked[idx], origin)
            require(surface_palette(baked[0], 'A').isdisjoint(surface_palette(baked[1], 'B')),
                    f'Textured control surfaces share a rendered palette in {technique}')

            suffix = 'gltf' if technique == 'deferred-raster' else 'glb'
            export = export_folder / suffix / f'merged.{suffix}'
            export.parent.mkdir(parents=True)
            live = capture(technique, primary_path, 'live-history', history, len(LIVE_SAMPLES), save=export)
            picks = []
            schedule = (
                ('original-A', single[0], 0, 0, 0, 0), ('merged-A', baked[0], 1, 1, 1, 0),
                ('merged-B', baked[1], 1, 1, 1, 0), ('undo-B', single[1], 2, 1, 0, 1),
                ('redo-B', baked[1], 3, 1, 1, 0), ('redo-A', baked[0], 3, 1, 1, 0),
            )
            for row, (_, origin), (label, oracle, *history_state) in zip(live, LIVE_SAMPLES, schedule, strict=True):
                merge = check_history(row['state'], live[0]['state'], *history_state)
                picks.append(verify(technique, label, row, origin, hit=label != 'undo-B',
                                    placed=label in ('merged-B', 'redo-B'), oracle=oracle, history_check=merge))
            # GPU IDs are slots in the current draw packet. Compare source and graph identity across compositions.
            retained = [{key: value for key, value in pick.items() if key != 'rawGpuId'} for pick in (picks[0], picks[1])]
            require(retained[0] == retained[1] and picks[1] == picks[5] and picks[2] == picks[4],
                    f'Merge/history changed a retained or restored GPU pick identity in {technique}')
            for key in ('rawGpuId', 'graphNode', 'sourceNode', 'sourceMesh', 'firstIndex', 'sourceMaterial',
                        'runtimeMaterial', 'sourceTexture', 'image', 'sampler', 'runtimeTexture'):
                require(picks[1][key] != picks[2][key], f'Merged surfaces alias {key} in {technique}')

            copied, files = portable_copy(export, output / 'portable' / suffix)
            portable[technique] = copied
            report['portable'][technique] = dict(format=suffix, files=files,
                                                  incomingProvenance=check_export(copied, primary, incoming))

        offline_sources, offline_exports = output / 'offline-sources', output / 'offline-exports'
        source_folder.rename(offline_sources)
        export_folder.rename(offline_exports)
        require(not source_folder.exists() and not export_folder.exists() and
                all(sha(offline_sources / path) == digest for path, digest in source_hashes.items()),
                'Source-free reload failed to isolate the unchanged source tree and original export directory')
        report['offlineSourceFiles'] = {str(offline_sources / path): digest for path, digest in source_hashes.items()}
        for technique in techniques:
            rows = capture(technique, portable[technique], 'portable-reload', controls, 2)
            for row, oracle, origin in zip(rows, merged_controls[technique], NAMES, strict=True):
                verify(technique, f'portable-{origin}', row, origin, placed=origin == 'B', exported=True, oracle=oracle)

        require(report['nativeFreeze'] == verify_runtime_manifest(manifest, renderer) and
                report['rendererSha256'] == sha(renderer), 'Frozen native input changed during validation')
        require(len(report['captures']) == report['expectedCaptureCount'] and
                len(report['checks']) == report['expectedCheckCount'], 'Incomplete GPU pick capture matrix')
        report.update(passed=True, nativeValidationPerformed=True)
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
