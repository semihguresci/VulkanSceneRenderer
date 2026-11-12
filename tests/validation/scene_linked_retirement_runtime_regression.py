#!/usr/bin/env python3
"""Exercise protected linked retirement, detachment, history, and portable save."""

import argparse
import json
from pathlib import Path

import numpy as np

from gltf_accessor_runtime_regression import finite_json, load_asset, require
from gltf_extended_materials_runtime_regression import validation_log_error
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_linked_assets_runtime_regression import fixture, node_authoring, pixels, write_json
from gltf_material_variants_runtime_regression import run
from gltf_merge_runtime_regression import sha


TECHNIQUES = ('deferred-raster', 'forward-raster')
COMMON = ('--import-scale', '1', '--sky', 'plain', '--no-ray-query', '--ray-shadows', 'raster')
LINKED = (*COMMON, '--experimental-gltf-linked-assets')
NO_SELECTION = 0xffffffff


def node_tree(state, name):
    graph = state['gltfMerge']
    require(not graph['authoringNodesTruncated'], 'Incomplete graph witness')
    nodes = graph['authoringNodes']
    root, = (node for node in nodes if node['name'] == name)
    branch = [root]
    for parent in branch:
        branch.extend(node for node in nodes if node['parentGraphNode'] == parent['graphNode'])
    require(len(branch) == 5 and sum(node['renderable'] for node in branch) == 1,
            f'Wrong linked placement closure: {branch}')
    return branch


def state_check(state, initial, *, retired, detached, undo, redo):
    require(state['gltfMerge']['graphIdentity'] == initial['gltfMerge']['graphIdentity'] and
            state['gltfScenes']['activeRenderableCount'] == 2 - int(retired),
            'Linked retirement changed graph identity or the wrong draw membership')
    history = state.get('sceneEditHistory', dict(undo=0, redo=0))
    require(history == dict(undo=undo, redo=redo), f'Wrong retirement history: {history}')
    flags = state['gltfNodeFlags']
    require(not flags['truncated'], 'Incomplete linked ownership witness')
    by_graph = {row['graphNode']: row for row in flags['nodes']}
    for placement in range(2):
        name = f'Placement {placement}'
        branch = node_tree(state, name)
        removed = placement == 0 and retired
        baseline = node_tree(initial, name)
        keys = ('graphNode', 'sourceNode', 'parentGraphNode', 'meshOwnerGraphNode', 'localColumns')
        require(all(all(node[key] == original[key] for key in keys)
                    for node, original in zip(branch, baseline, strict=True)) and
                all(node['active'] == (not removed) and node['retired'] == removed for node in branch),
                f'Linked retirement changed stable slots or active ancestry: {branch}')
        expected = [node['graphNode'] for node in baseline]
        require(not by_graph[expected[0]]['linkedReadOnly'] and
                all(by_graph[node]['linkedReadOnly'] == (not detached) for node in expected[1:]),
                f'Wrong linked ownership for {name}')


