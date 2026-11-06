"""Native local/inherited node flags, nearest-hit routing, history and export.

Visibility references use ordinary nodes after independently pruning hidden
mesh/light nodes. Select/hover checks exercise the reference renderer's bounded
nearest-hit ancestor adaptation; this is not a trace-through implementation of
the Khronos interaction-ray semantics and does not claim GPU pick readback.
--generate-only writes fixtures without starting the renderer.
--ray adds hard ray shadows and a bounded DDGI execution witness with zero GI
intensity; it does not claim visual validation of indirect illumination.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path

from gltf_accessor_runtime_regression import Asset, TECHNIQUES, load_asset, mesh, require
from gltf_material_variants_runtime_regression import pixels, run
from gltf_texture_formats_runtime_regression import verify_manifest

FLAGS = {'visible': 'KHR_node_visibility', 'selectable': 'KHR_node_selectability', 'hoverable': 'KHR_node_hoverability'}
# frame -> (interaction branch edited, hidden branch shown, undo depth)
SAMPLES = {1: (False, False, 0), 5: (True, False, 1), 9: (False, False, 0),
           13: (True, False, 1), 17: (True, True, 2), 21: (True, False, 1),
           25: (False, False, 0), 29: (True, False, 1), 33: (True, True, 2)}
TARGET_A, TARGET_B, HIDDEN_MESH, HIDDEN_LIGHT, HIDDEN_CAMERA, TARGET_C = 4, 6, 8, 9, 10, 12


def flag(property_name, value=None, *, metadata=True):
    result = {} if value is None else {property_name: value}
    if metadata:
        result.update(extras={'authoringTag': property_name}, vendorProperty='preserve-'+property_name)
    return {FLAGS[property_name]: result}


def fixture(path):
    asset = Asset()
    mesh(asset, asset.accessor('VEC3', [[-.27,-.25,0],[.27,-.25,0],[.27,.25,0],[-.27,.25,0]]),
         asset.accessor('VEC3', [[0,0,1]]*4), asset.accessor('VEC2', [[0,0],[1,0],[1,1],[0,1]]),
         tangent=asset.accessor('VEC4', [[1,0,0,1]]*4))
    all_true = dict(**flag('visible', True), **flag('selectable', True), **flag('hoverable', True))
    asset.model['nodes'] = [
        dict(name='Offscreen anchor', mesh=0, translation=[20,0,0]),
        dict(name='Selectable ancestor', children=[2]),
        dict(name='Disabled selection branch', children=[3],
             extensions=dict(**flag('selectable', False), **flag('hoverable'))),
        dict(name='Disabled hover branch', children=[TARGET_A], extensions=flag('hoverable', False)),
        dict(name='Nested target A', mesh=0, translation=[-.7,0,0], extensions=copy.deepcopy(all_true)),
        dict(name='Root selection block', children=[TARGET_B],
             extensions=dict(**flag('selectable', False), **flag('hoverable', metadata=False))),
        dict(name='Independent hover target B', mesh=0, translation=[.7,0,0], extensions=copy.deepcopy(all_true)),
        dict(name='Hidden ancestor', children=[HIDDEN_MESH,HIDDEN_LIGHT,HIDDEN_CAMERA], extensions=flag('visible', False)),
        dict(name='Explicit true hidden mesh', mesh=0, translation=[0,.75,0], extensions=copy.deepcopy(all_true)),
        dict(name='Explicit true hidden lamp', translation=[0,.75,1.4],
             extensions=dict(**flag('visible', True), KHR_lights_punctual=dict(light=0))),
        dict(name='Hidden-parent camera', camera=0, translation=[0,0,4], extensions=flag('visible', True)),
        dict(name='Root hover block', children=[TARGET_C], extensions=flag('hoverable', False)),
        dict(name='Independent selection target C', mesh=0, translation=[0,-.75,0], extensions=copy.deepcopy(all_true))]
    asset.model['materials'] = [dict(pbrMetallicRoughness=dict(baseColorFactor=[.8,.25,.08,1], metallicFactor=0, roughnessFactor=.8))]
    asset.model['cameras'] = [dict(name='Camera remains available', type='perspective',
        perspective=dict(yfov=.7853981633974483, znear=.1))]
    asset.model['extensions'] = dict(KHR_lights_punctual=dict(lights=[dict(type='point', color=[.1,.6,1], intensity=60, range=10)]))
    asset.model['extensionsUsed'] = [*FLAGS.values(), 'KHR_lights_punctual']
    asset.model['scenes'] = [dict(name='All flag branches', nodes=[0,1,5,7,11], extras=dict(sceneTag='all')),
                             dict(name='Shared alternate roots', nodes=[0,5,7], extras=dict(sceneTag='alternate'))]
    asset.model['scene'] = 0
    asset.write(path)
    return asset.model


def visual_reference(model, path, shown):
    # Interpret authored fixture visibility here, remove the hidden visuals,
    # then remove every tested extension. Native flag parsing/scene selection
    # therefore cannot determine whether the visual reference is correct.
    keep = [i for i in range(len(model['nodes'])) if shown or i not in (HIDDEN_MESH,HIDDEN_LIGHT)]
    remap = {old: new for new, old in enumerate(keep)}
    reference = copy.deepcopy(model)
    reference['nodes'] = [copy.deepcopy(model['nodes'][i]) for i in keep]
    for node in reference['nodes']:
        if 'children' in node: node['children'] = [remap[i] for i in node['children'] if i in remap]
        for extension in FLAGS.values(): node.get('extensions', {}).pop(extension, None)
        if not node.get('extensions'): node.pop('extensions', None)
    reference['extensionsUsed'] = ['KHR_lights_punctual']
    reference['scenes'] = [dict(nodes=[remap[i] for i in model['scenes'][0]['nodes']])]
    reference['scene'] = 0
    require(reference['buffers'] == model['buffers'] and reference['meshes'] == model['meshes'], 'Oracle changed source geometry')
    path.write_text(json.dumps(reference, indent=2), encoding='utf-8')


def inspect_state(state, model, edited, shown, *, history=None, stored_history=None, ray=False):
    flags = state['gltfNodeFlags']
    require(not flags.get('truncated',False), 'Node flag telemetry was truncated')
    rows = {row['graphNode']: row for row in flags['nodes']}
    source = {row['name']: row for row in flags['nodes'] if row['sourceIndex'] >= 0}
    node = lambda index: source[model['nodes'][index]['name']]
    require(all(node(index)['active'] for index in range(len(model['nodes']))), 'Active source branch disappeared')
    for index in range(len(model['nodes'])):
        expected = {key: model['nodes'][index].get('extensions', {}).get(extension, {}).get(key, True)
                    for key, extension in FLAGS.items()}
        if index == 2 and edited: expected.update(selectable=True, hoverable=False)
        if index == 7 and shown: expected['visible'] = True
        for key, value in expected.items():
            require(node(index)['local'+key.capitalize()] == value, f'Wrong local {key}: {model["nodes"][index]["name"]}')
    for index in (HIDDEN_MESH,HIDDEN_LIGHT,HIDDEN_CAMERA):
        require(node(index)['effectiveVisible'] == shown, 'Visible=true child overrode hidden ancestry')
        require(node(index)['effectiveSelectable'] and node(index)['effectiveHoverable'], 'Visibility changed independent interaction flags')
    require(node(HIDDEN_CAMERA)['sourceIndex'] in flags['cameraSourceNodes'], 'Hidden ancestry removed an available authored camera')
    require(state['gltfScenes']['pointLightCount'] == int(shown) and
            len(state['gltfAnimation']['pointLights']) == int(shown), 'Hidden attached light stayed active or failed to return')
    require(not node(TARGET_A)['effectiveHoverable'] and node(TARGET_A)['effectiveSelectable'] == edited,
            'Nested effective interaction flags differ')
    require(not node(TARGET_B)['effectiveSelectable'] and node(TARGET_B)['effectiveHoverable'], 'Selection root blocker affected hover')
    require(node(TARGET_C)['effectiveSelectable'] and not node(TARGET_C)['effectiveHoverable'], 'Hover root blocker affected selection')
    def probe_for(index):
        matches = []
        for probe in flags['probes']:
            current, seen = probe['graphNode'], set()
            while current in rows and current not in seen:
                seen.add(current)
                if current == node(index)['graphNode']:
                    matches.append(probe); break
                current = rows[current]['parentGraphNode']
        require(len(matches)==1, f'Expected one primitive below {model["nodes"][index]["name"]}')
        return matches[0]
    routed = []
    for index, selection, hover in ((TARGET_A, None if edited else 1, 1 if edited else 2),
                                    (TARGET_B, -1, None), (TARGET_C, None, -1)):
        probe = probe_for(index)
        require(probe['projected'] and probe['hit'] and probe['nearestHitGraphNode'] == probe['graphNode'],
                'Production nearest-hit probe did not hit its isolated target')
        require(rows[probe['graphNode']]['sourceIndex'] == -1 and
                all(rows[probe['graphNode']]['local'+key.capitalize()] for key in FLAGS),
                'Synthetic mesh child acquired source local flag overrides')
        resolve = lambda expected: probe['nearestHitGraphNode'] if expected is None else -1 if expected == -1 else node(expected)['graphNode']
        require(probe['resolvedSelection'] == resolve(selection), 'Nearest hit selected the wrong ancestor or synthetic root')
        require(probe['resolvedHover'] == resolve(hover), 'Nearest hit hovered the wrong independent ancestor')
        routed.append(dict(sourceName=probe['sourceName'], rawHit=probe['nearestHitGraphNode'],
                           selection=probe['resolvedSelection'], hover=probe['resolvedHover']))
    if shown:
        probe = probe_for(HIDDEN_MESH)
        require(probe['projected'] and probe['hit'] and probe['resolvedSelection'] == probe['nearestHitGraphNode'] and
                probe['resolvedHover'] == probe['nearestHitGraphNode'], 'Restored visible child has stale interaction state')
    if history is not None:
        # The scene view exposes stored commands, including the redo branch;
        # the material/history telemetry separately exposes its active cursor.
        require(state['gltfScenes']['editHistoryDepth'] == stored_history, 'Grouped flag edit stored command count differs')
        require(state['gltfMaterialVariants'].get('undoCount',0) == history and
                state['gltfMaterialVariants'].get('redoCount',0) == stored_history-history,
                'Grouped flag edit undo/redo cursor differs')
    result = dict(editedInteraction=edited, shown=shown, activeLights=int(shown), availableHiddenCamera=True, routedTargets=routed)
    if ray:
        shadows, gi = state['rayShadows'], state['ddgi']
        visible_geometry = 4 + int(shown) # Offscreen anchor, three targets, optional restored mesh.
        require(shadows['supported'] and shadows['active'] and shadows['build']['instances'] == visible_geometry and
                shadows['build']['blasBuilt'] + shadows['build']['blasReused'] > 0,
                'Production TLAS retained hidden geometry or omitted restored visible geometry')
        require(gi['supported'] and gi['active'] and gi['updatedProbes'] > 0 and gi['tracedRays'] > 0,
                'Node flag scene did not publish production DDGI work')
        result.update(rayInstances=shadows['build']['instances'], ddgiUpdatedProbes=gi['updatedProbes'],
                      ddgiTracedRays=gi['tracedRays'], ddgiWitness='execution only; GI intensity is zero')
    return result


def inspect_export(path, original):
    model, _ = load_asset(path)
    require(len(model['scenes']) == 2 and [scene['extras']['sceneTag'] for scene in model['scenes']] == ['all','alternate'],
            'Flag save lost inactive/shared scene metadata')
    nodes = {node['name']: node for node in model['nodes'] if 'name' in node}
    for index, source in enumerate(original['nodes']):
        expected = copy.deepcopy(source.get('extensions', {}))
        if index == 2:
            expected[FLAGS['selectable']]['selectable'] = True
            expected[FLAGS['hoverable']]['hoverable'] = False
        if index == 7: expected[FLAGS['visible']]['visible'] = True
        actual = nodes[source['name']].get('extensions', {})
        for extension in FLAGS.values():
            require((extension in actual) == (extension in expected), f'Authored extension presence changed: {source["name"]}')
            if extension in expected: require(actual[extension] == expected[extension], f'Flag property/metadata lost: {source["name"]}')
    for node in model['nodes']:
        if 'mesh' in node:
            require(not any(extension in node.get('extensions', {}) for extension in FLAGS.values()),
                    'Synthetic geometry node acquired copied source flag metadata')
    return dict(format=path.suffix, authoredFlagMetadata=True, syntheticFlagsAbsent=True, sceneCount=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--ray', action='store_true')
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    require(renderer.is_file(), f'Missing renderer: {renderer}')
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n', encoding='utf-8')
    report = dict(schemaVersion=1, passed=False, generatedOnly=args.generate_only, nativeValidationPerformed=False,
        rendererSha256=hashlib.sha256(renderer.read_bytes()).hexdigest(), ray=args.ray, captures=[], checks=[],
        expectedCaptureCount=13*len(args.technique or TECHNIQUES), expectedCheckCount=12*len(args.technique or TECHNIQUES),
        selectionPolicy='Nearest-hit ancestor reference approximation; no trace-through or GPU picking readback')
    def save():
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    if args.manifest: report['nativeManifestBefore'] = verify_manifest(args.manifest)
    try:
        source = output/'node-flags.gltf'; model = fixture(source)
        oracles = {False: output/'hidden-pruned-oracle.gltf', True: output/'visible-ordinary-oracle.gltf'}
        for shown, path in oracles.items(): visual_reference(model, path, shown)
        sequence = output/'flags-history.json'
        sequence.write_text(json.dumps(dict(schemaVersion=1, frames=max(SAMPLES), sampleFrames=list(SAMPLES),
            camera=[dict(frame=1,position=[0,0,4],target=[0,0,0])], events=[
                dict(frame=2,nodeFlags=dict(sourceNode=2,selectable=True,hoverable=False)),
                dict(frame=6,sceneHistory='undo'), dict(frame=10,sceneHistory='redo'),
                dict(frame=14,nodeFlags=dict(sourceNode=7,visible=True)), dict(frame=18,sceneHistory='undo'),
                dict(frame=22,sceneHistory='undo'), dict(frame=26,sceneHistory='redo'), dict(frame=30,sceneHistory='redo')]), indent=2), encoding='utf-8')
        report['fixtures'] = dict(source=str(source), references={str(k):str(v) for k,v in oracles.items()}, sequence=str(sequence))
        if not args.generate_only:
            for technique in args.technique or TECHNIQUES:
                references = {shown: run(renderer,output,path,technique+('-visible' if shown else '-hidden')+'-oracle',
                    technique,'lit',report['captures'],ray=args.ray)[0] for shown,path in oracles.items()}
                negative = pixels(references[False]['image'],references[True]['image'])
                require(negative['meanAbsoluteError']>.25 and negative['changedPixelFraction']>.003, 'Visibility/light negative control is vacuous')
                report['checks'].append(dict(technique=technique,check='hidden-visuals-negative',**negative))
                destination = output/(technique+'-edited.glb')
                captured = run(renderer,output,source,technique+'-history',technique,'lit',report['captures'],sequence=sequence,save=destination,ray=args.ray)
                require(len(captured)==len(SAMPLES), 'Missing flag/history captures')
                for record,(frame,(edited,shown,depth)) in zip(captured,sorted(SAMPLES.items())):
                    semantics = inspect_state(record['state'],model,edited,shown,history=depth,
                        stored_history=0 if frame==1 else 1 if frame<17 else 2,ray=args.ray)
                    error = pixels(record['image'],references[shown]['image'])
                    require(error['maximumError']==0, f'Independent visibility oracle differs at frame{frame}: {error}')
                    report['checks'].append(dict(technique=technique,frame=frame,**semantics,**error))
                for path in (destination,destination.with_suffix('.gltf')):
                    exported = inspect_export(path,model)
                    record = run(renderer,output,path,
                        technique+('-ascii-reload' if path.suffix=='.gltf' else '-glb-reload'),technique,'lit',report['captures'],
                        save=destination.with_suffix('.gltf') if path.suffix=='.glb' else None,ray=args.ray)[0]
                    semantics = inspect_state(record['state'],model,True,True,ray=args.ray)
                    error = pixels(record['image'],references[True]['image'])
                    require(error['maximumError']==0, f'Portable node flag appearance changed: {error}')
                    report['checks'].append(dict(technique=technique,check='portable-flag-reload',**exported,**semantics,**error))
                report['nativeValidationPerformed']=True
                save(); print('PASS node flags',technique,flush=True)
        require(hashlib.sha256(renderer.read_bytes()).hexdigest()==report['rendererSha256'],'Renderer changed during validation')
        if args.manifest: report['nativeManifestAfter']=verify_manifest(args.manifest)
        report['passed']=None if args.generate_only else True
    except Exception as error:
        report['failure']=str(error); raise
    finally: save()
    print('GENERATED' if args.generate_only else 'PASS',report['captureCount'],'captures',report['checkCount'],'checks',flush=True)


if __name__=='__main__':
    main()
