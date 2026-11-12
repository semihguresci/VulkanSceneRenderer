"""Verify Save Linked As preserves inline edits and primary animation with all links live."""

import argparse
import hashlib
from pathlib import Path

import numpy as np

from gltf_accessor_runtime_regression import Asset, TECHNIQUES, load_asset, mesh, require
from gltf_compression_runtime_regression import write_glb
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_linked_assets_runtime_regression import (
    BIND_POSITIONS, EDITABLE_PLACEMENTS, editable_fixture, node_authoring, pixels, write_json,
)
from gltf_material_variants_runtime_regression import run


PANEL = 'Inline panel'
FOLDER = 'Inline folder'
CLIP = 'Inline lift'
EDITED_COLOR = [.25, .62, .94, 1.]
OPTIONS = ('--experimental-gltf-linked-assets', '--import-scale', '1', '--sky', 'plain')


def fixture(output, animated):
    primary = editable_fixture(output, animated=animated)
    original, _ = load_asset(primary)
    asset = Asset()
    mesh(asset, asset.accessor('VEC3', BIND_POSITIONS), asset.accessor('VEC3', [[0, 0, 1]] * 4),
         asset.accessor('VEC2', [[0, 0], [1, 0], [1, 1], [0, 1]]))
    asset.model.update(original)
    asset.model['extensionsUsed'] = ['KHR_materials_unlit']
    asset.model['materials'] = [dict(name='Inline paint', extensions={'KHR_materials_unlit': {}},
                                     pbrMetallicRoughness=dict(baseColorFactor=[.8, .4, .05, 1.]))]
    inline = len(asset.model['nodes'])
    asset.model['nodes'].append(dict(name=PANEL, mesh=0, translation=[0, .875, 0]))
    asset.model['scenes'][0]['nodes'].append(inline)
    if animated:
        times = asset.accessor('SCALAR', [0, 1])
        positions = asset.accessor('VEC3', [[0, .875, 0], [.5, 1.25, 0]])
        asset.model['animations'] = [dict(name=CLIP, samplers=[dict(input=times, output=positions)],
                                         channels=[dict(sampler=0, target=dict(node=inline, path='translation'))])]
        # Span two verification read blocks before the original GLB's BIN chunk.
        asset.model['asset']['extras'] = {'description': 'x' * (70 * 1024)}
        primary = primary.with_suffix('.glb')
        asset.model['buffers'] = [dict(byteLength=len(asset.data))]
        write_glb(primary, asset.model, bytes(asset.data))
    else:
        asset.write(primary)
    return primary, inline


def link_bindings(model):
    files, assets = model['files'], model['externalAssets']
    bindings = {}
    for node in model['nodes']:
        if 'externalAsset' not in node:
            continue
        file = files[assets[node['externalAsset']]['file']]
        aliases = {alias['alias']: files[alias['file']]['uri'] for alias in file.get('aliases', [])}
        bindings[node['name']] = (file['uri'], aliases)
    return bindings


def verify_companion(source, companion, animated):
    original, _ = load_asset(source)
    saved, _ = load_asset(companion)
    require(saved['asset']['version'] == '2.1' and saved['asset']['minVersion'] == '2.1',
            'Linked companion lost its draft version declaration')
    bindings = link_bindings(saved)
    require(set(bindings) == set(EDITABLE_PLACEMENTS) and bindings == link_bindings(original),
            f'Linked companion changed the live file and alias bindings: {bindings}')
    panel, = (node for node in saved['nodes'] if node.get('name') == PANEL)
    if animated:
        clip, = saved['animations']
        channel, = clip['channels']
        target = channel['target']
        require(clip['name'] == CLIP and target['path'] == 'translation' and saved['nodes'][target['node']] is panel,
                f'Primary clip no longer targets the inline panel: {clip}')
    else:
        require(any(saved['nodes'][child].get('name') == FOLDER for child in panel['children']),
                'Linked companion omitted the new inline child')
        material, = (material for material in saved['materials'] if material.get('name') == 'Inline paint')
        np.testing.assert_allclose(material['pbrMetallicRoughness']['baseColorFactor'], EDITED_COLOR, atol=1e-6)


