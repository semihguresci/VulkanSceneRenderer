"""Selection-stroke binding isolation: two fixtures x both rasters = four runs.

Three blue one-pixel bars share one source primitive. Only the first imported
renderable leaf is initially selected. In the ordinary case they are distinct
source nodes; in the EXT case they are three glTF instances. A fourth ordinary
offscreen node prevents a trivial one-object scene in both fixtures. Exact
single-sample yellow pixels must surround ONLY the literal first bar; every
literal blue bar must remain unchanged. Source/graph selection is checked when
the production telemetry exposes it. The actual object-buffer row is NOT
instrumented, so this harness makes no measured nonzero-object-index claim.

Reuses the separately frozen continuity harness without changing it. In-memory
--self-check runs no renderer; --generate-only writes fixtures only. Native
output must be a fresh directory, preserving all earlier reports and failures.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import NativeRun, TECHNIQUES, require
from gltf_texture_formats_runtime_regression import verify_manifest
from gltf_extended_materials_runtime_regression import validation_log_error
from selection_stroke_runtime_regression import (build_asset, literal_fill,
    dilate, image_masks, shifted, stroke_metrics)


CASES = ('ordinary-nodes', 'ext-instances')
WIDTH, HEIGHT = 640, 360
SELECTED = 'Initially selected bar owner'
OFFSCREEN = 'Unselected offscreen ordinary owner'
PIXEL_OFFSETS = (0, -60, 60)


def build_fixture(case):
    require(case in CASES, 'Unknown binding fixture')
    asset, projection = build_asset('horizontal', WIDTH, HEIGHT)
    unit = projection['worldUnitsPerPixel']
    translations = [[0., -float(dy)*unit, 0.] for dy in PIXEL_OFFSETS]
    if case == 'ordinary-nodes':
        asset.model['nodes'] = [dict(name=SELECTED if i == 0 else f'Unselected bar owner {i}',
            mesh=0, translation=value) for i, value in enumerate(translations)]
    else:
        accessor = asset.accessor('VEC3', translations)
        asset.model['nodes'] = [dict(name=SELECTED, mesh=0,
            extensions=dict(EXT_mesh_gpu_instancing=dict(attributes=dict(TRANSLATION=accessor))))]
        asset.model['extensionsUsed'].append('EXT_mesh_gpu_instancing')
        asset.model['extensionsRequired'] = ['EXT_mesh_gpu_instancing']
    asset.model['nodes'].append(dict(name=OFFSCREEN, mesh=0, translation=[20., 0., 0.]))
    asset.model['scenes'][0]['nodes'] = list(range(len(asset.model['nodes'])))
    selected = literal_fill('horizontal', WIDTH, HEIGHT)
    all_blue = np.zeros_like(selected)
    bars = []
    for dy in PIXEL_OFFSETS:
        bar = shifted(selected, 0, dy)
        all_blue |= bar
        bars.append(bar)
    return asset, projection, selected, all_blue, bars


def inspect_selection(state, case):
    view = state.get('gltfMerge', {})
    rows = view.get('authoringNodes')
    selected = state.get('gui', {}).get('selectedMeshNodeId')
    result = dict(actualObjectBufferIndexMeasured=False,
        sourceGraphSelectionTelemetryAvailable=rows is not None and selected is not None)
    if not result['sourceGraphSelectionTelemetryAvailable']:
        result['unverified'] = 'Source/graph selected binding telemetry unavailable; exact image isolation still checked.'
        return result
    require(not view.get('authoringNodesTruncated', False), 'Binding graph telemetry unexpectedly truncated')
    nodes = {row['graphNode']: row for row in rows}
    require(len(nodes) == len(rows), 'Duplicate graph node telemetry')
    require(selected in nodes, 'Selected graph binding is missing')
    node = nodes[selected]
    require(node['renderable'] and node['active'] and not node['retired'], 'Selected leaf is not an active renderable')
    require(node['sourceNode'] == -1 and node['sourceMesh'] == 0 and node['sourcePrimitive'] == 0,
            'Selected leaf lost synthetic/source primitive identity')
    owner = nodes.get(node['meshOwnerGraphNode'])
    require(owner is not None and owner['sourceNode'] == 0 and owner['name'] == SELECTED,
            'Automatic selection did not retain the first source binding')
    expected_instance = 0 if case == 'ext-instances' else -1
    require(node['sourceInstance'] == expected_instance, 'Automatic selection chose a different source instance')
    renderables = [row for row in rows if row['renderable'] and row['active'] and not row['retired']]
    require(len(renderables) == 4 and all(row['primitiveIndex'] == node['primitiveIndex'] for row in renderables),
            'Shared primitive was duplicated or a visible/offscreen binding disappeared')
    offscreen_owners = [row for row in rows if row['name'] == OFFSCREEN and row['sourceNode'] >= 0]
    require(len(offscreen_owners) == 1, 'Offscreen unselected source binding missing')
    require(view['sourceTables']['meshes'] == 1 and view['sourceTables']['nodes'] == (2 if case == 'ext-instances' else 4),
            'Source identity table differs from generated fixture')
    instances = state.get('gltfInstancing', {}).get('instances')
    if instances is not None:
        require(len(instances) == (3 if case == 'ext-instances' else 0), 'Wrong production glTF instance count')
        if case == 'ext-instances':
            require(sorted(row['sourceInstance'] for row in instances) == [0, 1, 2] and
                    all(row['sourceNode'] == 0 for row in instances), 'Instanced source binding identities changed')
            require(any(row['graphNode'] == selected and row['sourceInstance'] == 0 for row in instances),
                    'Selected first instance is absent from production instance telemetry')
    result.update(selectedGraphNode=selected, selectedSourceNode=owner['sourceNode'],
        selectedSourceInstance=expected_instance, activeRenderableBindings=4, sharedRuntimePrimitive=node['primitiveIndex'])
    return result


def inspect_masks(blue, yellow, selected, all_blue, bars):
    require(blue.shape == (HEIGHT, WIDTH) and yellow.shape == blue.shape, 'Wrong binding capture dimensions')
    fill_difference = blue ^ all_blue
    require(not np.any(fill_difference), 'Literal unselected/selected blue bars changed: '+str(int(fill_difference.sum()))+' pixels')
    expected = dilate(selected) & ~selected
    require(not np.any(expected & (all_blue & ~selected)), 'Fixture strokes overlap another source bar')
    result = stroke_metrics(selected, yellow, 1)
    result.update(allBluePixels=int(blue.sum()), selectedBluePixels=int(selected.sum()),
        bluePixelsPerLiteralBar=[int(np.count_nonzero(blue & bar)) for bar in bars],
        unselectedBarsHaveNoYellow=True, actualObjectBufferIndexMeasured=False)
    for bar in bars[1:]:
        require(not np.any(yellow & (dilate(bar) & ~bar)), 'Outline leaked onto an unselected node/instance')
    return result


def inspect_capture(record, case, selected, all_blue, bars):
    state = record['state']
    require(state['msaaSamples'] == 1, 'Binding oracle requires actual single-sample rasterization')
    require(np.max(np.abs(np.asarray(state['camera']['position'])-[0, 0, 3])) < 1e-5, 'Literal camera position changed')
    image = np.asarray(Image.open(record['image']).convert('RGB'))
    blue, yellow = image_masks(image)
    return dict(case=case, **inspect_masks(blue, yellow, selected, all_blue, bars),
                selectionTelemetry=inspect_selection(state, case))


def self_check():
    rejected = []
    fixtures = []
    for case in CASES:
        asset, projection, selected, blue, bars = build_fixture(case)
        require(len(asset.model['meshes']) == 1 and len(asset.model['meshes'][0]['primitives']) == 1,
                'Binding fixture must share source geometry')
        yellow = dilate(selected) & ~selected
        positive = inspect_masks(blue, yellow, selected, blue, bars)
        for name, bad_yellow in [('all-bindings', dilate(blue) & ~blue),
                                 ('wrong-binding', dilate(bars[1]) & ~bars[1]),
                                 ('extra-island', yellow | shifted(selected, -150, -100))]:
            try:
                inspect_masks(blue, bad_yellow, selected, blue, bars)
            except RuntimeError:
                rejected.append(case+'/'+name)
            else:
                raise RuntimeError('Binding oracle accepted '+case+'/'+name)
        missing = blue.copy(); missing[bars[2]] = False
        try:
            inspect_masks(missing, yellow, selected, blue, bars)
        except RuntimeError:
            rejected.append(case+'/missing-unselected-blue')
        else:
            raise RuntimeError('Binding oracle ignored an unselected bar')
        owner = dict(graphNode=0, sourceNode=0, name=SELECTED, renderable=False, active=True, retired=False)
        leaves = [dict(graphNode=i+1, sourceNode=-1, meshOwnerGraphNode=0, renderable=True, active=True,
            retired=False, sourceMesh=0, sourcePrimitive=0, sourceInstance=i if case == 'ext-instances' else -1,
            primitiveIndex=0, name='Synthetic leaf') for i in range(3)]
        offscreen = dict(graphNode=4, sourceNode=1 if case == 'ext-instances' else 3, name=OFFSCREEN,
                         renderable=False, active=True, retired=False)
        offscreen_leaf = dict(leaves[0], graphNode=5, meshOwnerGraphNode=4, sourceInstance=-1)
        state = dict(gui=dict(selectedMeshNodeId=1), gltfMerge=dict(authoringNodes=[owner, *leaves, offscreen, offscreen_leaf],
            sourceTables=dict(meshes=1, nodes=2 if case == 'ext-instances' else 4)))
        inspect_selection(state, case)
        bad = copy.deepcopy(state); bad['gui']['selectedMeshNodeId'] = 5
        try:
            inspect_selection(bad, case)
        except RuntimeError:
            rejected.append(case+'/wrong-source-owner-telemetry')
        else:
            raise RuntimeError('Binding telemetry accepted the offscreen selection')
        fixtures.append(dict(case=case, sourceNodes=len(asset.model['nodes']),
            literalBluePixels=positive['allBluePixels'], expectedYellowPixels=int(yellow.sum()), **projection))
    require(len(rejected) == 10, 'Missing binding verifier negative controls')
    return dict(passed=True, fixtures=fixtures, rejectedControls=rejected, nativeExecuted=False)


def artifact_hashes():
    folder = Path(__file__).resolve().parent
    names = (Path(__file__).name, 'selection_stroke_runtime_regression.py',
             'gltf_accessor_runtime_regression.py', 'gltf_texture_formats_runtime_regression.py',
             'gltf_extended_materials_runtime_regression.py')
    return {str(folder/name): hashlib.sha256((folder/name).read_bytes()).hexdigest() for name in names}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--timeout', type=float, default=180.)
    parser.add_argument('--self-check', action='store_true')
    parser.add_argument('--generate-only', action='store_true')
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2), flush=True)
        if args.output is None and args.renderer is None:
            return
    require(args.output is not None, '--output required for generation/native execution')
    output = args.output.resolve()
    require(not output.exists() or not any(output.iterdir()), 'Output directory must be fresh; old evidence will not be overwritten')
    cases = args.case or CASES
    if args.generate_only:
        output.mkdir(parents=True, exist_ok=True)
        rows = []
        for case in cases:
            asset, projection, *_ = build_fixture(case)
            path = output/(case+'.gltf'); asset.write(path)
            rows.append(dict(case=case, source=str(path), **projection))
        (output/'generation.json').write_text(json.dumps(dict(nativeExecuted=False, fixtures=rows,
            plannedCaptures=len(cases)*len(args.technique or TECHNIQUES)), indent=2), encoding='utf-8')
        print('Generated binding fixtures; no native execution', flush=True)
        return
    require(args.renderer is not None, '--renderer required for native execution')
    run = NativeRun(args.renderer, output, args.timeout)
    run.report.update(oracle='literal-first-binding-only-disk-five', excludedCaptureCount=0,
        actualObjectBufferIndexMeasured=False, harnessArtifactsBefore=artifact_hashes())
    if args.manifest:
        run.report['nativeManifestBefore'] = verify_manifest(args.manifest)
    try:
        for case in cases:
            asset, projection, selected, all_blue, bars = build_fixture(case)
            path = output/(case+'.gltf'); asset.write(path)
            run.report['fixtures'].append(dict(case=case, source=str(path),
                sourceSha256=hashlib.sha256(path.read_bytes()).hexdigest(), **projection))
            for technique in args.technique or TECHNIQUES:
                record = run.capture(path, technique+'-'+case, technique)
                require(not validation_log_error(Path(record['log']).read_text(encoding='utf-8', errors='replace')),
                        'Strict Vulkan validation failed: '+record['log'])
                result = inspect_capture(record, case, selected, all_blue, bars)
                run.report['checks'].append(dict(check='only-selected-node-or-instance-stroked', technique=technique,
                    passed=True, **result))
                run.save()
                print('PASS', technique, case, 'selected-only exact stroke; all three blue bars preserved', flush=True)
        run.report['harnessArtifactsAfter'] = artifact_hashes()
        require(run.report['harnessArtifactsBefore'] == run.report['harnessArtifactsAfter'], 'Binding harness/dependencies changed')
        if args.manifest:
            run.report['nativeManifestAfter'] = verify_manifest(args.manifest)
        run.finish()
    except Exception as error:
        run.report['failure'] = str(error)
        raise
    finally:
        run.report.update(captureCount=len(run.report['captures']), checkCount=len(run.report['checks']))
        run.save()
    print('PASS', len(run.report['captures']), 'captures;', len(run.report['checks']), 'checks;', output)


if __name__ == '__main__':
    main()
