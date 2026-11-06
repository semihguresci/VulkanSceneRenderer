"""Native stable-slot subtree retirement, history, compaction and playback checks.

The core cohort uses literal binary-exact affine transforms, a shared quad, and
independent morph/material/camera/light arithmetic. Export reload runs from a
separate resource directory. A separate ray-ddgi cohort checks actual execution
with GI intensity zero. Generation and self-check never execute the renderer.
Native runs require a manifest binding this file, all local imports, the chosen
executable and every resident SPIR-V file. Failed evidence is never overwritten.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

import numpy as np

import scene_node_authoring_runtime_regression as authoring
from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, require
from gltf_extended_materials_runtime_regression import sha, validation_log_error
from gltf_merge_runtime_regression import document, forest_worlds, immutable, portable_copy
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest

TECHNIQUES = ('deferred-raster', 'forward-raster')
WIDTH, HEIGHT, INVALID = 640, 360, 2**32-1
PUNCTUAL, INSTANCING = 'KHR_lights_punctual', 'EXT_mesh_gpu_instancing'
RENAMED = 'Retired branch \u6771'
POSITIONS = [[-.5, -.375, 0.], [.5, -.375, 0.], [.5, .375, 0.], [-.5, .375, 0.]]
INDICES = [0, 1, 2, 0, 2, 3]
SHEAR = np.asarray([[1., .125, 0., 0.], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]])
RIGHT, LEFT, BACKGROUND = [397, 165, 423, 195], [175, 100, 300, 260], [16, 16, 64, 64]
STATIC_TABLES = dict(nodes=6, meshes=1, materials=1, images=0, textures=0, samplers=0, buffers=1, scenes=3)
# name, frame, renamed, hidden survivor, retired, undo, redo, selected scene
STATIC = [
    ('before', 13, False, False, False, 0, 0, 0),
    ('prior', 26, True, True, False, 2, 0, 0),
    ('delete', 39, True, True, True, 3, 0, 0),
    ('undo-delete', 52, True, True, False, 2, 1, 0),
    ('undo-visible', 65, True, False, False, 1, 2, 0),
    ('undo-rename', 78, False, False, False, 0, 3, 0),
    ('redo-rename', 91, True, False, False, 1, 2, 0),
    ('redo-visible', 104, True, True, False, 2, 1, 0),
    ('redo-delete', 117, True, True, True, 3, 0, 0),
    ('show-survivor', 130, True, False, True, 4, 0, 0),
    ('shared', 143, True, False, True, 0, 0, 1),
    ('empty', 156, True, False, True, 0, 0, 2)]
DYNAMIC = [('before', 13), ('deleted', 26), ('resumed', 39), ('undo', 52), ('redo', 65)]
RAY = [('before', 13), ('deleted', 26), ('undo', 39), ('redo', 52)]
UNVERIFIED = [
    'CPU deformation samples are binding evidence, not GPU vertex-buffer readback; pixels and GPU-timed passes provide execution witnesses.',
    'Portable reload starts with fresh loader selection; editor selection and stable runtime slots are not serialized.',
    'The rejected retained-skin process aborts after its baseline. No post-rejection unchanged-state or history claim is made.',
    'Physics/body rejection, allocation failure injection, GPU picking and leak counts are not exercised.',
    'Per-light manual edits and animation loop toggles have no capture mutation in this cohort.',
    'Ray/DDGI runs are separate execution checks with GI intensity zero; they do not establish a GI image-quality oracle.',
    'Generation and self-check do not establish native rendering.']


def write_asset(asset, path):
    with tempfile.TemporaryDirectory(prefix='retirement-asset-') as temporary:
        generated = Path(temporary)/'scene.gltf'
        asset.write(generated)
        immutable(path, generated.read_bytes())


def quad(asset, *, morph=False, skinned=False, lit=False):
    material = dict(name='Shared retirement material', extras=dict(retirementTag='material'),
        pbrMetallicRoughness=dict(baseColorFactor=[1., .25, .125, 1.], metallicFactor=0., roughnessFactor=1.))
    if not lit: material['extensions'] = {'KHR_materials_unlit': {}}
    asset.model['materials'] = [material]
    attributes = dict(POSITION=asset.accessor('VEC3', POSITIONS), NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*4))
    if skinned:
        attributes.update(JOINTS_0=asset.accessor('VEC4', [[0, 0, 0, 0]]*4, 5121),
            WEIGHTS_0=asset.accessor('VEC4', [[1., 0., 0., 0.]]*4))
    primitive = dict(attributes=attributes, indices=asset.accessor('SCALAR', INDICES, 5123, indices=True),
        material=0, mode=4, extras=dict(retirementTag='primitive', opaqueIndex=991))
    if morph: primitive['targets'] = [dict(POSITION=asset.accessor('VEC3', [[.5, .1, 0.]]*4))]
    asset.model['meshes'] = [dict(name='Shared retirement quad', primitives=[primitive], extras=dict(retirementTag='mesh'))]
    asset.model['extensionsUsed'] = [] if lit else ['KHR_materials_unlit']


def static_fixture(path, *, survivor_only=False, lit=False):
    asset = Asset(); quad(asset, lit=lit)
    tag = lambda name: dict(retirementTag=name, opaqueIndex=977)
    asset.model['nodes'] = [
        dict(name='Shared scene root', matrix=SHEAR.T.reshape(-1).tolist(), children=[1, 2], extras=tag('root')),
        dict(name='Delete branch', translation=[-1.25, 0., 0.], children=[3, 4], extras=tag('delete')),
        dict(name='Surviving shared owner', mesh=0, translation=[1.25, 0., 0.], extras=tag('survivor')),
        dict(name='Nested empty', translation=[0., .25, 0.], children=[5], extras=tag('delete-empty')),
        dict(name='Deleted instanced owner', mesh=0, translation=[0., -.5, 0.], extras=tag('delete-instanced'),
            extensions={INSTANCING: dict(attributes=dict(
                TRANSLATION=asset.accessor('VEC3', [[-.5, 0., 0.], [.5, 0., 0.]]),
                SCALE=asset.accessor('VEC3', [[.5, .5, 1.]]*2),
                _ID=asset.accessor('SCALAR', [16777217, 16777219], 5125)), extras=dict(opaqueInstanceIndex=991))}),
        dict(name='Nested single owner', mesh=0, translation=[0., .375, 0.], scale=[.75, .75, 1.], extras=tag('delete-leaf'))]
    asset.model['scenes'] = [dict(name='Shared A', nodes=[0], extras=dict(retirementTag='scene-a')),
        dict(name='Shared B', nodes=[0], extras=dict(retirementTag='scene-b')),
        dict(name='Empty', nodes=[], extras=dict(retirementTag='scene-empty'))]
    asset.model['extensionsUsed'].append(INSTANCING)
    if survivor_only:
        root, owner = copy.deepcopy(asset.model['nodes'][0]), copy.deepcopy(asset.model['nodes'][2])
        root['children'] = [1]; asset.model['nodes'] = [root, owner]
        asset.model['extensionsUsed'].remove(INSTANCING)
    write_asset(asset, path)


def dynamic_fixture(path):
    asset = Asset(); quad(asset, morph=True)
    asset.model['nodes'] = [dict(name='Dynamic delete branch', children=[1]),
        dict(name='Deleted morph owner', mesh=0, weights=[0.], translation=[-1.25, 0., 0.]),
        dict(name='Surviving morph owner', mesh=0, weights=[.2], translation=[1.25, 0., 0.]),
        dict(name='Surviving animated lamp', translation=[0., 1., 3.], extensions={PUNCTUAL: dict(light=0)}),
        dict(name='Surviving animated camera', camera=0, translation=[0., 0., 6.])]
    asset.model['scenes'] = [dict(name='Dynamic', nodes=[0, 2, 3, 4])]
    asset.model['cameras'] = [dict(name='Animated camera', type='perspective', perspective=dict(yfov=.7, znear=.1, zfar=100.))]
    asset.model['extensions'] = {PUNCTUAL: dict(lights=[dict(name='Animated lamp', type='point', intensity=20., range=10.)])}
    asset.model['extensionsUsed'] += [PUNCTUAL, 'KHR_animation_pointer']
    times = asset.accessor('SCALAR', [0., 2.]); samplers, channels = [], []
    def track(kind, values, node=None, path=None, pointer=None):
        target = dict(path='pointer', extensions={'KHR_animation_pointer': dict(pointer=pointer)}) if pointer else dict(node=node, path=path)
        channels.append(dict(sampler=len(samplers), target=target))
        samplers.append(dict(input=times, output=asset.accessor(kind, values), interpolation='LINEAR'))
    track('VEC3', [[-1.25, 0., 0.], [-.75, .25, 0.]], node=1, path='translation')
    track('SCALAR', [0., 1.], node=1, path='weights')
    track('SCALAR', [.2, .8], node=2, path='weights')
    track('VEC4', [[1., .25, .125, 1.], [.125, .75, 1., 1.]], pointer='/materials/0/pbrMetallicRoughness/baseColorFactor')
    track('SCALAR', [20., 40.], pointer='/extensions/KHR_lights_punctual/lights/0/intensity')
    track('SCALAR', [.7, .9], pointer='/cameras/0/perspective/yfov')
    asset.model['animations'] = [dict(name='Deletion snapshot playback', samplers=samplers, channels=channels)]
    write_asset(asset, path)


def blocked_fixture(path):
    asset = Asset(); quad(asset, skinned=True)
    asset.model['nodes'] = [dict(name='Retained skin owner', mesh=0, skin=0), dict(name='Protected used joint')]
    asset.model['skins'] = [dict(name='Retained skin', joints=[1], skeleton=1)]
    asset.model['scenes'] = [dict(name='Retained skin', nodes=[0, 1])]
    write_asset(asset, path)


def sequence(frames, samples, events, *, fixed_camera=True):
    result = dict(schemaVersion=1, frames=frames, sampleFrames=samples, events=events)
    if fixed_camera: result['camera'] = [dict(frame=1, position=[0., 0., 6.], target=[0., 0., 0.])]
    return result


def deletion(frame, source):
    return dict(frame=frame, nodeAuthoring=dict(action='delete-subtree', sourceNode=source))


def generate(output, cohort):
    output.mkdir(parents=True, exist_ok=True)
    sources = {name: output/'sources'/(name+'.gltf') for name in ('static', 'survivor', 'dynamic', 'blocked', 'ray')}
    for name in ('static', 'survivor', 'ray'):
        static_fixture(sources[name], survivor_only=name == 'survivor', lit=name == 'ray')
    dynamic_fixture(sources['dynamic']); blocked_fixture(sources['blocked'])
    scripts = {
        'static': sequence(156, [row[1] for row in STATIC], [
            dict(frame=14, nodeAuthoring=dict(action='rename', sourceNode=1, name=RENAMED)),
            dict(frame=15, nodeFlags=dict(sourceNode=2, visible=False)), deletion(27, 1),
            *[dict(frame=frame, sceneHistory=action) for frame, action in [(40, 'undo'), (53, 'undo'), (66, 'undo'),
                (79, 'redo'), (92, 'redo'), (105, 'redo')]],
            dict(frame=118, nodeFlags=dict(sourceNode=2, visible=True)), dict(frame=131, gltfScene=1), dict(frame=144, gltfScene=2)]),
        'ascii': sequence(13, [13], [deletion(2, 1), dict(frame=3, gltfScene=2)]),
        'reload': sequence(39, [13, 26, 39], [dict(frame=14, gltfScene=1), dict(frame=27, gltfScene=2)]),
        'dynamic': sequence(65, [row[1] for row in DYNAMIC], [deletion(14, 0), dict(frame=27, animationPlaying=True),
            dict(frame=40, sceneHistory='undo'), dict(frame=53, sceneHistory='redo')], fixed_camera=False),
        'blocked': sequence(26, [13], [deletion(14, 1)]),
        'ray': sequence(52, [row[1] for row in RAY], [deletion(14, 1), dict(frame=27, sceneHistory='undo'), dict(frame=40, sceneHistory='redo')])}
    for name, value in scripts.items(): document(output/(name+'.json'), value)
    immutable(output/'vk_layer_settings.txt', b'khronos_validation.validate_sync = true\n')
    files = [*sources.values(), *(output/(name+'.json') for name in scripts), output/'vk_layer_settings.txt']
    plan = dict(schemaVersion=1, cohort=cohort, sources={name: str(path) for name, path in sources.items()},
        scripts={name: str(output/(name+'.json')) for name in scripts},
        immutableInputs=[dict(path=str(path), sha256=sha(path), byteLength=path.stat().st_size) for path in files],
        capturesPerTechnique=26 if cohort == 'core' else 4, processesPerTechnique=7 if cohort == 'core' else 1,
        staticStates=STATIC, dynamicStates=DYNAMIC, rois=dict(right=RIGHT, deletedLeft=LEFT, background=BACKGROUND),
        captureSettings=dict(resolution=[WIDTH, HEIGHT], fixedDelta=.016666667, importScale=1, exposure=.35,
            msaa=1, taa=False, bloom=False, environmentIntensity=0),
        unverified=UNVERIFIED)
    document(output/'launch-plan.json', plan)
    return plan


def close(actual, expected, label, tolerance=3e-5):
    a, b = np.asarray(actual, dtype=float), np.asarray(expected, dtype=float)
    require(a.shape == b.shape and np.isfinite(a).all() and np.isfinite(b).all() and
        np.max(np.abs(a-b), initial=0.) <= tolerance, 'Incorrect retirement '+label)


def source_mapping(nodes, count):
    pairs = [(row['sourceNode'], index) for index, row in nodes.items() if row['sourceNode'] >= 0]
    result = dict(pairs)
    require(len(result) == len(pairs) == count and set(result) == set(range(count)), 'Source identities are absent/aliased')
    return result


def descendants(nodes, root):
    children = {index: [] for index in nodes}
    for index, row in nodes.items():
        parent = row['parentGraphNode']
        require(parent == INVALID or parent in nodes, 'Invalid runtime parent')
        if parent != INVALID: children[parent].append(index)
    result, pending = set(), [root]
    while pending:
        index = pending.pop(); require(index not in result, 'Runtime hierarchy is not a strict forest')
        result.add(index); pending.extend(children[index])
    return result


def stable_state(state, baseline, *, source_count, retired_root, retired, scene, roots, undo, redo, draws,
                 active_renderables, selected, renamed=None, hidden=False, dynamic=False, allow_uncreated=False):
    nodes, original = authoring.rows(state), authoring.rows(baseline)
    mapping = source_mapping(nodes, source_count)
    require(set(nodes) == set(original) and state['gltfMerge']['graphIdentity'] == baseline['gltfMerge']['graphIdentity'],
        'Deletion changed graph identity or stable slot set')
    require(state['gltfMerge']['sourceTables'] == baseline['gltfMerge']['sourceTables'], 'Retirement compacted live source tables')
    require(state['gltfMerge']['activeRoots'] == roots and state['gltfScenes']['selected'] == scene, 'Exact active-root view changed')
    require(state['gltfScenes']['activeRenderableCount'] == state['scene']['primaryObjectCount'] == active_renderables and
        state['gltfInstancing']['drawCount'] == draws, 'Wrong active/visible object membership')
    require(state['gui']['selectedMeshNodeId'] == selected, 'Deletion selection/history restoration changed')
    authoring.history_cursor(state, undo, redo, allow_uncreated=allow_uncreated)
    require(not state['gui']['modelLoading']['pending'] and state['scene']['primaryImportScale'] == 1., 'Retirement did not settle')
    merge = state['gltfMerge']
    require(merge['sourceReadCount'] == merge['generationPublications'] == merge['requestId'] == 0 and merge['outcome'] == 'idle' and
        not merge['error'] and merge['descriptorGeneration'] == merge['commitGeneration'], 'Retirement used the import/merge transaction')
    closure = descendants(original, mapping[retired_root])
    flags = {row['graphNode']: row for row in state['gltfNodeFlags']['nodes']}
    require(set(flags) == set(nodes), 'Retirement flags omit stable slots')
    for index, row in nodes.items():
        expected_retired = retired and index in closure
        require(row['retired'] is expected_retired and flags[index]['retired'] is expected_retired, 'Wrong retired stable slot')
        for key in ('sourceNode', 'parentGraphNode', 'meshOwnerGraphNode', 'renderable'):
            require(row[key] == original[index][key], 'Retirement changed retained node '+key)
        expected_name = RENAMED if renamed and row['sourceNode'] == 1 else original[index]['name']
        require(row['name'] == expected_name, 'Retirement/history changed authored name')
        if not dynamic:
            require(np.array_equal(authoring.matrix(row), authoring.matrix(original[index])), 'Retirement changed exact world transform')
        require(flags[index]['localSelectable'] and flags[index]['localHoverable'], 'Retirement changed local interaction flags')
        require(flags[index]['localVisible'] is (not hidden if row['sourceNode'] == 2 else True), 'Visibility history was overwritten')
        require(not expected_retired or not flags[index]['effectiveVisible'] and not row['active'], 'Retired slot is active/visible')
    return nodes, mapping, closure


def static_state(state, baseline, spec, source):
    name, _, renamed, hidden, retired, undo, redo, scene = spec
    original = authoring.rows(baseline); mapping = source_mapping(original, 6)
    closure = descendants(original, mapping[1]); selected_before = baseline['gui']['selectedMeshNodeId']
    require(selected_before in closure and original[selected_before]['renderable'], 'Initial selection does not exercise deleted mesh restoration')
    roots = [mapping[0]] if scene != 2 else []
    draws = (0 if retired else 3)+int(not hidden) if scene != 2 else 0
    active = (1 if retired else 4) if scene != 2 else 0
    selected = selected_before if not retired else INVALID
    nodes, mapping, closure = stable_state(state, baseline, source_count=6, retired_root=1, retired=retired,
        scene=scene, roots=roots, undo=undo, redo=redo, draws=draws, active_renderables=active,
        selected=selected, renamed=renamed, hidden=hidden, allow_uncreated=name == 'before')
    require(len(nodes) == 11 and state['gltfMerge']['sourceTables'] == STATIC_TABLES and
        state['gltfScenes']['names'] == ['Shared A', 'Shared B', 'Empty'], 'Static source/catalog/slot counts changed')
    active_sources = [] if scene == 2 else [0, 2] if retired else list(range(6))
    require(sorted(state['gltfScenes']['activeSourceNodes']) == active_sources, 'Shared-scene retirement is not global')
    active_slots = descendants(original, mapping[0]) if scene != 2 else set()
    if retired: active_slots -= closure
    survivor_slots = descendants(original, mapping[2])
    flags = {row['graphNode']: row for row in state['gltfNodeFlags']['nodes']}
    for index, row in nodes.items():
        require(row['active'] is (index in active_slots) and
            flags[index]['effectiveVisible'] is (index in active_slots and (not hidden or index not in survivor_slots)),
            'Exact runtime active/effective-visible membership changed')
    worlds, _ = forest_worlds(load_asset(source)[0])
    for index, graph in mapping.items():
        require(np.array_equal(authoring.matrix(nodes[graph]), worlds[index]), 'Source world differs from literal affine oracle')
    instances = state['gltfInstancing']['instances']
    require(len(instances) == (0 if retired or scene == 2 else 2), 'Instanced descendants survived retirement or were lost on undo')
    if instances:
        matrices = []
        for x in (-.5, .5):
            local = np.diag([.5, .5, 1., 1.]); local[0, 3] = x
            matrices.append(worlds[4]@local)
        for row in instances:
            require(row['sourceNode'] == 4, 'Instance source binding changed')
            matrix = np.asarray(row['modelColumns']).T
            require(any(np.array_equal(matrix, expected) for expected in matrices), 'Instance transform order changed')
    return dict(passed=True, name=name, retiredSlots=len(closure) if retired else 0, sourceIdsStable=True,
        exactWorldsAndRoots=True, visibleDraws=draws, activeRenderableCount=active, undoCount=undo, redoCount=redo)


def ascii_state(state, source):
    """One final sample has no same-process pre-edit identity witness."""
    nodes = authoring.rows(state); mapping = source_mapping(nodes, 6)
    closure = descendants(nodes, mapping[1]); model, _ = load_asset(source); worlds, _ = forest_worlds(model)
    require(len(nodes) == 11 and len(closure) == 7 and state['gltfMerge']['sourceTables'] == STATIC_TABLES,
        'ASCII save did not retain the complete stable-slot source forest')
    require(state['gltfScenes']['selected'] == 2 and state['gltfMerge']['activeRoots'] == [] and
        state['gltfScenes']['activeSourceNodes'] == [] and state['gui']['selectedMeshNodeId'] == INVALID and
        state['gltfScenes']['activeRenderableCount'] == state['scene']['primaryObjectCount'] == state['gltfInstancing']['drawCount'] == 0,
        'ASCII final empty view contains active/selected deleted content')
    authoring.history_cursor(state, 0, 0)
    flags = {row['graphNode']: row for row in state['gltfNodeFlags']['nodes']}
    require(set(flags) == set(nodes), 'ASCII final flags omit retained slots')
    for index, row in nodes.items():
        require(row['retired'] is (index in closure) and flags[index]['retired'] is (index in closure) and
            flags[index]['localVisible'] and flags[index]['localSelectable'] and flags[index]['localHoverable'],
            'ASCII final retirement/independent local flags differ')
    for source_id, graph in mapping.items():
        require(nodes[graph]['name'] == model['nodes'][source_id]['name'] and
            np.array_equal(authoring.matrix(nodes[graph]), worlds[source_id]), 'ASCII retained source name/world changed')
    return dict(passed=True, scene=2, retiredSlots=7, sourceIdsComplete=True, exactSourceWorlds=True,
        crossProcessStableSlotClaim=False)


def mesh_descendants(model, root):
    result, pending, seen = [], [root], set()
    while pending:
        index = pending.pop(); require(index not in seen and 0 <= index < len(model['nodes']), 'Invalid exported subtree')
        seen.add(index); row = model['nodes'][index]
        if 'mesh' in row: result.append(index)
        pending.extend(row.get('children', []))
    return result


def inspect_export(path, source):
    model, _ = load_asset(path); original, _ = load_asset(source)
    worlds, _ = forest_worlds(model)
    require(len(model['nodes']) == 4 and len(model['scenes']) == 3 and model.get('scene') == 2,
        'Export did not compact exactly the seven retired graph slots')
    require(not model.get('skins') and not model.get('animations') and not model.get('cameras'), 'Unexpected compacted resource catalog')
    tags = {row.get('extras', {}).get('retirementTag'): index for index, row in enumerate(model['nodes'])
        if 'retirementTag' in row.get('extras', {})}
    require(set(tags) == {'root', 'survivor'}, 'Retired source metadata remains or retained metadata was lost')
    require(all(row.get('name') not in ('Delete branch', RENAMED, 'Nested empty', 'Deleted instanced owner', 'Nested single owner')
        for row in model['nodes']), 'Retired authored names remain in exported nodes')
    for index, expected in enumerate(original['scenes']):
        require(model['scenes'][index].get('name') == expected['name'] and model['scenes'][index].get('extras') == expected['extras'] and
            model['scenes'][index].get('nodes', []) == ([tags['root']] if index < 2 else []), 'Compaction changed scene metadata/membership')
    require(model['nodes'][tags['root']]['extras'] == original['nodes'][0]['extras'] and
        model['nodes'][tags['survivor']]['extras'] == original['nodes'][2]['extras'], 'Opaque node metadata changed')
    children = mesh_descendants(model, tags['survivor']); require(len(children) == 1, 'Survivor source owner lost/shared duplicate mesh draw')
    child = children[0]; primitive = model['meshes'][model['nodes'][child]['mesh']]['primitives'][0]
    require(primitive.get('extras') == original['meshes'][0]['primitives'][0]['extras'] and
        model['meshes'][model['nodes'][child]['mesh']].get('extras') == original['meshes'][0]['extras'] and
        model['materials'][primitive['material']].get('extras') == original['materials'][0]['extras'], 'Shared geometry/material metadata changed')
    expected_worlds, _ = forest_worlds(original)
    require(np.array_equal(worlds[child], expected_worlds[2]), 'Compacted survivor world transform changed')
    close(decode_accessor(path, primitive['attributes']['POSITION']), POSITIONS, 'exported shared positions', 0.)
    require(np.array_equal(decode_accessor(path, primitive['indices']).reshape(-1), INDICES), 'Exported shared indices changed')
    for index in range(len(model.get('accessors', []))):
        require(np.isfinite(decode_accessor(path, index)).all(), 'Nonresident/invalid compacted accessor')
    # The shared forest validator checks every node, including orphan editor
    # slots, so no dangling retired child/joint/root index can evade this check.
    return dict(passed=True, path=str(path), sha256=sha(path), compactedNodeCount=4, removedSlots=7,
        sourceRoot=tags['root'], sourceOwner=tags['survivor'], sourceMeshChild=child, sceneMetadataExact=True,
        sharedGeometryAndOpaqueMetadataPreserved=True)


def reload_state(state, exported, *, scene, first, baseline):
    model, _ = load_asset(exported); nodes = authoring.rows(state); mapping = source_mapping(nodes, len(model['nodes']))
    worlds, _ = forest_worlds(model)
    require(len(nodes) == 6 and all(not row['retired'] for row in nodes.values()), 'Portable reload retained tombstones or omitted source groups')
    require(state['gltfScenes']['selected'] == scene and state['gltfScenes']['names'] == ['Shared A', 'Shared B', 'Empty'], 'Reload scene catalog changed')
    expected_roots = [mapping[index] for index in model['scenes'][scene].get('nodes', [])]
    require(state['gltfMerge']['activeRoots'] == expected_roots, 'Portable exact roots changed')
    active_source = set()
    pending = list(model['scenes'][scene].get('nodes', []))
    while pending:
        index = pending.pop(); require(index not in active_source, 'Invalid compacted source forest')
        active_source.add(index); pending.extend(model['nodes'][index].get('children', []))
    require(set(state['gltfScenes']['activeSourceNodes']) == active_source, 'Reload activated orphan context/retired content')
    count = int(scene != 2)
    require(state['gltfScenes']['activeRenderableCount'] == state['scene']['primaryObjectCount'] == state['gltfInstancing']['drawCount'] == count,
        'Portable reload lost surviving shared owner or restored deleted content')
    for source, graph in mapping.items():
        require(np.array_equal(authoring.matrix(nodes[graph]), worlds[source]), 'Reloaded literal world changed')
    require(state['scene']['primaryVertexCount'] == 4 and state['scene']['primaryIndexCount'] == 6, 'Reload duplicated/lost shared geometry')
    history_shape = authoring.history_cursor(state, 0, 0, allow_uncreated=True)
    expected_shape = tuple(key in baseline['gltfMaterialVariants'] for key in ('undoCount', 'redoCount'))
    require(history_shape == expected_shape, 'Never-authored reload history-counter shape changed')
    selected = state['gui']['selectedMeshNodeId']
    require(selected == INVALID if not first or scene == 2 else selected in nodes and nodes[selected]['renderable'] and nodes[selected]['active'],
        'Reload selection does not match fresh-load/scene-switch policy')
    return dict(passed=True, scene=scene, compactedSourceNodes=len(model['nodes']), stableRuntimeSlots=len(nodes), roots=expected_roots)


def linear(first, last, time):
    return np.asarray(first, dtype=np.float32).astype(float)+(np.asarray(last, dtype=np.float32).astype(float)-
        np.asarray(first, dtype=np.float32).astype(float))*(time/2.)


def dynamic_values(time):
    return dict(translation=linear([-1.25, 0., 0.], [-.75, .25, 0.], time), deletedWeight=float(linear(0., 1., time)),
        survivorWeight=float(linear(.2, .8, time)), color=linear([1., .25, .125, 1.], [.125, .75, 1., 1.], time),
        intensity=float(linear(20., 40., time)), fov=float(linear(.7, .9, time)))


def dynamic_layout(deformation, count):
    # This fixture supplies NORMAL but no authored TANGENT. Morph-frame
    # regeneration retains one independent vertex per literal triangle corner
    # [0,1,2,0,2,3], including both occurrences of shared source vertices 0/2.
    # The four bind vertices stay in the prefix; each owner adds six corners.
    corners = len(INDICES)
    require(deformation['active'] and deformation['gpuReady'] and deformation['currentBoundToGpu'] and deformation['previousBoundToGpu'] and
        deformation['instanceCount'] == count and len(deformation['instances']) == count and deformation['sourceVertexCount'] == len(POSITIONS) and
        deformation['expandedVertexCount'] == len(POSITIONS)+corners*count and deformation['expandedIndexCount'] == len(INDICES)+corners*count,
        'Actual surviving GPU deformation layout is not active/independent')
    vertex_ranges, index_ranges = [], []
    for row in deformation['instances']:
        require(row['vertexCount'] == corners and row['indexCount'] == corners and row['boundsValid'], 'Frame regeneration did not retain independent triangle corners')
        vertex_ranges.append((row['vertexOffset'], row['vertexOffset']+corners))
        index_ranges.append((row['firstIndex'], row['firstIndex']+corners))
        require(row['firstIndex']-len(INDICES) == row['vertexOffset']-len(POSITIONS), 'Deformation vertex/index owner ranges disagree')
    require(sorted(vertex_ranges) == [(len(POSITIONS)+corners*i, len(POSITIONS)+corners*(i+1)) for i in range(count)] and
        sorted(index_ranges) == [(len(INDICES)+corners*i, len(INDICES)+corners*(i+1)) for i in range(count)],
        'Deformation owners alias or omit a corner/index range')


def dynamic_state(state, baseline, name):
    animation = state['gltfAnimation']; before = baseline['gltfAnimation']; retired = name in ('deleted', 'resumed', 'redo')
    resumed = name == 'resumed'; time = animation['time']; saved = before['time']
    require(0.25 < saved < 1. and 0. < time < 2., 'Dynamic timeline did not advance within the independent linear interval')
    require(animation['playing'] is (name in ('before', 'resumed')) and animation['speed'] == 1. and animation['loop'] is True and
        animation['selectedClip'] == 0 and animation['clipCount'] == 1 and animation['diagnostics'] == 0, 'Deletion changed playback library/controls')
    require(time > saved+.1 if resumed else abs(time-saved) < 1e-7, 'Resume/undo did not advance or rewind the captured deletion-time pose')
    cursor = {'before': (0, 0), 'deleted': (1, 0), 'resumed': (1, 0), 'undo': (0, 1), 'redo': (1, 0)}[name]
    roots = baseline['gltfMerge']['activeRoots']; selected = INVALID if retired else baseline['gui']['selectedMeshNodeId']
    nodes, mapping, closure = stable_state(state, baseline, source_count=5, retired_root=0, retired=retired,
        scene=0, roots=roots, undo=cursor[0], redo=cursor[1], draws=1 if retired else 2, active_renderables=1 if retired else 2,
        selected=selected, dynamic=True, allow_uncreated=name == 'before')
    require(closure and baseline['gui']['selectedMeshNodeId'] in closure, 'Dynamic selection does not cover deletion')
    require(sorted(state['gltfScenes']['activeSourceNodes']) == ([2, 3, 4] if retired else list(range(5))), 'Dynamic active-source membership changed')
    expected = dynamic_values(time); graph_expected = dynamic_values(saved) if retired else expected
    actual_nodes = {row['sourceIndex']: row for row in animation['nodes']}
    require(set(actual_nodes) == {1, 2}, 'Missing actual typed source animation properties')
    close(actual_nodes[1]['translation'], expected['translation'], 'actual evaluated retired-owner source translation')
    close(actual_nodes[1]['weights'], [expected['deletedWeight']], 'actual evaluated retired-owner source weights')
    close(actual_nodes[2]['weights'], [expected['survivorWeight']], 'actual survivor source weights')
    local = np.eye(4); local[:3, 3] = graph_expected['translation']
    close(np.asarray(actual_nodes[1]['localMatrix']).reshape(4, 4).T, local, 'retired graph pose freeze/rewind')
    close(authoring.matrix(nodes[mapping[1]]), local, 'actual deleted-owner graph world')
    require(animation['selectedCameraNode'] == 4 and not animation['orthographic'], 'Surviving authored camera selection changed')
    close(state['camera']['position'], [0., 0., 6.], 'effective camera position')
    projection = np.zeros((4, 4)); focal = 1/math.tan(expected['fov']/2)
    projection[0, 0], projection[1, 1], projection[2, 2], projection[2, 3], projection[3, 2] = focal/(WIDTH/HEIGHT), focal, .1/99.9, 10/99.9, -1.
    close(np.asarray(animation['cameraProjection']).reshape(4, 4).T, projection, 'effective animated reverse-Z camera')
    close([animation['cameraNear'], animation['cameraFar']], [.1, 100.], 'effective camera clips')
    require(len(animation['pointLights']) == 1 and len(animation['materials']) == 1, 'Surviving light/material binding was lost')
    light = animation['pointLights'][0]
    close(light['position'], [0., 1., 3.], 'effective light position'); close(light['color'], [1., 1., 1.], 'effective light color')
    close([light['intensity'], light['range']], [expected['intensity'], 10.], 'effective light intensity/range')
    close(animation['materials'][0]['baseColor'], expected['color'], 'effective runtime material color')
    deformation = state['gltfDeformation']; count = 1 if retired else 2
    dynamic_layout(deformation, count)
    seen = set()
    for row in deformation['instances']:
        source = row['sourceNodeIndex']; require(source in ({2} if retired else {1, 2}) and source not in seen, 'Deformation source owner aliases/lost')
        seen.add(source); weight = expected['survivorWeight' if source == 2 else 'deletedWeight']
        positions = np.asarray(POSITIONS, dtype=np.float32).astype(float)+np.asarray([.5, .1, 0.], dtype=np.float32).astype(float)*weight
        close(row['weights'], [weight], 'bound morph pose'); close(row['firstPosition'], positions[0], 'independent morph vertex')
        close(row['firstNormal'], [0., 0., 1.], 'retained authored morph normal')
        close(row['boundsMin'], positions.min(axis=0), 'independent morph min'); close(row['boundsMax'], positions.max(axis=0), 'independent morph max')
        close(np.asarray(row['modelColumns']).T, authoring.matrix(nodes[mapping[source]]), 'deformed world binding')
    return dict(passed=True, name=name, time=time, savedTime=saved, playing=animation['playing'], sourcePoseNumeric=True,
        actualCameraProjection=True, actualLightAndMaterial=True, boundMorphStreams=count, retiredGraphPoseFrozen=retired)


def image_check(first, second, *, regions=None, contrast=False, minimum=0.):
    a, b = authoring.rgba(first), authoring.rgba(second)
    regions = regions or [[0, 0, WIDTH, HEIGHT]]; values = []
    for left, top, right, bottom in regions:
        channels = slice(0, 3) if contrast else slice(None)
        delta = np.abs(a[top:bottom, left:right, channels]-b[top:bottom, left:right, channels])
        values.append(dict(maximum=int(delta.max()), mean=float(delta.mean()), changedFraction=float(np.any(delta > 0, axis=2).mean())))
    if contrast:
        require(any(row['mean'] >= minimum and row['changedFraction'] >= .03 for row in values), 'Independent retirement image contrast is vacuous')
    else: require(all(row['maximum'] == 0 for row in values), 'Retirement image restoration/survivor oracle differs')
    return dict(passed=True, regions=regions, contrast=contrast, minimumMean=minimum, measurements=values)


def visible_control(record):
    pixels = authoring.rgba(record)
    def mean(roi):
        left, top, right, bottom = roi
        return pixels[top:bottom, left:right, :3].mean(axis=(0, 1))
    delta = float(np.abs(mean(RIGHT)-mean(BACKGROUND)).mean())
    require(delta >= 10., 'Independent surviving shared owner is not visibly rendered')
    return dict(passed=True, survivorBackgroundMeanDelta=delta, minimum=10.)


def ray_state(state, baseline, name, previous):
    retired = name in ('deleted', 'redo'); count = 1 if retired else 4
    mapping = source_mapping(authoring.rows(baseline), 6)
    cursor = {'before': (0, 0), 'deleted': (1, 0), 'undo': (0, 1), 'redo': (1, 0)}[name]
    stable_state(state, baseline, source_count=6, retired_root=1, retired=retired, scene=0, roots=[mapping[0]],
        undo=cursor[0], redo=cursor[1], draws=count, active_renderables=count,
        selected=INVALID if retired else baseline['gui']['selectedMeshNodeId'], allow_uncreated=name == 'before')
    ray, gi = state['rayShadows'], state['ddgi']; build = ray.get('build', {})
    require(ray['supported'] and ray['active'] and ray['mode'] == 1 and not ray['denoise'] and
        build.get('instances') == count and build.get('blasBuilt', 0)+build.get('blasReused', 0) > 0 and build.get('asAllocatedBytes', 0) > 0,
        'Actual hard-ray TLAS/BLAS does not match retirement membership')
    require(gi['supported'] and gi['enabled'] and gi['active'] and gi['contentEpoch'] > 0 and gi['updatedProbes'] > 0 and gi['tracedRays'] > 0 and
        gi['settings']['intensity'] == 0 and gi['settings']['counts'] == [3, 3, 3] and gi['settings']['raysPerProbe'] == 4,
        'No actual surface-backed DDGI update/trace')
    require(previous is None or ray['generation'] > previous[0] and gi['contentEpoch'] > previous[1],
        'Actual ray/DDGI content generation did not advance with retirement/replay')
    table = {row['name']: row for row in state['passes']}; times = {}
    for pass_name in ('RayShadowTrace', 'DdgiTrace', 'DdgiUpdate'):
        row = table.get(pass_name, {})
        require(row.get('active') and row.get('gpuTimed') and row.get('gpuMs', 0.) > 0., 'No actual GPU dispatch timestamp: '+pass_name)
        times[pass_name] = row['gpuMs']
    return dict(passed=True, name=name, TLASInstances=count, rayGeneration=ray['generation'], ddgiUpdatedProbes=gi['updatedProbes'],
        ddgiTracedRays=gi['tracedRays'], ddgiContentEpoch=gi['contentEpoch'], activeGpuPassMilliseconds=times,
        witness='execution only; GI intensity zero')


def dependencies():
    directory = Path(__file__).resolve().parent; result = {Path(__file__).resolve()}
    for module in tuple(sys.modules.values()):
        path = getattr(module, '__file__', None)
        if path and Path(path).resolve().parent == directory: result.add(Path(path).resolve())
    return {str(path): sha(path) for path in sorted(result)}


def launch(args, output, report, persist, verify, technique, name, source, labels, *, sequence_path=None,
           save=None, dynamic=False, ray=False, reject=False):
    verify(); target = output/'captures'/(technique+'-'+name); target.parent.mkdir(parents=True, exist_ok=True)
    require(not any(target.parent.glob(target.name+'*')), 'Refusing to overwrite native retirement evidence')
    image, log = target.with_suffix('.png'), target.with_suffix('.log')
    command = [str(args.renderer.resolve()), '--hidden', '--no-ui', '--validation', '--no-bloom', '--no-taa', '--width', str(WIDTH),
        '--height', str(HEIGHT), '--msaa', '1', '--warmup-frames', '12', '--capture-frame', '13', '--fixed-dt', '.016666667',
        '--render-technique', technique, '--display-mode', 'lit', '--exposure', '.35', '--camera-position', '0', '0', '6',
        '--camera-target', '0', '0', '0', '--camera-fov', '45', '--environment-intensity', '0', '--directional-intensity', '3' if ray else '0',
        '--import-scale', '1', '--gltf-scene', '0', '--model', str(source), '--screenshot', str(image)]
    command += ['--ray-shadows', 'hard', '--no-ray-shadow-denoise', '--ddgi', '--ddgi-counts', '3', '3', '3', '--ddgi-rays', '4',
        '--ddgi-probes-per-frame', '27', '--ddgi-ray-budget', '108', '--ddgi-intensity', '0'] if ray else ['--no-ray-query', '--ray-shadows', 'raster']
    if ray: command += ['--directional-direction', '0', '0', '-1']
    if dynamic: command += ['--animation-clip', '0', '--animation-time', '.25', '--animation-speed', '1', '--gltf-camera-node', '4']
    if sequence_path: command += ['--capture-sequence', str(sequence_path)]
    if save:
        save.parent.mkdir(parents=True, exist_ok=True); require(not save.exists(), 'Refusing to overwrite compacted export')
        command += ['--save-scene', str(save)]
    row = dict(name=name, technique=technique, command=command, log=str(log), completed=False, expectedRejection=reject)
    report['processes'].append(row); report['rendererExecuted'] = True; persist()
    with log.open('x', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=args.renderer.resolve().parent, stdout=stream, stderr=subprocess.STDOUT,
            timeout=args.timeout, env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)), creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    row.update(returnCode=process.returncode, completed=True, logSha256=sha(log)); persist()
    text = log.read_text(encoding='utf-8', errors='replace')
    require(not validation_log_error(text), 'Vulkan validation failed: '+str(log))
    if reject:
        require(process.returncode != 0 and 'Delete scene subtree:' in text and 'retained skin' in text,
            'Retained skin dependency was not rejected for the expected reason: '+str(log))
    else: require(process.returncode == 0, 'Native retirement process failed: '+str(log))
    records = {}
    # Rejected processes never reach the final screenshot rename; a baseline
    # already emitted by a sequence therefore keeps its .frame-NNNN suffix.
    for label, frame in labels:
        sampled = image if not reject and frame == labels[-1][1] else image.with_name(image.stem+f'.frame-{frame:04d}.png')
        telemetry = sampled.with_suffix('.telemetry.json')
        require(sampled.is_file() and telemetry.is_file(), 'Missing retirement sample: '+str(sampled))
        state = json.loads(telemetry.read_text(encoding='utf-8-sig'))
        require(finite_json(state) and state['technique'] == technique and state['resolution'] == [WIDTH, HEIGHT] and
            state['msaaSamples'] == 1 and not state['taa']['enabled'] and state['lighting']['environmentIntensity'] == 0 and
            any(pass_row['active'] and pass_row['gpuTimed'] for pass_row in state['passes']), 'Wrong capture settings or no actual GPU-timed frame')
        item = dict(name=label, technique=technique, frame=frame, image=str(sampled), telemetry=str(telemetry), log=str(log),
            imageSha256=sha(sampled), telemetrySha256=sha(telemetry), logSha256=sha(log), validationClean=True)
        report['captures'].append(item); records[label] = {**item, 'state': state}
    require({path.resolve() for path in target.parent.glob(target.name+'*.png')} == {Path(item['image']).resolve() for item in records.values()},
        'Wrong retirement sample count')
    persist(); verify(); return records


def run_native(args, output, plan, report):
    require(args.renderer.is_file() and args.manifest.is_file(), 'Missing explicit renderer/frozen manifest')
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/**/*')) and not any(output.glob('portable/**/*')), 'Native evidence directories must be fresh')
    if (output/'results.json').exists():
        old = json.loads((output/'results.json').read_text(encoding='utf-8')); require(old.get('generatedOnly') and not old.get('rendererExecuted'), 'Refusing to overwrite native results')
    manifest_hash = sha(args.manifest)
    def persist():
        for key in ('captures', 'stateChecks', 'imageChecks', 'exports', 'processes'):
            report[key+'Count'] = len(report[key])
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    def verify():
        require(sha(args.manifest) == manifest_hash, 'Retirement manifest changed')
        result = verify_manifest(args.manifest)
        require(dependencies() == report['sourceDependencies'] and sha(output/'launch-plan.json') == report['launchPlanSha256'], 'Harness/dependency/launch plan changed')
        for item in plan['immutableInputs']:
            path = Path(item['path']); require(path.stat().st_size == item['byteLength'] and sha(path) == item['sha256'], 'Immutable fixture/sequence changed')
        for copied in report['portableCopies']:
            require(all(sha(Path(item['path'])) == item['sha256'] for item in copied['files']), 'Portable resource changed')
        if 'runtimeShaderInputs' in report:
            require(sorted(str(path.resolve()) for path in (args.renderer.resolve().parent/'spv_shaders').glob('*.spv')) == report['runtimeShaderInputs'], 'Runtime shader inventory changed')
        return result
    def check(name, technique, result, *, pixels=False):
        report['imageChecks' if pixels else 'stateChecks'].append(dict(check=name, technique=technique, **result)); persist()
    try:
        report['nativeManifestBefore'] = verify_manifest(args.manifest)
        frozen = {Path(path).resolve() for path in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
        require(args.renderer.resolve() in frozen and {Path(path).resolve() for path in report['sourceDependencies']} <= frozen,
            'Manifest must bind selected executable and every local Python dependency')
        shaders = list((args.renderer.resolve().parent/'spv_shaders').glob('*.spv'))
        require(shaders and {path.resolve() for path in shaders} <= frozen, 'Manifest omits resident runtime SPIR-V')
        report['runtimeShaderInputs'] = sorted(str(path.resolve()) for path in shaders); report['rendererSha256'] = sha(args.renderer); persist(); verify()
        sources = {name: Path(path) for name, path in plan['sources'].items()}; scripts = {name: Path(path) for name, path in plan['scripts'].items()}
        for technique in args.technique:
            if args.cohort == 'ray-ddgi':
                captured = launch(args, output, report, persist, verify, technique, 'ray', sources['ray'], RAY, sequence_path=scripts['ray'], ray=True)
                baseline = captured['before']['state']; previous = None
                for name, _ in RAY:
                    result = ray_state(captured[name]['state'], baseline, name, previous)
                    previous = (result['rayGeneration'], result['ddgiContentEpoch']); check(name, technique, result)
                continue
            glb = output/'exports'/technique/'binary'/'scene.glb'
            captured = launch(args, output, report, persist, verify, technique, 'static', sources['static'], [(row[0], row[1]) for row in STATIC],
                sequence_path=scripts['static'], save=glb)
            baseline = captured['before']['state']
            for spec in STATIC: check(spec[0], technique, static_state(captured[spec[0]]['state'], baseline, spec, sources['static']))
            for name in ('undo-visible', 'undo-rename', 'redo-rename'): check(name+'-exact-before', technique, image_check(captured[name], captured['before']), pixels=True)
            for name in ('undo-delete', 'redo-visible'): check(name+'-exact-prior', technique, image_check(captured[name], captured['prior']), pixels=True)
            for name in ('redo-delete', 'empty'): check(name+'-exact-blank', technique, image_check(captured[name], captured['delete']), pixels=True)
            check('global-shared-membership', technique, image_check(captured['shared'], captured['show-survivor']), pixels=True)
            ascii_path = output/'exports'/technique/'ascii'/'scene.gltf'
            ascii_records = launch(args, output, report, persist, verify, technique, 'ascii', sources['static'], [('ascii-empty', 13)], sequence_path=scripts['ascii'], save=ascii_path)
            check('ascii-empty', technique, ascii_state(ascii_records['ascii-empty']['state'], sources['static']))
            check('ascii-exact-blank', technique, image_check(ascii_records['ascii-empty'], captured['empty']), pixels=True)
            control = launch(args, output, report, persist, verify, technique, 'control', sources['survivor'], [('control', 13)])['control']
            check('independent-survivor-visible', technique, visible_control(control), pixels=True)
            check('deleted-content-independent-contrast', technique, image_check(captured['before'], control, regions=[LEFT], contrast=True, minimum=3.), pixels=True)
            check('shared-survivor-independent-oracle', technique, image_check(captured['show-survivor'], control, regions=[RIGHT, LEFT, BACKGROUND]), pixels=True)
            for suffix, exported in (('glb', glb), ('gltf', ascii_path)):
                inspection = inspect_export(exported, sources['static']); report['exports'].append(dict(technique=technique, format=suffix, **inspection)); persist()
                copied, files = portable_copy(exported, output/'portable'/technique/suffix)
                report['portableCopies'].append(dict(technique=technique, format=suffix, document=str(copied), files=files)); persist()
                labels = [('reload-'+suffix+'-'+str(scene), frame) for scene, frame in enumerate((13, 26, 39))]
                reloaded = launch(args, output, report, persist, verify, technique, 'reload-'+suffix, copied, labels, sequence_path=scripts['reload'])
                for scene, (name, _) in enumerate(labels):
                    check(name, technique, reload_state(reloaded[name]['state'], copied, scene=scene, first=scene == 0,
                        baseline=reloaded[labels[0][0]]['state']))
                    check(name+'-pixels', technique, image_check(reloaded[name], captured['empty'] if scene == 2 else control,
                        regions=None if scene == 2 else [RIGHT, LEFT, BACKGROUND]), pixels=True)
            dynamic = launch(args, output, report, persist, verify, technique, 'dynamic', sources['dynamic'], DYNAMIC,
                sequence_path=scripts['dynamic'], dynamic=True)
            for name, _ in DYNAMIC: check('dynamic-'+name, technique, dynamic_state(dynamic[name]['state'], dynamic['before']['state'], name))
            check('dynamic-undo-exact-rewind', technique, image_check(dynamic['before'], dynamic['undo']), pixels=True)
            check('dynamic-redo-exact-rewind', technique, image_check(dynamic['deleted'], dynamic['redo']), pixels=True)
            check('dynamic-resume-effective-change', technique, image_check(dynamic['deleted'], dynamic['resumed'], regions=[RIGHT], contrast=True, minimum=.75), pixels=True)
            blocked = launch(args, output, report, persist, verify, technique, 'blocked', sources['blocked'], [('blocked-before', 13)],
                sequence_path=scripts['blocked'], reject=True)['blocked-before']['state']
            require(all(not row['retired'] for row in authoring.rows(blocked).values()), 'Blocked baseline already contains retired slots')
            authoring.history_cursor(blocked, 0, 0, allow_uncreated=True)
            check('retained-skin-rejected', technique, dict(passed=True, beforeAllLive=True, expectedGuardDiagnostic=True,
                postFailureStateObserved=False, rollbackClaim=False))
        require(len(report['captures']) == plan['capturesPerTechnique']*len(args.technique) and
            len(report['processes']) == plan['processesPerTechnique']*len(args.technique), 'Incomplete retirement capture/process cohort')
        require(len(report['stateChecks']) == (25 if args.cohort == 'core' else 4)*len(args.technique) and
            len(report['imageChecks']) == (21 if args.cohort == 'core' else 0)*len(args.technique) and
            len(report['exports']) == (2 if args.cohort == 'core' else 0)*len(args.technique), 'Incomplete retirement checks/export cohort')
        report['passed'] = report['nativeValidationPerformed'] = True
    except Exception as error:
        report['passed'] = False; report['failure'] = str(error); raise
    finally:
        try: report['nativeManifestAfter'] = verify(); report['afterFreezeVerified'] = True
        except Exception as error:
            report['afterFreezeVerified'] = False; report['freezeAuditFailure'] = str(error)
            if 'failure' not in report:
                report['passed'] = False; report['failure'] = 'After-run freeze audit failed: '+str(error); persist(); raise
        persist()


def synthetic_static(source, spec):
    """Hand-authored verifier witness, never reported as native evidence."""
    _, _, renamed, hidden, retired, undo, redo, scene = spec
    model, _ = load_asset(source); worlds, _ = forest_worlds(model)
    mapping = {0: 0, 1: 1, 3: 2, 5: 3, 4: 5, 2: 8}; nodes = []
    parents = {0: 10, 1: 0, 3: 1, 5: 2, 4: 1, 2: 0}
    for source_id, graph in mapping.items():
        nodes.append(dict(graphNode=graph, sourceNode=source_id, parentGraphNode=parents[source_id], meshOwnerGraphNode=INVALID,
            name=RENAMED if renamed and source_id == 1 else model['nodes'][source_id]['name'], renderable=False,
            retired=retired and source_id in (1, 3, 4, 5), active=False, worldColumns=worlds[source_id].T.tolist()))
    for graph, owner, source_id, x in [(4, 3, 5, None), (6, 5, 4, -.5), (7, 5, 4, .5), (9, 8, 2, None)]:
        local = np.eye(4)
        if x is not None: local = np.diag([.5, .5, 1., 1.]); local[0, 3] = x
        nodes.append(dict(graphNode=graph, sourceNode=-1, parentGraphNode=owner, meshOwnerGraphNode=owner, name='Primitive 0',
            renderable=True, retired=retired and source_id != 2, active=False, worldColumns=(worlds[source_id]@local).T.tolist()))
    nodes.append(dict(graphNode=10, sourceNode=-1, parentGraphNode=INVALID, meshOwnerGraphNode=INVALID, name='Scene Root',
        renderable=False, retired=False, active=False, worldColumns=np.eye(4).T.tolist()))
    index = {row['graphNode']: row for row in nodes}; closure = descendants(index, 1)
    active = descendants(index, 0) if scene != 2 else set()
    if retired: active -= closure
    survivor = descendants(index, 8); flags = []
    for row in nodes:
        graph = row['graphNode']; row['active'] = graph in active
        flags.append(dict(graphNode=graph, retired=row['retired'], localVisible=not hidden if row['sourceNode'] == 2 else True,
            localSelectable=True, localHoverable=True, effectiveVisible=graph in active and (not hidden or graph not in survivor)))
    draws = (0 if retired else 3)+int(not hidden) if scene != 2 else 0
    active_count = (1 if retired else 4) if scene != 2 else 0
    instances = [] if retired or scene == 2 else [dict(sourceNode=4, modelColumns=index[graph]['worldColumns']) for graph in (6, 7)]
    return dict(gltfMerge=dict(retainedNodes=nodes, graphIdentity=19, graphNodeCount=11, retainedNodesTruncated=False,
            sourceTables=copy.deepcopy(STATIC_TABLES), activeRoots=[0] if scene != 2 else [], sourceReadCount=0,
            generationPublications=0, requestId=0, outcome='idle', error='', descriptorGeneration=0, commitGeneration=0),
        gltfScenes=dict(graphIdentity=19, authoringNodeCount=11, selected=scene, activeRenderableCount=active_count,
            names=['Shared A', 'Shared B', 'Empty'], activeSourceNodes=[] if scene == 2 else [0, 2] if retired else list(range(6)), editHistoryDepth=undo+redo),
        scene=dict(primaryObjectCount=active_count, primaryImportScale=1.), gltfInstancing=dict(drawCount=draws, instances=instances),
        gltfMaterialVariants=dict(undoCount=undo, redoCount=redo), gltfNodeFlags=dict(truncated=False, nodes=flags),
        gui=dict(selectedMeshNodeId=INVALID if retired else 4, modelLoading=dict(pending=False)))


def self_check():
    with tempfile.TemporaryDirectory(prefix='subtree-retirement-selfcheck-') as temporary:
        root = Path(temporary); plan = generate(root/'first', 'core'); again = generate(root/'again', 'core')
        require([row['sha256'] for row in plan['immutableInputs']] == [row['sha256'] for row in again['immutableInputs']], 'Nondeterministic immutable retirement fixtures')
        source = Path(plan['sources']['static']); model, buffers = load_asset(source)
        worlds, _ = forest_worlds(model)
        close(worlds[2][:3, 3], [1.25, 0., 0.], 'literal survivor translation', 0.)
        attrs = model['nodes'][4]['extensions'][INSTANCING]['attributes']; accessor = model['accessors'][attrs['_ID']]
        view = model['bufferViews'][accessor['bufferView']]; start = view.get('byteOffset', 0)+accessor.get('byteOffset', 0)
        require(struct.unpack_from('<II', buffers[view['buffer']], start) == (16777217, 16777219), 'UINT32 instance fixture lost exact integer identities')
        dynamic = load_asset(Path(plan['sources']['dynamic']))[0]
        attributes = dynamic['meshes'][0]['primitives'][0]['attributes']
        require('NORMAL' in attributes and 'TANGENT' not in attributes,
            'Dynamic fixture no longer requires independent tangent-frame corners')
        require(len(dynamic['animations'][0]['channels']) == 6 and dynamic_values(.5)['deletedWeight'] == .25 and
            dynamic_values(.75)['intensity'] > dynamic_values(.5)['intensity'], 'Dynamic independent fixture/control is vacuous')
        for channel in dynamic['animations'][0]['channels']:
            sampler = dynamic['animations'][0]['samplers'][channel['sampler']]
            close(decode_accessor(Path(plan['sources']['dynamic']), sampler['input']).reshape(-1), [0., 2.], 'animation time bounds', 0.)
        baseline = synthetic_static(source, STATIC[0])
        for spec in STATIC: static_state(synthetic_static(source, spec), baseline, spec, source)
        rejected = []
        for name, mutate in [('changed-slot', lambda s: s['gltfMerge']['retainedNodes'][0].__setitem__('graphNode', 42)),
                ('retired-live', lambda s: s['gltfMerge']['retainedNodes'][1].__setitem__('retired', False)),
                ('selected-deleted', lambda s: s['gui'].__setitem__('selectedMeshNodeId', 4)),
                ('wrong-history', lambda s: s['gltfMaterialVariants'].__setitem__('undoCount', 0)),
                ('wrong-roots', lambda s: s['gltfMerge'].__setitem__('activeRoots', [8])),
                ('wrong-parent', lambda s: s['gltfMerge']['retainedNodes'][1].__setitem__('parentGraphNode', 10))]:
            bad = synthetic_static(source, STATIC[2]); mutate(bad)
            try: static_state(bad, baseline, STATIC[2], source)
            except (RuntimeError, KeyError, IndexError): rejected.append(name)
            else: raise RuntimeError('Retirement verifier accepted mutation: '+name)
        bad = synthetic_static(source, STATIC[-1]); bad['gltfMaterialVariants'] = {}
        try: static_state(bad, baseline, STATIC[-1], source)
        except RuntimeError: rejected.append('missing-post-authoring-zero-history')
        else: raise RuntimeError('Post-authoring missing history counters were accepted')
        def layout(count):
            return dict(active=True, gpuReady=True, currentBoundToGpu=True, previousBoundToGpu=True,
                instanceCount=count, sourceVertexCount=4, expandedVertexCount=4+6*count, expandedIndexCount=6+6*count,
                instances=[dict(vertexCount=6, indexCount=6, boundsValid=True, vertexOffset=4+6*i, firstIndex=6+6*i) for i in range(count)])
        dynamic_layout(layout(1), 1); dynamic_layout(layout(2), 2)
        for name, mutate in [('unsplit-four-vertex-layout', lambda s: s.__setitem__('expandedVertexCount', 12)),
                ('unsplit-four-vertex-owner', lambda s: s['instances'][0].__setitem__('vertexCount', 4)),
                ('aliased-owner-vertices', lambda s: s['instances'][1].__setitem__('vertexOffset', 4)),
                ('aliased-owner-indices', lambda s: s['instances'][1].__setitem__('firstIndex', 6)),
                ('owner-range-gap', lambda s: s['instances'][1].update(vertexOffset=16, firstIndex=18)),
                ('source-prefix-alias', lambda s: s['instances'][0].update(vertexOffset=0, firstIndex=2)),
                ('missing-owner-corner', lambda s: s['instances'][1].__setitem__('vertexCount', 5)),
                ('missing-output-index', lambda s: s.__setitem__('expandedIndexCount', 17))]:
            bad = layout(2); mutate(bad)
            try: dynamic_layout(bad, 2)
            except RuntimeError: rejected.append(name)
            else: raise RuntimeError('Corner layout verifier accepted mutation: '+name)
        # A synthetic compacted file exercises the export checker independently
        # of production: explicit retained groups, mesh child and orphan editor.
        compact = copy.deepcopy(model); parent, survivor = copy.deepcopy(model['nodes'][0]), copy.deepcopy(model['nodes'][2])
        parent['children'] = [1]; survivor.pop('mesh'); survivor['children'] = [2]
        compact['nodes'] = [parent, survivor, dict(name='Primitive 0', mesh=0), dict(name='Scene Root')]; compact['scene'] = 2
        path = root/'compact.gltf'; document(path, compact); inspect_export(path, source)
        for name, mutate in [('retired-metadata', lambda m: m['nodes'][1]['extras'].__setitem__('retirementTag', 'delete')),
                ('wrong-root', lambda m: m['scenes'][1].__setitem__('nodes', [1])),
                ('dangling-child', lambda m: m['nodes'][0].__setitem__('children', [8])),
                ('wrong-world', lambda m: m['nodes'][1].__setitem__('translation', [9., 0., 0.])),
                ('wrong-default', lambda m: m.__setitem__('scene', 0))]:
            bad = copy.deepcopy(compact); mutate(bad); candidate = root/(name+'.gltf'); document(candidate, bad)
            try: inspect_export(candidate, source)
            except (RuntimeError, KeyError, IndexError): rejected.append(name)
            else: raise RuntimeError('Retirement verifier accepted mutation: '+name)
        # A graph witness uses non-prefix IDs and an explicit root. Exercise
        # closure/reference checks without a renderer-generated oracle.
        graph = {7: dict(parentGraphNode=INVALID), 2: dict(parentGraphNode=7), 9: dict(parentGraphNode=2), 4: dict(parentGraphNode=7)}
        require(descendants(graph, 2) == {2, 9}, 'Closure verifier assumes prefix IDs')
        graph[7]['parentGraphNode'] = 9
        try: descendants(graph, 2)
        except RuntimeError: rejected.append('cyclic-runtime-forest')
        else: raise RuntimeError('Runtime closure verifier accepted a cycle')
        require(all('name' not in event['nodeAuthoring'] for name in ('static', 'ascii', 'dynamic', 'blocked', 'ray')
            for event in json.loads(Path(plan['scripts'][name]).read_text())['events']
            if event.get('nodeAuthoring', {}).get('action') == 'delete-subtree'), 'Deletion JSON contains a forbidden name')
        require(len(STATIC) == 12 and len(DYNAMIC) == 5 and len(RAY) == 4, 'Capture completeness constants changed')
    return dict(passed=True, rendererExecuted=False, deterministicImmutableInputs=True, literalWorldsAndUINT32=True,
        independentAnimationInputBounds=True, staticHistoryStatesVerified=12, verifierMutationsRejected=rejected,
        plannedCoreCapturesPerTechnique=26, plannedRayCapturesPerTechnique=4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path); parser.add_argument('--manifest', type=Path); parser.add_argument('--output', type=Path)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES); parser.add_argument('--cohort', choices=('core', 'ray-ddgi'), default='core')
    parser.add_argument('--timeout', type=float, default=300.); parser.add_argument('--generate-only', action='store_true'); parser.add_argument('--self-check', action='store_true')
    args = parser.parse_args()
    if args.self_check: print(json.dumps(self_check(), indent=2)); return
    if args.output is None: parser.error('--output is required')
    if not args.generate_only and (args.renderer is None or args.manifest is None): parser.error('Native execution requires explicit --renderer and --manifest')
    if not math.isfinite(args.timeout) or args.timeout <= 0: parser.error('--timeout must be finite and positive')
    args.technique = tuple(dict.fromkeys(args.technique or TECHNIQUES)); output = args.output.resolve(); plan = generate(output, args.cohort)
    report = dict(schemaVersion=1, cohort=args.cohort, passed=None if args.generate_only else False, generatedOnly=args.generate_only,
        rendererExecuted=False, nativeValidationPerformed=False, sourceDependencies=dependencies(), launchPlanSha256=sha(output/'launch-plan.json'),
        renderer=str(args.renderer.resolve()) if args.renderer else None, manifest=str(args.manifest.resolve()) if args.manifest else None,
        expectedCaptureCount=plan['capturesPerTechnique']*len(args.technique), expectedProcessCount=plan['processesPerTechnique']*len(args.technique),
        captures=[], stateChecks=[], imageChecks=[], exports=[], processes=[], portableCopies=[], unverified=UNVERIFIED)
    if args.generate_only:
        document(output/'results.json', report); print('GENERATED', report['expectedCaptureCount'], 'planned captures; renderer not executed')
    else:
        run_native(args, output, plan, report); print('PASS', len(report['captures']), 'captures;', len(report['stateChecks']), 'state checks;', len(report['imageChecks']), 'image checks')


if __name__ == '__main__': main()