def validate_log(path):
    log = path.read_text(encoding='utf-8', errors='replace')
    require(not validation_log_error(log) and
            'Insert instance layer "VK_LAYER_KHRONOS_validation"' in log and
            'Inserted device layer "VK_LAYER_KHRONOS_validation"' in log, f'Invalid Vulkan log: {path}')
    return log


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--technique', choices=TECHNIQUES, action='append')
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    techniques = args.technique or TECHNIQUES
    require(len(set(techniques)) == len(techniques), 'Duplicate raster technique')
    output = args.output.resolve()
    output.mkdir(parents=True)
    report = dict(schemaVersion=1, passed=False, generatedOnly=args.generate_only, nativeValidationPerformed=False,
                  captures=[], rejected=[], checks=[])
    try:
        primary, _ = fixture(output)
        live = output / 'retirement.json'
        reject = output / 'protected-retirement.json'
        reload = output / 'portable-reload.json'
        write_json(live, dict(schemaVersion=1, frames=14, sampleFrames=[2, 5, 8, 11, 14], events=[
            node_authoring(4, 'make-editable', 0), node_authoring(7, 'delete-subtree', 0),
            dict(frame=10, sceneHistory='undo'), dict(frame=13, sceneHistory='redo')]))
        write_json(reject, dict(schemaVersion=1, frames=5, sampleFrames=[2, 5], events=[
            node_authoring(4, 'delete-subtree', 0)]))
        # Startup selects the first mesh. Match the vacant selection after deletion before comparing pixels.
        write_json(reload, dict(schemaVersion=1, frames=5, sampleFrames=[5], events=[
            dict(frame=2, viewportPointer=dict(action='select', pixel=[40, 180]))]))
        if args.generate_only:
            report['passed'] = True
            return

        require(args.renderer is not None and args.manifest is not None, 'Native validation requires renderer and manifest')
        renderer, manifest = args.renderer.resolve(), args.manifest.resolve()
        report.update(rendererSha256=sha(renderer), nativeFreeze=verify_runtime_manifest(manifest, renderer))
        (output / 'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
        companion = primary.with_name('existing-companion.gltf')
        companion.write_bytes(b'Existing companion contents\n')
        source_hashes = {path.relative_to(primary.parent): sha(path)
                         for path in primary.parent.rglob('*') if path.is_file()}
        saved = {}
        for technique in techniques:
            prefix = f'{technique}-protected'
            rejected_log = output / f'{prefix}.log'
            try:
                run(renderer, output, primary, prefix, technique, 'lit', report['captures'], sequence=reject,
                    extra=(*LINKED, '--save-linked-scene', str(companion)), require_validation=True)
            except RuntimeError as error:
                require(str(error).startswith('Variant capture failed ('), f'Unexpected rejection: {error}')
                log = validate_log(rejected_log)
                require('linked asset is read-only' in log and 'Use Make Editable first' in log and
                        'A retained linked placement has no export node' not in log,
                        f'Linked carrier deletion did not reject before save: {rejected_log}')
            else:
                raise RuntimeError(f'Linked carrier retirement was accepted: {rejected_log}')
            rejected_telemetry, = output.glob(f'{prefix}*.telemetry.json')
            rejected_image = rejected_telemetry.with_suffix('').with_suffix('.png')
            rejected_state = json.loads(rejected_telemetry.read_text(encoding='utf-8'))
            require(rejected_state['validationEnabled'] and rejected_state['technique'] == technique and finite_json(rejected_state),
                    f'Invalid pre-rejection capture: {rejected_telemetry}')
            state_check(rejected_state, rejected_state, retired=False, detached=False, undo=0, redo=0)
            require(all(sha(primary.parent / path) == digest for path, digest in source_hashes.items()),
                    'Rejected retirement changed a linked source or the existing companion')
            report['rejected'].append(dict(technique=technique, telemetry=str(rejected_telemetry),
                                           image=str(rejected_image), log=str(rejected_log)))

            suffix = '.gltf' if technique == 'deferred-raster' else '.glb'
            portable = output / f'{technique}-portable{suffix}'
            before, detached, deleted, undone, redone = run(renderer, output, primary, f'{technique}-live',
                technique, 'lit', report['captures'], sequence=live, save=portable, extra=LINKED, require_validation=True)
            for row, (retired, editable, undo, redo) in zip((before, detached, deleted, undone, redone), (
                    (False, False, 0, 0), (False, True, 0, 0), (True, True, 1, 0), (False, True, 0, 1), (True, True, 1, 0)), strict=True):
                state_check(row['state'], before['state'], retired=retired, detached=editable, undo=undo, redo=redo)
            baseline, unlocked, removed, restored, repeated = (pixels(row['image'])
                for row in (before, detached, deleted, undone, redone))
            require(np.array_equal(baseline, pixels(rejected_image)) and np.array_equal(baseline, unlocked) and
                    np.array_equal(baseline, restored) and np.array_equal(removed, repeated),
                    'Detachment or history changed restored pixels')
            midpoint = baseline.shape[1] // 2
            require(np.array_equal(baseline[:, midpoint:], removed[:, midpoint:]) and
                    np.count_nonzero(np.any(baseline != removed, axis=2)) > baseline.shape[0] * baseline.shape[1] * .005,
                    'Deleting one placement changed its sibling or failed to remove visible geometry')
            model, _ = load_asset(portable)
            names = [node.get('name') for node in model['nodes']]
            require('Placement 0' not in names and names.count('Placement 1') == 1 and 'files' not in model and
                    'externalAssets' not in model, 'Portable save retained the retired placement or source links')
            saved[technique] = (portable, redone)
            report['checks'].append(dict(technique=technique, rejectedBeforeSave=True, stableSlots=True,
                                         siblingPixelsExact=True, historyPixelsExact=True, exportCompacted=True))

        source_folder = primary.parent
        offline = output / 'offline-linked-inputs'
        source_folder.rename(offline)
        require(all(sha(offline / path) == digest for path, digest in source_hashes.items()),
                'Portable export changed the original linked files')
        for technique, (portable, reference) in saved.items():
            reopened, = run(renderer, output, portable, f'{technique}-reopened', technique, 'lit',
                            report['captures'], sequence=reload, extra=COMMON, require_validation=True)
            require(reopened['state']['gui']['selectedMeshNodeId'] ==
                    reference['state']['gui']['selectedMeshNodeId'] == NO_SELECTION,
                    'Reload and retirement must have matching vacant viewport selections')
            require(reopened['state']['gltfScenes']['activeRenderableCount'] == 1 and
                    np.array_equal(pixels(reopened['image']), pixels(reference['image'])),
                    'Source-free reload changed the retained placement')
            report['checks'].append(dict(technique=technique, portableReloadPixelsExact=True))
        for log in {row['log'] for row in report['captures']}:
            validate_log(Path(log))
        require(verify_runtime_manifest(manifest, renderer) == report['nativeFreeze'] and
                sha(renderer) == report['rendererSha256'], 'Frozen runtime inputs changed during validation')
        report.update(passed=True, nativeValidationPerformed=True)
    finally:
        report.update(captureCount=len(report['captures']), rejectionCount=len(report['rejected']), checkCount=len(report['checks']))
        write_json(output / 'results.json', report)


if __name__ == '__main__':
    main()