def verify_live(record, selected, clip_count):
    state = record['state']
    require(state['scene']['primaryObjectCount'] == 4, 'A live linked or inline draw is missing')
    nodes = [node for node in state['gltfNodeFlags']['nodes'] if node['active'] and not node['retired']]
    panel, = (node for node in nodes if node['name'] == PANEL)
    require(not panel['linkedReadOnly'], 'The primary inline panel became read-only')
    linked_panels = []
    for name, payload in zip(EDITABLE_PLACEMENTS, ('Panel A', 'Panel A', 'Panel B')):
        carrier, = (node for node in nodes if node['name'] == name)
        child, = (node for node in nodes if node['name'] == payload and node['parentGraphNode'] == carrier['graphNode'])
        require(not carrier['linkedReadOnly'] and child['linkedReadOnly'], f'Linked ownership changed for {name}')
        linked_panels.append(child)
    if probe := state.get('depthProbe'):
        interaction = probe['sceneInteraction']
        hit = interaction['gpu']['selection']
        require(hit is not None and interaction['rawGpuId'] > 0 and hit['sourceNode'] == linked_panels[0]['sourceIndex'] and
                hit['graphNode'] == state['gui']['selectedMeshNodeId'], 'The comparison selected a different linked surface')
    animation = state['gltfAnimation']
    if clip_count:
        require(animation['clipCount'] == clip_count and animation['selectedClip'] == selected and
                animation['diagnostics'] == 0 and not animation['playing'], f'Unexpected playback state: {animation}')
        if selected == 0:
            target, = animation['nodes']
            require(target['sourceIndex'] == panel['sourceIndex'], 'The primary clip no longer samples its inline node')
            np.testing.assert_allclose(target['translation'], [.375, 1.15625, 0], atol=1e-6)
    return pixels(Path(record['image']))


def capture_case(renderer, output, primary, inline, animated, records):
    companion = primary.with_name('companion.gltf')
    source_inputs = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in primary.parent.iterdir()}
    if animated:
        samples, selected = [2, 4, 6, 8], [-1, 0, 2, -1]
        events = [dict(frame=3, animationClip=0, animationTime=.75, animationPlaying=False),
                  dict(frame=5, animationClip=2, animationTime=.75), dict(frame=7, animationClip=-1)]
        restore_events = events
    else:
        samples, selected = [1, 6], [-1, -1]
        # Create Empty selects the new folder. Match the reopened scene's
        # selection so its outline does not change the image comparison.
        pixel = [168, 180]
        restore_events = [dict(frame=4, viewportPointer=dict(action='select', pixel=pixel)),
                          dict(frame=6, depthProbe=pixel)]
        events = [node_authoring(2, 'create-empty', inline, FOLDER),
                  dict(frame=3, materialEdit=dict(sourceMaterial=0, values=[
                      dict(property='pbrMetallicRoughness/baseColorFactor', value=EDITED_COLOR)])), *restore_events]
    sequence = output / 'live.json'
    restored_sequence = output / 'restored.json'
    write_json(sequence, dict(schemaVersion=1, frames=samples[-1], sampleFrames=samples, events=events))
    write_json(restored_sequence, dict(schemaVersion=1, frames=samples[-1],
                                      sampleFrames=samples if animated else samples[-1:], events=restore_events))
    for technique in TECHNIQUES:
        first = not companion.exists()
        extra = (*OPTIONS, '--save-linked-scene', str(companion)) if first else OPTIONS
        live = run(renderer, output, primary, f'live-{technique}', technique, 'lit', records,
                   sequence=sequence, extra=extra, require_validation=True)
        if first:
            verify_companion(primary, companion, animated)
        restored = run(renderer, output, companion, f'restored-{technique}', technique, 'lit', records,
                       sequence=restored_sequence, extra=OPTIONS, require_validation=True)
        require(len(live) == len(samples) and len(restored) == (len(samples) if animated else 1),
                'Incomplete source or companion captures')
        clip_count = 3 if animated else 0
        live_images = [verify_live(record, clip, clip_count) for record, clip in zip(live, selected)]
        restored_images = [verify_live(record, clip, clip_count) for record, clip in zip(restored, selected[-len(restored):])]
        require(not np.array_equal(live_images[0], live_images[1]), 'The inline edit or primary pose did not affect pixels')
        if animated:
            require(not np.array_equal(live_images[0], live_images[2]), 'The retained child clip did not affect pixels')
            np.testing.assert_array_equal(live_images[0], live_images[-1])
        for actual, expected in zip(restored_images, live_images[-len(restored_images):]):
            np.testing.assert_array_equal(actual, expected)
    require(all(hashlib.sha256(path.read_bytes()).hexdigest() == digest for path, digest in source_inputs.items()),
            'Linked Save As modified an original input')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    cases = []
    for label, animated in (('edited', False), ('animated', True)):
        directory = output / label
        directory.mkdir()
        cases.append((directory, *fixture(directory, animated), animated))
    if args.generate_only:
        return
    require(args.renderer is not None and args.manifest is not None, 'Native run requires --renderer and --manifest')
    renderer = args.renderer.resolve(strict=True)
    manifest = verify_runtime_manifest(args.manifest, renderer)
    records = []
    for directory, primary, inline, animated in cases:
        (directory / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
        capture_case(renderer, directory, primary, inline, animated, records)
    require(len(records) == 22 and verify_runtime_manifest(args.manifest, renderer) == manifest,
            'Capture matrix or runtime manifest changed')
    write_json(output / 'results.json', dict(passed=True, rendererSha256=hashlib.sha256(renderer.read_bytes()).hexdigest(),
                                             captures=records))
    print('PASS all-live linked companions retain inline edits, clips and source ownership')


if __name__ == '__main__':
    main()
