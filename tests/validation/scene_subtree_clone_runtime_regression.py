"""Independent native checks for graph-driven subtree duplication.

Graph-only edits use the same typed capture requests as the editor. Literal
world-space controls do not call the production clone/deformation planners.
Separate cohorts bound edited graphs, animation/skin, primitive membership and
actual hard-ray/DDGI execution. No generation/self-check launches a renderer.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
import os
from pathlib import Path
import re
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
COHORTS = ('edited', 'primitive', 'animated', 'binding', 'ray-ddgi')
WIDTH, HEIGHT, INVALID = 640, 360, 2**32-1
INSTANCING, PUNCTUAL = 'EXT_mesh_gpu_instancing', 'KHR_lights_punctual'
E, F, OUTSIDE, PROCEDURAL = 'Graph empty E', 'Graph empty F', 'Outside owner', 'Procedural child'
COPY, EMPTY_COPY = 'Edited copy \u6771', 'Detached owner A copy'
SHEAR = np.asarray([[1., .125, 0., 0.], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]])
QUAD = np.asarray([[-.25, -.25, 0.], [.25, -.25, 0.], [.25, .25, 0.], [-.25, .25, 0.]])
INDICES = [0, 1, 2, 0, 2, 3]
OFFSET = 2.5
SOURCE_TABLES = dict(nodes=3, meshes=2, materials=3, images=0, textures=0, samplers=0, buffers=1, scenes=3)
EDITED = [('loaded', 13), ('prepared', 26), ('copy', 39), ('offset', 52), ('undo-transform', 65),
    ('undo-clone', 78), ('redo-clone', 91), ('redo-transform', 104), ('empty-owner', 117),
    ('undo-empty', 130), ('redo-empty', 143), ('shared', 156), ('empty', 169)]
PRIMITIVE = [('created', 13), ('undo', 26), ('redo', 39), ('undo-again', 52), ('replacement', 65), ('other-scene', 78)]
RAY = [('prepared', 26), ('copy', 39), ('undo', 52), ('redo', 65)]
ANIMATED = [('before', 13), ('copy', 26), ('offset', 39), ('seek', 52), ('undo-transform', 65),
    ('undo-clone', 78), ('redo-clone', 91), ('redo-transform', 104), ('zero', 117), ('rest', 130), ('reselect', 143)]
ANIMATION_NAME = 'Animated skin owner'
JOINT_PARENT = 'External visible joint ancestor'
JOINT = 'External animated joint'
POINTER = 'KHR_animation_pointer'
UNVERIFIED = [
    'Generated inputs/self-check do not establish native rendering or GPU execution.',
    'Fixed inset image regions exclude selection outlines; full-image equality is used only with matching selection.',
    'CPU deformation samples describe bound streams, not direct GPU vertex readback.',
    'A moved skinned primitive retains the existing explicit export rejection; its live binding-context coverage is separate from normally parented portable skin coverage.',
    'Physics duplication, allocation failure injection and resource leak counts are outside this cohort.',
    'Per-light manual override mutation is unavailable in the capture API; only observable retained/duplicated authored binding and CLI lighting controls are claimed.']


def close(actual, expected, label, tolerance=3e-5):
    a, b = np.asarray(actual, dtype=float), np.asarray(expected, dtype=float)
    require(a.shape == b.shape and np.isfinite(a).all() and np.isfinite(b).all() and
        np.max(np.abs(a-b), initial=0.) <= tolerance, 'Incorrect subtree clone '+label)


def translation(x=0., y=0., z=0.):
    result = np.eye(4); result[:3, 3] = [x, y, z]
    return result


def transformed(points, matrix):
    return (matrix@np.column_stack((points, np.ones(len(points)))).T).T[:, :3]


def exact_uint32(path, index):
    model, buffers = load_asset(path); accessor = model['accessors'][index]
    require(accessor['componentType'] == 5125 and accessor['type'] == 'SCALAR' and not accessor.get('normalized', False) and
        'sparse' not in accessor, 'Expected plain exact custom UINT32 instance accessor')
    view = model['bufferViews'][accessor['bufferView']]; offset = view.get('byteOffset', 0)+accessor.get('byteOffset', 0)
    return [struct.unpack_from('<I', buffers[view['buffer']], offset+i*view.get('byteStride', 4))[0] for i in range(accessor['count'])]


def write_asset(asset, path):
    with tempfile.TemporaryDirectory(prefix='subtree-clone-asset-') as temporary:
        generated = Path(temporary)/'scene.gltf'; asset.write(generated)
        immutable(path, generated.read_bytes())


def material(name, color, *, lit=False):
    result = dict(name=name, extras=dict(cloneTag=name), pbrMetallicRoughness=dict(
        baseColorFactor=[*color, 1.], metallicFactor=0., roughnessFactor=1.))
    if not lit: result['extensions'] = {'KHR_materials_unlit': {}}
    return result


def primitive(asset, points, mat, tag, *, morph=False, skin=False):
    attributes = dict(POSITION=asset.accessor('VEC3', points), NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*len(points)),
        TANGENT=asset.accessor('VEC4', [[1., 0., 0., 1.]]*len(points)))
    if skin:
        attributes.update(JOINTS_0=asset.accessor('VEC4', [[0, 0, 0, 0]]*len(points), 5121),
            WEIGHTS_0=asset.accessor('VEC4', [[1., 0., 0., 0.]]*len(points)))
    result = dict(attributes=attributes, material=mat, indices=asset.accessor('SCALAR', INDICES, 5123, indices=True),
        mode=4, extras=dict(cloneTag=tag, opaqueIndex=991))
    if morph: result['targets'] = [dict(POSITION=asset.accessor('VEC3', [[.5, .125, 0.]]*len(points)))]
    return result


def edited_fixture(path, *, lit=False):
    asset = Asset()
    asset.model['materials'] = [material('Orange upper', [1., .25, .125], lit=lit),
        material('Green retained', [.125, 1., .25], lit=lit), material('Blue instances', [.125, .25, 1.], lit=lit)]
    asset.model['meshes'] = [dict(name='Two source primitives', extras=dict(cloneTag='mesh-a'), primitives=[
        primitive(asset, QUAD+[0., .75, 0.], 0, 'a0'), primitive(asset, QUAD+[0., -.75, 0.], 1, 'a1')]),
        dict(name='Instanced source primitive', extras=dict(cloneTag='mesh-b'), primitives=[primitive(asset, QUAD, 2, 'b0')])]
    asset.model['nodes'] = [dict(name='Shared root', matrix=SHEAR.T.reshape(-1).tolist(), children=[1, 2], extras=dict(cloneTag='root')),
        dict(name='Detached owner A', mesh=0, translation=[-1.25, 0., 0.], extras=dict(cloneTag='owner-a')),
        dict(name='Instanced owner B', mesh=1, translation=[-1.25, -.75, 0.], extras=dict(cloneTag='owner-b'), extensions={INSTANCING: dict(
            attributes=dict(TRANSLATION=asset.accessor('VEC3', [[-.75, 0., 0.], [.75, 0., 0.]]),
                SCALE=asset.accessor('VEC3', [[.5, .5, 1.]]*2), _ID=asset.accessor('SCALAR', [16777217, 16777219], 5125)),
            extras=dict(opaqueInstanceIndex=977))})]
    asset.model['scenes'] = [dict(name='Shared A', nodes=[0], extras=dict(cloneTag='scene-a')),
        dict(name='Shared B', nodes=[0], extras=dict(cloneTag='scene-b')),
        dict(name='Empty', nodes=[], extras=dict(cloneTag='scene-empty'))]
    asset.model['extensionsUsed'] = [INSTANCING]+([] if lit else ['KHR_materials_unlit'])
    write_asset(asset, path)


def edited_world_quads(copied):
    """Literal world vertices, excluding procedural MaterialX appearance."""
    result = [(transformed(QUAD+[0., .75, 0.], SHEAR@translation(-1.25)), 0, 'a0'),
        (transformed(QUAD+[0., -.75, 0.], SHEAR@translation(-1.25)), 1, 'a1')]
    for x in (-.75, .75):
        matrix = SHEAR@translation(-1.25, -.75)@translation(x)@np.diag([.5, .5, 1., 1.])
        result.append((transformed(QUAD, matrix), 2, 'b0'))
    if copied:
        result += [(points+[OFFSET, 0., 0.], mat, tag) for points, mat, tag in list(result) if tag != 'a1']
    return result


def edited_control(path, copied):
    asset = Asset(); asset.model['materials'] = [material('Orange upper', [1., .25, .125]),
        material('Green retained', [.125, 1., .25]), material('Blue instances', [.125, .25, 1.])]
    asset.model['nodes'], asset.model['meshes'] = [], []
    for index, (points, mat, tag) in enumerate(edited_world_quads(copied)):
        asset.model['meshes'].append(dict(primitives=[primitive(asset, points, mat, tag)]))
        asset.model['nodes'].append(dict(name='Literal baked '+str(index), mesh=index))
    asset.model['scenes'] = [dict(name='Independent baked control', nodes=list(range(len(asset.model['nodes']))))]
    asset.model['extensionsUsed'] = ['KHR_materials_unlit']; write_asset(asset, path)


def dynamic_fixture(path):
    asset = Asset()
    asset.model['materials'] = [material('Animated morph material', [1., .25, .125]), material('External bystander', [.125, .25, 1.])]
    asset.model['meshes'] = [dict(name='One morph skin', weights=[.15], primitives=[primitive(asset, QUAD, 0, 'skin', morph=True, skin=True)]),
        dict(name='Ancestor visual attachment', primitives=[primitive(asset, QUAD+[2., -1., 0.], 1, 'bystander')])]
    asset.model['cameras'] = [dict(name='Animated perspective', type='perspective', perspective=dict(yfov=.7, znear=.1, zfar=100.))]
    asset.model['extensions'] = {PUNCTUAL: dict(lights=[dict(name='Shared animated point', type='point', intensity=20., range=10., color=[1., 1., 1.])])}
    asset.model['nodes'] = [dict(name=ANIMATION_NAME, mesh=0, skin=0, weights=[.15], translation=[-1.25, 0., 0.], extras=dict(cloneTag='skin-owner')),
        dict(name=JOINT_PARENT, mesh=1, camera=0, children=[2], translation=[0., .25, 0.],
            extensions={PUNCTUAL: dict(light=0)}, extras=dict(cloneTag='joint-context')),
        dict(name=JOINT, translation=[-.25, -.25, 0.], extras=dict(cloneTag='joint')),
        dict(name='Selected camera', camera=0, translation=[0., 0., 6.]),
        dict(name='Authored lamp', translation=[0., 1.5, 3.], extensions={PUNCTUAL: dict(light=0)})]
    asset.model['skins'] = [dict(name='External joint skin', joints=[2], skeleton=1,
        inverseBindMatrices=asset.accessor('MAT4', [translation(-1.).T.reshape(-1).tolist()]))]
    asset.model['scenes'] = [dict(name='Animated clone', nodes=[0, 1, 3, 4])]
    times = asset.accessor('SCALAR', [0., 2.]); samplers, channels = [], []
    def channel(kind, values, target):
        samplers.append(dict(input=times, output=asset.accessor(kind, values), interpolation='LINEAR'))
        channels.append(dict(sampler=len(samplers)-1, target=target))
    channel('VEC3', [[0., 0., 0.], [.5, .25, 0.]], dict(node=2, path='translation'))
    channel('SCALAR', [.2, .8], dict(node=0, path='weights'))
    channel('SCALAR', [1., 1.5], dict(path='pointer', extensions={POINTER: dict(pointer='/nodes/0/scale/0')}))
    channel('VEC4', [[1., .25, .125, 1.], [.125, .75, 1., 1.]], dict(path='pointer', extensions={POINTER: dict(pointer='/materials/0/pbrMetallicRoughness/baseColorFactor')}))
    channel('SCALAR', [20., 40.], dict(path='pointer', extensions={POINTER: dict(pointer='/extensions/KHR_lights_punctual/lights/0/intensity')}))
    channel('SCALAR', [.7, .9], dict(path='pointer', extensions={POINTER: dict(pointer='/cameras/0/perspective/yfov')}))
    asset.model['animations'] = [dict(name='Original clip with cloned node suffix', samplers=samplers, channels=channels)]
    asset.model['extensionsUsed'] = ['KHR_materials_unlit', POINTER, PUNCTUAL]
    write_asset(asset, path)


def linear(first, last, time):
    # Source endpoints are independently narrowed to their actual accessor FLOATs.
    a, b = np.asarray(first, np.float32).astype(float), np.asarray(last, np.float32).astype(float)
    return np.asarray(a+(b-a)*(time/2.), np.float32).astype(float)


def dynamic_values(time):
    rest = time is None
    return dict(weight=float(np.float32(.15)) if rest else float(linear(.2, .8, time)),
        scale=1. if rest else float(linear(1., 1.5, time)),
        joint=np.asarray([-.25, -.25, 0.]) if rest else linear([0., 0., 0.], [.5, .25, 0.], time),
        color=np.asarray([1., .25, .125, 1.]) if rest else linear([1., .25, .125, 1.], [.125, .75, 1., 1.], time),
        intensity=20. if rest else float(linear(20., 40., time)), fov=.7 if rest else float(linear(.7, .9, time)))


def dynamic_geometry(time, *, binding=False, copied=False, offset=False):
    values = dynamic_values(time)
    owner = translation(-1.25)@np.diag([values['scale'], 1., 1., 1.])
    joint_world = translation(0., .25)@translation(*values['joint'])
    positions = QUAD+np.asarray([.5, .125, 0.])*values['weight']
    local = transformed(positions, np.linalg.inv(owner)@joint_world@translation(-1.))
    draw_model = translation(-1.25)@np.diag([dynamic_values(.5)['scale'], 1., 1., 1.]) if binding else owner
    result = [(local, draw_model, transformed(local, draw_model), values['color'])]
    if copied:
        if binding:
            clone_model = translation(OFFSET if offset else 0.)@draw_model
            clone_local = local
        else:
            clone_model = owner
            clone_local = transformed(positions, np.linalg.inv(owner)@translation(OFFSET if offset else 0.)@joint_world@translation(-1.))
        result.append((clone_local, clone_model, transformed(clone_local, clone_model), values['color']))
    return result


def dynamic_control(path, time, *, binding=False, copied=False, offset=False):
    asset = Asset(); values = dynamic_values(time)
    asset.model['materials'] = [material('Baked animated', values['color'][:3].tolist()), material('External bystander', [.125, .25, 1.])]
    worlds = [(row[2], 0) for row in dynamic_geometry(time, binding=binding, copied=copied, offset=offset)]
    worlds.append((QUAD+[2., -.75, 0.], 1))
    asset.model['meshes'] = [dict(primitives=[primitive(asset, points, mat, 'literal')]) for points, mat in worlds]
    asset.model['nodes'] = [dict(name='Literal skinned world '+str(i), mesh=i) for i in range(len(worlds))]
    asset.model['cameras'] = [dict(type='perspective', perspective=dict(yfov=values['fov'], znear=.1, zfar=100.))]
    camera = len(asset.model['nodes']); asset.model['nodes'].append(dict(name='Literal effective camera', camera=0, translation=[0., 0., 6.]))
    asset.model['scenes'] = [dict(nodes=list(range(len(asset.model['nodes']))))]
    asset.model['extensionsUsed'] = ['KHR_materials_unlit']; write_asset(asset, path)
    return camera


def setup_events(start=14):
    # Initial deterministic authored graph is explicitly witnessed by loaded13:
    # R0,A1,a0=2,a1=3,B4,instance5/6,editor7. Appends E8,F9,Out10,P11.
    return [dict(frame=start, nodeAuthoring=dict(action='create-empty', sourceNode=0, name=E)),
        dict(frame=start+1, nodeAuthoring=dict(action='create-empty', graphNode=8, name=F)),
        dict(frame=start+2, nodeAuthoring=dict(action='create-empty', sourceNode=0, name=OUTSIDE)),
        dict(frame=start+3, addPrimitive=dict(kind='quad', name=PROCEDURAL, parent=dict(graphNode=9))),
        dict(frame=start+4, nodeReparent=dict(node=dict(graphNode=2), parent=dict(graphNode=8))),
        dict(frame=start+5, nodeReparent=dict(node=dict(graphNode=3), parent=dict(graphNode=10))),
        dict(frame=start+6, nodeReparent=dict(node=dict(sourceNode=2), parent=dict(graphNode=9))),
        dict(frame=start+7, nodeFlags=dict(sourceNode=2, hoverable=False))]


def duplicate(frame, *, graph=None, source=None, name=None):
    request = dict(action='duplicate-subtree')
    request['graphNode' if graph is not None else 'sourceNode'] = graph if graph is not None else source
    if name is not None: request['name'] = name
    return dict(frame=frame, nodeAuthoring=request)


def transform_event(frame, graph, matrix):
    return dict(frame=frame, nodeTransform=dict(node=dict(graphNode=graph), matrix=np.asarray(matrix).T.reshape(-1).tolist()))


def script(frames, labels, events, *, camera=True):
    value = dict(schemaVersion=1, frames=frames, sampleFrames=[row[1] for row in labels], events=events)
    if camera: value['camera'] = [dict(frame=1, position=[0., 0., 6.], target=[0., 0., 0.])]
    return value


def generate(output, cohort):
    output.mkdir(parents=True, exist_ok=True)
    sources = {name: output/'sources'/(name+'.gltf') for name in ('edited', 'ray', 'baked-before', 'baked-copy', 'animated')}
    edited_fixture(sources['edited']); edited_fixture(sources['ray'], lit=True)
    edited_control(sources['baked-before'], False); edited_control(sources['baked-copy'], True)
    dynamic_fixture(sources['animated'])
    controls = {}
    for kind in ('animated', 'binding'):
        for name, time, copied, offset in [('before', .5, False, False), ('copy', .5, True, False), ('offset', .5, True, True),
                ('seek', 1., True, True), ('undo-transform', 1., True, False), ('zero', 0., True, True), ('rest', None, True, True)]:
            key = kind+'-control-'+name; sources[key] = output/'sources'/(key+'.gltf')
            controls[key] = dynamic_control(sources[key], time, binding=kind == 'binding', copied=copied, offset=offset)
    scripts = {
        'edited': script(169, EDITED, [*setup_events(), duplicate(27, graph=8, name=COPY), transform_event(40, 18, translation(OFFSET)),
            *[dict(frame=frame, sceneHistory=action) for frame, action in [(53, 'undo'), (66, 'undo'), (79, 'redo'), (92, 'redo')]],
            duplicate(105, source=1), dict(frame=118, sceneHistory='undo'), dict(frame=131, sceneHistory='redo'),
            dict(frame=144, gltfScene=1), dict(frame=157, gltfScene=2)]),
        'ascii': script(26, [('ascii-empty', 26)], [*setup_events(2), duplicate(10, graph=8, name=COPY),
            transform_event(11, 18, translation(OFFSET)), duplicate(12, source=1), dict(frame=14, gltfScene=2)]),
        'reload': script(39, [('shared-a', 13), ('shared-b', 26), ('empty', 39)], [dict(frame=14, gltfScene=1), dict(frame=27, gltfScene=2)]),
        'primitive': script(78, PRIMITIVE, [dict(frame=1, gltfScene=2),
            dict(frame=2, addPrimitive=dict(kind='quad', name='Temporary empty-scene quad', parent=dict(graphNode=7))),
            dict(frame=14, sceneHistory='undo'), dict(frame=27, sceneHistory='redo'), dict(frame=40, sceneHistory='undo'),
            dict(frame=53, gltfScene=0), dict(frame=54, addPrimitive=dict(kind='quad', name='Replacement other scene', parent=dict(sourceNode=0))),
            dict(frame=66, gltfScene=2)]),
        'ray': script(65, RAY, [*setup_events(), duplicate(27, graph=8, name=COPY),
            dict(frame=40, sceneHistory='undo'), dict(frame=53, sceneHistory='redo')])}
    for kind in ('animated', 'binding'):
        is_binding = kind == 'binding'
        setup = [dict(frame=14, nodeAuthoring=dict(action='create-empty', graphNode=7, name='Binding empty')),
            dict(frame=15, nodeReparent=dict(node=dict(graphNode=1), parent=dict(graphNode=8)))] if is_binding else []
        # Binding preparation consumes one additional sample before duplication.
        shift = 13 if is_binding else 0
        labels = [('loaded', 13), ('before', 26)]+[(name, frame+13) for name, frame in ANIMATED[1:]] if is_binding else ANIMATED
        scripts[kind] = script(143+shift, labels, [dict(frame=2, animationPlaying=False), *setup,
            duplicate(14+shift, graph=8 if is_binding else 0, name='Binding copy' if is_binding else 'Animated copy'),
            transform_event(27+shift, 13 if is_binding else 10, translation(OFFSET, 0. if is_binding else .25)),
            dict(frame=40+shift, animationTime=1.),
            *[dict(frame=frame+shift, sceneHistory=action) for frame, action in [(53, 'undo'), (66, 'undo'), (79, 'redo'), (92, 'redo')]],
            dict(frame=105+shift, animationTime=0.), dict(frame=118+shift, animationClip=-1),
            dict(frame=131+shift, animationClip=0, animationTime=.5, animationPlaying=False)], camera=False)
    scripts['animated-ascii'] = script(13, [('ascii', 13)], [dict(frame=1, animationPlaying=False),
        duplicate(2, graph=0, name='Animated copy'), transform_event(3, 10, translation(OFFSET, .25))], camera=False)
    scripts['animated-reload'] = script(39, [('zero', 13), ('midpoint', 26), ('one', 39)],
        [dict(frame=1, animationPlaying=False, animationTime=0.), dict(frame=14, animationTime=.5), dict(frame=27, animationTime=1.)], camera=False)
    for name, value in scripts.items(): document(output/(name+'.json'), value)
    immutable(output/'vk_layer_settings.txt', b'khronos_validation.validate_sync = true\n')
    files = [*sources.values(), *(output/(name+'.json') for name in scripts), output/'vk_layer_settings.txt']
    plan = dict(schemaVersion=1, cohort=cohort, sources={name: str(path) for name, path in sources.items()},
        scripts={name: str(output/(name+'.json')) for name in scripts},
        controlCameraNodes=controls,
        immutableInputs=[dict(path=str(path), sha256=sha(path), byteLength=path.stat().st_size) for path in files],
        editedStates=EDITED, primitiveStates=PRIMITIVE, rayStates=RAY,
        limits=UNVERIFIED, sourceTables=SOURCE_TABLES,
        fixtureGraphIds=dict(editor=7, emptyE=8, emptyF=9, outside=10, procedural=11, copiedE=18),
        literalDuplicateClosure=dict(visual=[2, 4, 5, 6, 8, 9, 11], bindingOnly=[1], transformOnly=[0]),
        independentWorldQuadCount=dict(before=4, separatedCopy=7))
    document(output/'launch-plan.json', plan); return plan


def rows(state):
    view = state['gltfMerge']
    require(not view['authoringNodesTruncated'], 'Complete clone authoring evidence was truncated')
    result = {row['graphNode']: row for row in view['authoringNodes']}
    require(len(result) == len(view['authoringNodes']) == view['graphNodeCount'] == state['gltfScenes']['authoringNodeCount'],
        'Missing/duplicate authoring graph slots')
    return result


def matrix(row, key='worldColumns'):
    value = np.asarray(row[key], dtype=float)
    require(value.shape == (4, 4) and np.isfinite(value).all(), 'Invalid clone graph matrix')
    return value.T


def source_map(nodes, count):
    pairs = [(row['sourceNode'], index) for index, row in nodes.items() if row['sourceNode'] >= 0]
    result = dict(pairs)
    require(len(result) == len(pairs) == count and set(result) == set(range(count)), 'Clone source IDs are not complete/injective')
    return result


def descendants(nodes, root):
    children = {index: [] for index in nodes}
    for index, row in nodes.items():
        parent = row['parentGraphNode']; require(parent == INVALID or parent in nodes, 'Invalid clone graph parent')
        if parent != INVALID: children[parent].append(index)
    seen, pending = set(), [root]
    while pending:
        index = pending.pop(); require(index not in seen, 'Invalid clone forest cycle/duplicate edge')
        seen.add(index); pending.extend(children[index])
    return seen


def common_state(state, *, scene, draws, slots, sources, vertices, indices):
    nodes = rows(state); mapping = source_map(nodes, sources)
    require(len(nodes) == slots and state['gltfScenes']['selected'] == scene and
        state['gltfScenes']['activeRenderableCount'] == state['scene']['primaryObjectCount'] == state['gltfInstancing']['drawCount'] == draws,
        'Wrong clone graph/scene/draw counts')
    require(state['scene']['primaryVertexCount'] == vertices and state['scene']['primaryIndexCount'] == indices,
        'Duplication decoded/copied geometry or lost a procedural runtime gap')
    require(not state['gui']['modelLoading']['pending'] and state['scene']['primaryImportScale'] == 1. and
        all(not row['retired'] for row in nodes.values()), 'Unsettled clone or unexpected retired graph slots')
    view = state['gltfMerge']
    require(view['sourceReadCount'] == 0 and view['descriptorGeneration'] == view['commitGeneration'],
        'Duplication reopened an asset or published mismatched descriptor keys')
    return nodes, mapping


def loaded_state(state):
    nodes, mapping = common_state(state, scene=0, draws=4, slots=8, sources=3, vertices=12, indices=18)
    require(mapping == {0: 0, 1: 1, 2: 4} and nodes[0]['parentGraphNode'] == 7 and nodes[1]['parentGraphNode'] == 0 and
        nodes[2]['parentGraphNode'] == nodes[3]['parentGraphNode'] == 1 and nodes[4]['parentGraphNode'] == 0,
        'Loaded fixture graph IDs differ from the explicitly selected capture targets')
    require(state['gltfMerge']['sourceTables'] == SOURCE_TABLES and state['gltfMerge']['activeRoots'] == [0], 'Loaded source table/root mismatch')
    for graph, primitive_id, mesh, local in [(2, 0, 0, 0), (3, 1, 0, 1), (5, 2, 1, 0), (6, 2, 1, 0)]:
        row = nodes[graph]
        require(row['renderable'] and (row['primitiveIndex'], row['sourceMesh'], row['sourcePrimitive']) == (primitive_id, mesh, local),
            'Source-flat/runtime primitive provenance mismatch before edits')
    authoring.history_cursor(state, 0, 0, allow_uncreated=True)
    return dict(passed=True, literalInitialGraphIds=True, sourceFlatPrimitives=3, runtimeGeometryVertices=12)


def prepared_state(state, *, history=(8, 0)):
    nodes, mapping = common_state(state, scene=0, draws=5, slots=12, sources=3, vertices=16, indices=24)
    require(mapping == {0: 0, 1: 1, 2: 4} and state['gltfMerge']['sourceTables'] == SOURCE_TABLES and
        state['gltfMerge']['activeRoots'] == [0], 'Prepared source catalog or exact roots changed')
    parents = {0: 7, 1: 0, 2: 8, 3: 10, 4: 9, 5: 4, 6: 4, 7: INVALID, 8: 0, 9: 8, 10: 0, 11: 9}
    for index, parent in parents.items(): require(nodes[index]['parentGraphNode'] == parent, 'Typed reparent did not preserve the intended edited forest')
    for graph, name in [(8, E), (9, F), (10, OUTSIDE), (11, PROCEDURAL)]:
        require(nodes[graph]['sourceNode'] == -1 and nodes[graph]['name'] == name, 'Graph-only edit acquired an invented source identity')
    require(nodes[11]['renderable'] and nodes[11]['primitiveIndex'] == 3 and nodes[11]['sourceMesh'] == -1 and
        nodes[11]['sourcePrimitive'] == -1 and nodes[11]['meshOwnerGraphNode'] == INVALID, 'Procedural runtime primitive aliases source geometry')
    require(nodes[2]['meshOwnerGraphNode'] == nodes[3]['meshOwnerGraphNode'] == 1 and not descendants(nodes, 1)-{1},
        'Reparenting changed immutable mesh ownership or failed to empty owner A')
    expected = {0: SHEAR, 1: SHEAR@translation(-1.25), 2: SHEAR@translation(-1.25), 3: SHEAR@translation(-1.25),
        4: SHEAR@translation(-1.25, -.75), 7: np.eye(4), 8: SHEAR, 9: SHEAR, 10: SHEAR, 11: SHEAR}
    for graph, x in ((5, -.75), (6, .75)):
        expected[graph] = expected[4]@translation(x)@np.diag([.5, .5, 1., 1.])
    for graph, world in expected.items(): close(matrix(nodes[graph]), world, 'literal prepared world '+str(graph), 0.)
    flags = {row['graphNode']: row for row in state['gltfNodeFlags']['nodes']}
    require(not flags[4]['localHoverable'] and all(flags[index]['localVisible'] and flags[index]['localSelectable'] for index in nodes),
        'Prepared flag edits or independent local visibility changed')
    require(state['gui']['selectedMeshNodeId'] == 11, 'Procedural setup selection changed')
    authoring.history_cursor(state, *history)
    return dict(passed=True, typedEditedForest=True, graphOnlyNodes=4, exactLiteralWorlds=True, immutableMeshOwner=1, proceduralPrimitive=3)


def edited_state(state, prepared, name, *, same_session=True):
    if name == 'prepared': return prepared_state(state)
    before = rows(prepared); has_copy = name != 'undo-clone'
    empty_owner = name in ('empty-owner', 'redo-empty', 'shared', 'empty')
    offset = name in ('offset', 'redo-transform', 'empty-owner', 'undo-empty', 'redo-empty', 'shared', 'empty')
    scene = 1 if name == 'shared' else 2 if name == 'empty' else 0
    slots, sources = (22, 7) if empty_owner else (21, 6) if has_copy else (12, 3)
    draws = 0 if scene == 2 else 9 if has_copy else 5
    nodes, mapping = common_state(state, scene=scene, draws=draws, slots=slots, sources=sources, vertices=16, indices=24)
    require(not same_session or state['gltfMerge']['graphIdentity'] == prepared['gltfMerge']['graphIdentity'], 'Copy replaced the stable graph object identity')
    source_tables = dict(SOURCE_TABLES, nodes=sources)
    require(state['gltfMerge']['sourceTables'] == source_tables, 'Clone changed shared source resources')
    expected_roots = [] if scene == 2 else [0, 12] if has_copy else [0]
    require(state['gltfMerge']['activeRoots'] == expected_roots, 'Exact returned clone scene-root order/membership changed')
    for graph, old in before.items():
        for key in ('sourceNode', 'parentGraphNode', 'meshOwnerGraphNode', 'renderable', 'name', 'primitiveIndex', 'materialIndex',
                    'sourceMesh', 'sourcePrimitive', 'sourceInstance', 'localColumns', 'worldColumns', 'retired'):
            require(nodes[graph][key] == old[key], 'Clone changed retained source/geometry/local payload: '+key)
    cursor = {'copy': (9, 0), 'offset': (10, 0), 'undo-transform': (9, 1), 'undo-clone': (8, 2), 'redo-clone': (9, 1),
        'redo-transform': (10, 0), 'empty-owner': (11, 0), 'undo-empty': (10, 1), 'redo-empty': (11, 0), 'shared': (0, 0), 'empty': (0, 0)}[name]
    authoring.history_cursor(state, *cursor)
    selected = INVALID if scene else 21 if empty_owner else 18 if has_copy else 11
    require(state['gui']['selectedMeshNodeId'] == selected, 'Clone root/prior selection history policy changed')
    if has_copy:
        literal = {0: 12, 1: 13, 2: 14, 4: 15, 5: 16, 6: 17, 8: 18, 9: 19, 11: 20}
        require(set(nodes)-set(before) == set(literal.values()) | ({21} if empty_owner else set()), 'Clone pulled an external sibling or omitted a selected/dependency slot')
        for old_id, new_id in literal.items():
            old, new = before[old_id], nodes[new_id]
            require(new['name'] == (COPY if old_id == 8 else old['name']), 'Cloned graph label differs')
            if old_id in (0, 1):
                require(not new['renderable'] and new['primitiveIndex'] == INVALID and new['meshOwnerGraphNode'] == INVALID and
                    new['sourceMesh'] == new['sourcePrimitive'] == new['sourceInstance'] == -1, 'Nonvisual dependency pulled a visual attachment')
            else:
                for key in ('renderable', 'primitiveIndex', 'materialIndex', 'sourceMesh', 'sourcePrimitive', 'sourceInstance'):
                    require(new[key] == old[key], 'Clone lost shared runtime primitive/material provenance: '+key)
            expected_parent = old['parentGraphNode'] if old_id == 8 else literal.get(old['parentGraphNode'], old['parentGraphNode'])
            require(new['parentGraphNode'] == expected_parent, 'Clone edited/dependency parent is wrong')
            if old_id not in (0, 1):
                require(new['meshOwnerGraphNode'] == literal.get(old['meshOwnerGraphNode'], old['meshOwnerGraphNode']), 'Cloned immutable owner aliases the original')
            expected_local = translation(OFFSET) if offset and old_id == 8 else matrix(old, 'localColumns')
            close(matrix(new, 'localColumns'), expected_local, 'exact cloned local', 0.)
            expected_world = translation(OFFSET)@matrix(old) if offset and old_id not in (0, 1) else matrix(old)
            close(matrix(new), expected_world, 'literal cloned world', 0.)
        require(mapping == {0: 0, 1: 1, 2: 4, 3: 12, 4: 13, 5: 15, **({6: 21} if empty_owner else {})},
            'Cloned source groups are not append-only or graph-only nodes acquired source IDs')
        if empty_owner:
            row = nodes[21]
            require(row['name'] == EMPTY_COPY and row['sourceNode'] == 6 and not row['renderable'] and row['parentGraphNode'] == 0 and
                descendants(nodes, 21) == {21}, 'Owner with moved-out primitives invented a descendant/draw')
            close(matrix(row), matrix(before[1]), 'childless owner world', 0.)
        flags = {row['graphNode']: row for row in state['gltfNodeFlags']['nodes']}
        require(not flags[4]['localHoverable'] and not flags[15]['localHoverable'], 'Duplicate lost an independent local node flag')
    return dict(passed=True, name=name, graphSlots=slots, sourceNodes=sources, draws=draws, exactRetainedPayloads=True,
        graphOnlyAndBindingContexts=True, sourceResourcesShared=True, sourceReadAttempts=state['gltfMerge']['sourceReadCount'])


def primitive_state(state, name):
    present = name in ('created', 'redo', 'replacement', 'other-scene')
    scene = 0 if name == 'replacement' else 2
    draws = 5 if scene == 0 else int(name in ('created', 'redo'))
    replacement = name in ('replacement', 'other-scene')
    nodes, _ = common_state(state, scene=scene, draws=draws, slots=9 if present else 8, sources=3,
        vertices=20 if replacement else 16, indices=30 if replacement else 24)
    roots = [0] if scene == 0 else [8] if draws else []
    require(state['gltfMerge']['activeRoots'] == roots, 'Undone primitive root activated a reused slot in another scene')
    cursor = (0, 0) if name == 'other-scene' else (0, 1) if name in ('undo', 'undo-again') else (1, 0)
    authoring.history_cursor(state, *cursor)
    if present:
        require(nodes[8]['name'] == ('Replacement other scene' if replacement else 'Temporary empty-scene quad') and
            nodes[8]['sourceNode'] == -1 and nodes[8]['primitiveIndex'] == (4 if replacement else 3) and
            nodes[8]['parentGraphNode'] == (0 if replacement else 7), 'Primitive undo/redo or reused-slot ownership changed')
    selected = INVALID if name in ('undo', 'undo-again', 'other-scene') else 8
    require(state['gui']['selectedMeshNodeId'] == selected, 'Primitive addition selection/history changed')
    return dict(passed=True, name=name, activeRoots=roots, graphSlots=len(nodes), runtimeGeometryIdsAppendOnly=True,
        reusedSlotDoesNotActivateOtherScene=name == 'other-scene')


def animation_spec(name):
    copied = name not in ('before', 'loaded', 'undo-clone')
    time = None if name == 'rest' else 0. if name == 'zero' else 1. if name in ('seek', 'undo-transform', 'redo-clone', 'redo-transform') else .5
    offset = name in ('offset', 'seek', 'redo-transform', 'zero', 'rest', 'reselect')
    return time, copied, offset


def check_deformation(state, time, *, copied, offset, binding=False, owners=(0, 5)):
    deformation = state['gltfDeformation']; count = 1+int(copied)
    require(deformation['active'] and deformation['gpuReady'] and deformation['currentBoundToGpu'] and deformation['previousBoundToGpu'] and
        deformation['instanceCount'] == count and deformation['sourceVertexCount'] == 8 and
        deformation['expandedVertexCount'] == 8+4*count and deformation['expandedIndexCount'] == 12+6*count,
        'Incorrect complete-frame source prefix/independent skin instance allocation')
    samples = deformation['instances']; require(len(samples) == count, 'Missing/extra bound deformation samples')
    expected = dynamic_geometry(time, copied=copied, offset=offset, binding=binding); values = dynamic_values(time)
    by_owner = {row['sourceNodeIndex']: row for row in samples}
    require(set(by_owner) == set(owners[:count]) and len(by_owner) == len(samples), 'Cloned skin aliases an original source owner')
    vertex_ranges, index_ranges = [], []
    for source, (local, model, _) in zip(owners, [row[:3] for row in expected]):
        row = by_owner[source]
        require(row['vertexCount'] == 4 and row['indexCount'] == 6 and row['boundsValid'], 'Authored complete tangent frame unexpectedly splits/loses a skin vertex')
        close(row['weights'], [values['weight']], 'cloned morph weights')
        close(row['firstPosition'], local[0], 'literal independent morph-before-skin vertex')
        close(row['boundsMin'], local.min(axis=0), 'literal skin min'); close(row['boundsMax'], local.max(axis=0), 'literal skin max')
        close(row['firstNormal'], [0., 0., 1.], 'skin normal'); close(np.asarray(row['modelColumns']).T, model, 'post-skin draw world')
        vertex_ranges.append((row['vertexOffset'], row['vertexOffset']+4)); index_ranges.append((row['firstIndex'], row['firstIndex']+6))
    require(sorted(vertex_ranges) == [(8+4*i, 12+4*i) for i in range(count)] and
        sorted(index_ranges) == [(12+6*i, 18+6*i) for i in range(count)], 'Cloned deformation ranges alias or leave unaccounted suffix geometry')


def check_effective_animation(state, time, *, camera=3, source_owners=(0, 5), copied=False, require_rest=True):
    animation = state['gltfAnimation']; values = dynamic_values(time)
    require(animation['clipCount'] == 1 and animation['diagnostics'] == 0 and animation['selectedClip'] == (-1 if time is None else 0) and
        abs(animation['time']-(0. if time is None else time)) < 1e-6 and not animation['playing'] and animation['speed'] == 0. and animation['loop'],
        'Clone changed selected clip/time/speed/playing/loop or appended/replaced a clip')
    require(animation['selectedCameraNode'] == camera and not animation['orthographic'], 'Clone changed authored camera selection')
    close(state['camera']['position'], [0., 0., 6.], 'effective selected camera position')
    projection = np.zeros((4, 4)); focal = 1./math.tan(values['fov']/2.)
    projection[0, 0], projection[1, 1] = focal/(WIDTH/HEIGHT), focal
    projection[2, 2], projection[2, 3], projection[3, 2] = .1/99.9, 10./99.9, -1.
    close(np.asarray(animation['cameraProjection']).reshape(4, 4).T, projection, 'independent effective reverse-Z projection')
    close([animation['cameraNear'], animation['cameraFar']], [.1, 100.], 'camera clips')
    lights = animation['pointLights']; require(len(lights) == 2, 'Joint-only dependency copied a visible light attachment or lost a retained binding')
    for position in ([0., .25, 0.], [0., 1.5, 3.]):
        matches = [row for row in lights if np.linalg.norm(np.asarray(row['position'])-position) < 1e-4]
        require(len(matches) == 1, 'External context/retained light world position changed')
        close(matches[0]['color'], [1., 1., 1.], 'authored point color')
        close([matches[0]['intensity'], matches[0]['range']], [values['intensity'], 10.], 'shared resource light animation')
    if time is None:
        # This telemetry intentionally enumerates selected-clip targets; after
        # Clear it is empty. The literal rest-color image control covers color.
        require(animation['materials'] == [] and animation['targets'] == [], 'Cleared clip retained sampled target rows')
    else:
        require(len(animation['materials']) == 1, 'Unexpected animated source material count')
        close(animation['materials'][0]['baseColor'], values['color'], 'shared material resource animation')
        targets = animation['targets']
        if camera == 3:
            expected = {(0, 2, 'translation', -1), (3, 0, 'weights', -1), (2, 0, 'scale', 0),
                (4, 0, 'pbrMetallicRoughness/baseColorFactor', -1), (5, 0, 'intensity', -1), (6, 0, 'perspective/yfov', -1)}
            if copied: expected |= {(0, 7, 'translation', -1), (3, 5, 'weights', -1), (2, 5, 'scale', 0)}
            actual = [(row['kind'], row['sourceIndex'], row['property'], row['component']) for row in targets]
            require(len(actual) == len(expected) and set(actual) == expected, 'Cloned same-clip node suffix changed resource targets or omitted binding-only weight/component tracks')
    require(state['lighting']['directionalIntensity'] == 0. and state['lighting']['environmentIntensity'] == 0., 'CLI lighting edits changed during clone/history')


def animated_state(state, before, name, *, binding=False, same_session=True):
    time, copied, offset = animation_spec(name)
    slots = (14 if copied else 9) if binding and name != 'loaded' else (12 if copied else 8)
    nodes, mapping = common_state(state, scene=0, draws=3 if copied else 2, slots=slots,
        sources=8 if copied else 5, vertices=8, indices=12)
    require(not same_session or state['gltfMerge']['graphIdentity'] == before['gltfMerge']['graphIdentity'], 'Clone replaced graph identity')
    require(mapping == ({0: 0, 1: 2, 2: 4, 3: 5, 4: 6, 5: 9, 6: 11, 7: 12} if binding else
        {0: 0, 1: 2, 2: 4, 3: 5, 4: 6, 5: 8, 6: 10, 7: 11}) if copied else mapping == {0: 0, 1: 2, 2: 4, 3: 5, 4: 6},
        'Clone dependency/source IDs or explicitly selected fixture IDs changed')
    source_tables = dict(before['gltfMerge']['sourceTables']); source_tables['nodes'] = 8 if copied else 5
    require(state['gltfMerge']['sourceTables'] == source_tables, 'Duplication copied meshes/materials/accessors/textures or changed retained resource IDs')
    roots = [0, 2, 5, 6]
    if binding and name != 'loaded': roots.append(8)
    if copied: roots += [13, 9, 11] if binding else [8, 10]
    require(state['gltfMerge']['activeRoots'] == roots, 'Clone lost exact original/visual/dependency scene-root membership/order')
    for graph in (0, 1, 2, 3, 4, 5, 6, 7):
        old = rows(before)[graph]; row = nodes[graph]
        for key in ('name', 'sourceNode', 'meshOwnerGraphNode', 'primitiveIndex', 'materialIndex', 'sourceMesh', 'sourcePrimitive', 'sourceInstance'):
            require(row[key] == old[key], 'Animated clone changed retained identity/provenance '+key)
    values = dynamic_values(time)
    owner_local = translation(-1.25)@np.diag([values['scale'], 1., 1., 1.])
    close(matrix(nodes[0], 'localColumns'), owner_local, 'retained animation owner component pointer')
    close(matrix(nodes[4], 'localColumns'), translation(*values['joint']), 'retained joint translation')
    if copied:
        root, child, context, joint = (13, 10, 11, 12) if binding else (8, 9, 10, 11)
        require(nodes[root]['name'] == ('Binding copy' if binding else 'Animated copy') and
            nodes[child]['meshOwnerGraphNode'] == (9 if binding else 8) and nodes[child]['primitiveIndex'] == 0 and
            nodes[joint]['parentGraphNode'] == context and nodes[context]['name'] == JOINT_PARENT and
            not nodes[context]['renderable'] and descendants(nodes, context) == {context, joint},
            'Visual/binding-only/joint-only closure pulled external bystander draws or lost copied skin joints')
        require(nodes[child]['parentGraphNode'] == root, 'Copied primitive did not follow actual graph-only edited parent')
        if binding:
            require(nodes[9]['name'] == ANIMATION_NAME and not nodes[9]['renderable'] and descendants(nodes, 9) == {9} and
                nodes[9]['sourceNode'] == 5 and nodes[root]['sourceNode'] == -1, 'Binding-only immutable owner was treated as a selected visual subtree')
        close(matrix(nodes[9 if binding else 8], 'localColumns'), owner_local, 'cloned owner component pointer')
        close(matrix(nodes[joint], 'localColumns'), translation(*values['joint']), 'cloned same-clip joint track')
        close(matrix(nodes[context], 'localColumns'), translation(OFFSET if offset and not binding else 0., .25), 'cloned dependency context local')
    check_effective_animation(state, time, copied=copied)
    check_deformation(state, time, copied=copied, offset=offset, binding=binding)
    if name != 'loaded':
        cursor = {'before': (2, 0) if binding else (0, 0), 'copy': (3, 0) if binding else (1, 0),
            'offset': (4, 0) if binding else (2, 0), 'seek': (4, 0) if binding else (2, 0),
            'undo-transform': (3, 1) if binding else (1, 1), 'undo-clone': (2, 2) if binding else (0, 2),
            'redo-clone': (3, 1) if binding else (1, 1), 'redo-transform': (4, 0) if binding else (2, 0),
            'zero': (4, 0) if binding else (2, 0), 'rest': (4, 0) if binding else (2, 0), 'reselect': (4, 0) if binding else (2, 0)}[name]
        authoring.history_cursor(state, *cursor, allow_uncreated=not binding and name == 'before')
    expected_selected = before['gui']['selectedMeshNodeId']
    if name not in ('before', 'loaded', 'undo-clone'): expected_selected = 13 if binding else 8
    require(state['gui']['selectedMeshNodeId'] == expected_selected, 'Clone/history failed to restore prior/new root selection')
    return dict(passed=True, name=name, copied=copied, time=time, playing=False, sharedSourceResources=True,
        independentMorphSkinAndPostSkin=binding, boundDeformedOwners=1+int(copied), dependencyAttachmentsOmitted=True,
        preservedSameClipAndAuthoredRest=True)


def image_compare(first, second, *, regions=None, contrast=False, minimum=.5):
    a, b = authoring.rgba(first), authoring.rgba(second); measures = []
    for left, top, right, bottom in regions or [[0, 0, WIDTH, HEIGHT]]:
        delta = np.abs(a[top:bottom, left:right, :3]-b[top:bottom, left:right, :3])
        measures.append(dict(maximum=int(delta.max()), mean=float(delta.mean()), changedFraction=float(np.any(delta > 2, axis=2).mean())))
    if contrast: require(any(row['mean'] >= minimum and row['changedFraction'] >= .002 for row in measures), 'Independent clone image control has no visible contrast')
    else: require(all(row['maximum'] == 0 for row in measures), 'Clone independent/restored image differs')
    return dict(passed=True, regions=regions, contrast=contrast, measurements=measures)


STATIC_ROIS = [[232, 122, 241, 131], [219, 230, 228, 239], [165, 231, 171, 237], [274, 231, 280, 237],
    [413, 122, 422, 131], [346, 231, 352, 237], [455, 231, 461, 237], [40, 40, 55, 55]]


def visible_regions(record, centers):
    image = authoring.rgba(record); background = image[40:55, 40:55, :3].mean(axis=(0, 1)); deltas = []
    for x, y in centers:
        color = image[y-2:y+3, x-2:x+3, :3].mean(axis=(0, 1)); deltas.append(float(np.abs(color-background).mean()))
    require(all(delta >= 10. for delta in deltas), 'Independent fixture geometry is not visibly distinguishable from background')
    return dict(passed=True, minimumBackgroundDelta=10., measuredDeltas=deltas)


def dynamic_regions(time, *, binding=False, copied=False, offset=False):
    values = dynamic_values(time); focal = HEIGHT/(12.*math.tan(values['fov']/2.)); regions = []
    for _, _, points, _ in dynamic_geometry(time, binding=binding, copied=copied, offset=offset):
        center = points.mean(axis=0); x, y = WIDTH/2+center[0]*focal, HEIGHT/2-center[1]*focal
        regions.append([int(x)-4, int(y)-4, int(x)+5, int(y)+5])
    # Include bystander interior and a background witness; camera projection is already numeric.
    x, y = WIDTH/2+2.*focal, HEIGHT/2+.75*focal
    regions += [[int(x)-4, int(y)-4, int(x)+5, int(y)+5], [40, 40, 55, 55]]
    return regions


def mesh_rows(path):
    model, _ = load_asset(path); worlds, _ = forest_worlds(model); result = []
    for node_index, node in enumerate(model['nodes']):
        if 'mesh' not in node: continue
        mesh = model['meshes'][node['mesh']]
        extension = node.get('extensions', {}).get(INSTANCING)
        instance = np.eye(4); identity = None
        if extension:
            attrs = extension['attributes']; require('_ID' in attrs, 'Export dropped exact custom instancing identity')
            identity_accessor = model['accessors'][attrs['_ID']]
            require(identity_accessor['componentType'] == 5125 and identity_accessor['count'] == 1 and identity_accessor['type'] == 'SCALAR',
                'Exported singleton instance ID changed width/count/type')
            identity = exact_uint32(path, attrs['_ID'])[0]
            t = decode_accessor(path, attrs['TRANSLATION']).reshape(-1) if 'TRANSLATION' in attrs else [0., 0., 0.]
            s = decode_accessor(path, attrs['SCALE']).reshape(-1) if 'SCALE' in attrs else [1., 1., 1.]
            if 'ROTATION' in attrs: close(decode_accessor(path, attrs['ROTATION']).reshape(-1), [0., 0., 0., 1.], 'identity instance rotation', 0.)
            instance = translation(*t)@np.diag([*s, 1.])
        for primitive_id, primitive_value in enumerate(mesh['primitives']):
            points = decode_accessor(path, primitive_value['attributes']['POSITION'])
            result.append(dict(node=node_index, primitive=primitive_value, mesh=node['mesh'], instanceId=identity,
                world=worlds[node_index]@instance, points=points, worldPoints=transformed(points, worlds[node_index]@instance)))
    return model, worlds, result


def match_worlds(actual, expected, label):
    pending = list(actual)
    for points in expected:
        matches = [i for i, candidate in enumerate(pending) if candidate.shape == points.shape and np.max(np.abs(candidate-points)) < 4e-5]
        require(matches, 'Missing/incorrect independent exported world geometry: '+label)
        pending.pop(matches[0])
    require(not pending, 'Extra exported geometry: '+label)


def inspect_edited_export(path):
    model, worlds, meshes = mesh_rows(path)
    require((path.read_bytes()[:4] == b'glTF') == (path.suffix == '.glb') and len(model['nodes']) == 22 and len(meshes) == 9 and
        model['scene'] == 2 and len(model['scenes']) == 3, 'Wrong edited graph export layout/container/default')
    require([scene['name'] for scene in model['scenes']] == ['Shared A', 'Shared B', 'Empty'] and
        [scene.get('nodes', []) for scene in model['scenes']] == [[0, 12], [0, 12], []] and
        [scene.get('extras', {}).get('cloneTag') for scene in model['scenes']] == ['scene-a', 'scene-b', 'scene-empty'],
        'Global shared-scene clone membership/order/metadata was lost at export')
    groups = {tag: [i for i, node in enumerate(model['nodes']) if node.get('extras', {}).get('cloneTag') == tag]
        for tag in ('root', 'owner-a', 'owner-b')}
    require(groups == {'root': [0, 12], 'owner-a': [1, 13, 21], 'owner-b': [4, 15]}, 'Source metadata attached to wrong duplicate or synthetic primitive child')
    require(all(model['nodes'][index].get('extensions', {}).get('KHR_node_hoverability', {}).get('hoverable') is False for index in (4, 15)),
        'Portable duplicate lost independent local hoverability edits')
    for group in (*groups['owner-a'], *groups['owner-b']): require('mesh' not in model['nodes'][group], 'Export failed to separate source groups from primitive bindings')
    require(not model['nodes'][1].get('children') and not model['nodes'][13].get('children') and not model['nodes'][21].get('children') and
        model['nodes'][21]['name'] == EMPTY_COPY and model['nodes'][18]['name'] == COPY, 'Empty owner duplication restored moved-out draws or lost optional/default names')
    points = {tag: [] for tag in ('a0', 'a1', 'b0', 'procedural')}; ids = []
    for row in meshes:
        tag = row['primitive'].get('extras', {}).get('cloneTag')
        if tag not in ('a0', 'a1', 'b0'): tag = 'procedural'
        points[tag].append(row['worldPoints'])
        if tag == 'b0':
            ids.append(row['instanceId']); node = model['nodes'][row['node']]
            require(node['extensions'][INSTANCING].get('extras', {}).get('opaqueInstanceIndex') == 977, 'Opaque instance extension metadata changed')
        if tag != 'procedural': require(row['primitive']['extras']['opaqueIndex'] == 991, 'Primitive source metadata was not shared/preserved')
    require(sorted(ids) == [16777217, 16777217, 16777219, 16777219], 'UINT32 singleton clone identities were rounded/lost/duplicated')
    for tag in ('a0', 'a1', 'b0'):
        match_worlds(points[tag], [points for points, _, candidate in edited_world_quads(True) if candidate == tag], tag)
    procedural = np.asarray([[-.5, -.5, 0.], [.5, -.5, 0.], [.5, .5, 0.], [-.5, .5, 0.]])
    # Compare sorted vertices because procedural source winding/order is independently immaterial here.
    sort = lambda p: np.asarray(sorted(map(tuple, p)))
    match_worlds([sort(p) for p in points['procedural']], [sort(transformed(procedural, transform)) for transform in
        (SHEAR, translation(OFFSET)@SHEAR)], 'procedural quad')
    require(len({row['mesh'] for row in meshes}) == 4, 'Duplication serialized extra source geometry instead of sharing runtime primitive definitions')
    for i in range(len(model.get('accessors', []))): require(np.isfinite(decode_accessor(path, i)).all(), 'Nonfinite exported accessor')
    return dict(passed=True, path=str(path), sha256=sha(path), graphNodes=22, renderableBindings=9, sharedMeshDefinitions=4,
        exactUINT32Instances=sorted(ids), literalWorldGeometry=True, childlessOwners=3, sharedSceneRoots=True)


def reload_edited_state(state, exported, scene, baseline):
    model, worlds, exported_meshes = mesh_rows(exported)
    nodes, mapping = common_state(state, scene=scene, draws=0 if scene == 2 else 9, slots=32, sources=22, vertices=16, indices=24)
    require(state['gltfScenes']['names'] == ['Shared A', 'Shared B', 'Empty'], 'Portable reload lost scene catalog')
    for source, graph in mapping.items():
        require(nodes[graph]['name'] == model['nodes'][source].get('name', ''), 'Portable reload lost exact graph label')
        close(matrix(nodes[graph]), worlds[source], 'portable edited graph world')
    shape = authoring.history_cursor(state, 0, 0, allow_uncreated=True)
    if baseline is not None: require(shape == authoring.history_cursor(baseline, 0, 0, allow_uncreated=True), 'Reload history counter shape changed without an edit')
    require(state['gltfMerge']['sourceTables']['nodes'] == 22, 'Portable source graph node identity changed')
    return dict(passed=True, selectedScene=scene, sourceNodes=22, graphNodes=32, sourceGeometryShared=True, literalExportWorlds=True)


def node_channel_key(channel):
    target = channel['target']
    if target.get('path') != 'pointer': return ('node', target['node'], target['path'])
    pointer = target['extensions'][POINTER]['pointer']; parts = pointer.split('/')
    if len(parts) >= 4 and parts[1] == 'nodes': return ('node', int(parts[2]), '/'.join(parts[3:]))
    return ('resource', pointer)


def inspect_animated_export(path, source):
    original, _ = load_asset(source); model, worlds, meshes = mesh_rows(path)
    require(len(model['nodes']) == 14 and len(meshes) == 3 and len(model['skins']) == 2 and len(model['animations']) == 1,
        'Portable animated clone exported external ancestor visuals or lost skins/clip identity')
    owners = [i for i, n in enumerate(model['nodes']) if n.get('extras', {}).get('cloneTag') == 'skin-owner']
    ancestors = [i for i, n in enumerate(model['nodes']) if n.get('extras', {}).get('cloneTag') == 'joint-context']
    joints = [i for i, n in enumerate(model['nodes']) if n.get('extras', {}).get('cloneTag') == 'joint']
    require(owners == [0, 8] and ancestors == [2, 10] and joints == [4, 11], 'Animated source/graph projection identity changed')
    require('camera' not in model['nodes'][10] and PUNCTUAL not in model['nodes'][10].get('extensions', {}) and
        len([row for row in meshes if row['primitive'].get('extras', {}).get('cloneTag') == 'bystander']) == 1,
        'Joint-only dependency copied camera/light/bystander attachments')
    skin_children = []
    for owner, joint, offset in zip(owners, joints, (0., OFFSET)):
        node = model['nodes'][owner]; require('mesh' not in node and 'skin' not in node and 'weights' not in node, 'Export source group retained primitive-only skin/morph attachment')
        children = [row for row in meshes if row['node'] in node.get('children', [])]
        require(len(children) == 1, 'Missing actual skinned primitive child')
        child = model['nodes'][children[0]['node']]; skin = model['skins'][child['skin']]
        require(skin['joints'] == [joint] and skin['skeleton'] == (2 if owner == 0 else 10), 'Cloned skin references original/external unremapped joints')
        close(child['weights'], [dynamic_values(.5)['weight']], 'saved-pose morph defaults')
        close(decode_accessor(path, skin['inverseBindMatrices']), [translation(-1.).T.reshape(-1)], 'shared source IBM bytes', 0.)
        close(children[0]['points'], QUAD, 'shared bind primitive positions', 0.)
        close(worlds[2 if owner == 0 else 10][:3, 3], [offset, .25, 0.], 'edited portable joint context world', 0.)
        skin_children.append(children[0]['node'])
    original_clip = original['animations'][0]; clip = model['animations'][0]
    expected_node = {(4, 'translation'), (0, 'weights'), (0, 'scale/0'), (11, 'translation'), (8, 'weights'), (8, 'scale/0')}
    seen = set()
    # Export rewrites weight tracks onto actual primitive children; typed scale/translation remain source groups.
    for channel in clip['channels']:
        key = node_channel_key(channel); sampler = clip['samplers'][channel['sampler']]
        if key[0] != 'node': continue
        node, path_name = key[1:]
        if path_name == 'weights': node = owners[skin_children.index(node)] if node in skin_children else node
        require((node, path_name) in expected_node and (node, path_name) not in seen, 'Unexpected/duplicate clone node animation suffix')
        seen.add((node, path_name)); index = 0 if path_name == 'translation' else 1 if path_name == 'weights' else 2
        old_sampler = original_clip['samplers'][original_clip['channels'][index]['sampler']]
        require(model['accessors'][sampler['input']]['componentType'] == 5126 and model['accessors'][sampler['output']]['componentType'] == 5126 and
            sampler.get('interpolation', 'LINEAR') == 'LINEAR', 'Cloned track metadata changed')
        require(decode_accessor(path, sampler['input']).astype(np.float32).tobytes() == decode_accessor(source, old_sampler['input']).astype(np.float32).tobytes() and
            decode_accessor(path, sampler['output']).astype(np.float32).tobytes() == decode_accessor(source, old_sampler['output']).astype(np.float32).tobytes(),
            'Original/remapped clip accessor FLOAT bytes changed')
    require(seen == expected_node, 'Missing cloned weight/component/joint track')
    camera = next(i for i, n in enumerate(model['nodes']) if n.get('name') == 'Selected camera')
    close(model['cameras'][model['nodes'][camera]['camera']]['perspective']['yfov'], dynamic_values(.5)['fov'], 'saved effective camera defaults')
    aliases = []
    for parent in (2, 6):
        bound = [child for child in model['nodes'][parent].get('children', []) if PUNCTUAL in model['nodes'][child].get('extensions', {})]
        require(len(bound) == 1, 'Export did not preserve the two authored point bindings through overlay children')
        aliases.append(bound[0])
    require(set(aliases) == {12, 13} and all(not model['nodes'][index].get('children') for index in aliases), 'Extra nonvisual/export overlay nodes')
    for alias in aliases:
        light = model['nodes'][alias]['extensions'][PUNCTUAL]['light']; definition = model['extensions'][PUNCTUAL]['lights'][light]
        close([definition['intensity'], definition['range']], [dynamic_values(.5)['intensity'], 10.], 'saved animated light alias')
        matches = [channel for channel in clip['channels'] if node_channel_key(channel) == ('resource', '/extensions/KHR_lights_punctual/lights/'+str(light)+'/intensity')]
        require(len(matches) == 1, 'Export light alias lost/shared an ambiguous animation track')
        sampler = clip['samplers'][matches[0]['sampler']]; old_sampler = original_clip['samplers'][4]
        require(decode_accessor(path, sampler['output']).astype(np.float32).tobytes() == decode_accessor(source, old_sampler['output']).astype(np.float32).tobytes(),
            'Export point-light alias curve differs from independent original FLOAT bytes')
    return dict(passed=True, path=str(path), sha256=sha(path), graphNodes=14, skins=2, clipCount=1, rawTrackFloatBytesPreserved=True,
        nodeTrackTargets=sorted(seen), skinChildren=skin_children, cameraSourceNode=camera,
        authoredRestClaim='Live clear/reselect only; portable file defaults intentionally contain the saved midpoint pose')


def reload_animated_state(state, inspection, time, baseline):
    common_state(state, scene=0, draws=3, slots=18, sources=14, vertices=8, indices=12)
    check_effective_animation(state, time, camera=inspection['cameraSourceNode'], copied=True)
    check_deformation(state, time, copied=True, offset=True, owners=tuple(inspection['skinChildren']))
    shape = authoring.history_cursor(state, 0, 0, allow_uncreated=True)
    if baseline is not None: require(shape == authoring.history_cursor(baseline, 0, 0, allow_uncreated=True), 'Portable animation reload changed lazy history shape')
    return dict(passed=True, time=time, sameClip=True, decodedSourceJointMorphAndCamera=True, deformedOwners=2,
        savedFileDefaults='midpoint; explicit t=0/.5/1 seeks verify original curves')


def ray_state(state, prepared, name, previous):
    # Same edited graph, without the later transform/childless-owner steps.
    result = prepared_state(state, history=(8, int(name == 'undo'))) if name in ('prepared', 'undo') else edited_state(state, prepared, 'copy')
    expected_cursor = {'prepared': (8, 0), 'copy': (9, 0), 'undo': (8, 1), 'redo': (9, 0)}[name]
    # The common edited checker expects two replayable commands only in the core
    # schedule; validate the ray schedule's actual cursor independently below.
    authoring.history_cursor(state, *expected_cursor)
    count = 5 if name in ('prepared', 'undo') else 9
    ray, gi = state['rayShadows'], state['ddgi']; build = ray.get('build', {})
    require(ray['supported'] and ray['active'] and ray['mode'] == 1 and not ray['denoise'] and build.get('instances') == count and
        build.get('blasBuilt', 0)+build.get('blasReused', 0) > 0 and build.get('asAllocatedBytes', 0) > 0,
        'No actual hard-ray TLAS/BLAS for the duplicated draw table')
    require(gi['supported'] and gi['enabled'] and gi['active'] and gi['contentEpoch'] > 0 and gi['updatedProbes'] > 0 and gi['tracedRays'] > 0 and
        gi['settings']['intensity'] == 0 and gi['settings']['counts'] == [3, 3, 3] and gi['settings']['raysPerProbe'] == 4,
        'No actual surface-backed DDGI update/trace')
    require(previous is None or ray['generation'] > previous[0] and gi['contentEpoch'] > previous[1], 'Ray/DDGI content did not advance on clone/replay')
    passes = {row['name']: row for row in state['passes']}; times = {}
    for name in ('RayShadowTrace', 'DdgiTrace', 'DdgiUpdate'):
        row = passes.get(name, {}); require(row.get('active') and row.get('gpuTimed') and row.get('gpuMs', 0.) > 0., 'No actual dispatch timestamp: '+name)
        times[name] = row['gpuMs']
    return dict(passed=True, TLASInstances=count, rayGeneration=ray['generation'], ddgiContentEpoch=gi['contentEpoch'],
        activeGpuPassMilliseconds=times, witness='execution only; GI intensity zero', graphClosureVerified=result['passed'])


def dependencies():
    directory = Path(__file__).resolve().parent; result = {Path(__file__).resolve()}
    for module in tuple(sys.modules.values()):
        path = getattr(module, '__file__', None)
        if path and Path(path).resolve().parent == directory: result.add(Path(path).resolve())
    return {str(path): sha(path) for path in sorted(result)}


def capture_artifacts(target):
    """Only this process's concrete outputs, excluding '-control-*' siblings."""
    pattern = re.compile(re.escape(target.name)+r'(?:\.frame-[0-9]{4,})?\.(?:png|log|telemetry\.json)\Z')
    return [path for path in target.parent.iterdir() if pattern.fullmatch(path.name)]


def require_fresh_capture(target):
    require(not capture_artifacts(target), 'Refusing to overwrite native clone evidence')


def launch(args, output, report, persist, verify, technique, name, source, labels, *, sequence_path=None,
           save=None, animated=False, camera=None, ray=False, export_rejected=False, sun=0.):
    verify(); target = output/'captures'/(technique+'-'+name); target.parent.mkdir(parents=True, exist_ok=True)
    require_fresh_capture(target)
    image, log = target.with_suffix('.png'), target.with_suffix('.log')
    command = [str(args.renderer.resolve()), '--hidden', '--no-ui', '--validation', '--no-bloom', '--no-taa', '--width', str(WIDTH),
        '--height', str(HEIGHT), '--msaa', '1', '--warmup-frames', '12', '--capture-frame', '13', '--fixed-dt', '.016666667',
        '--render-technique', technique, '--display-mode', 'lit', '--exposure', '.35', '--camera-position', '0', '0', '6',
        '--camera-target', '0', '0', '0', '--camera-fov', '45', '--environment-intensity', '0', '--directional-intensity', '3' if ray else str(sun),
        '--directional-color', '.2', '.7', '.4', '--directional-direction', '0', '0', '-1',
        '--import-scale', '1', '--gltf-scene', '0', '--model', str(source), '--screenshot', str(image)]
    command += ['--ray-shadows', 'hard', '--no-ray-shadow-denoise', '--ddgi', '--ddgi-counts', '3', '3', '3', '--ddgi-rays', '4',
        '--ddgi-probes-per-frame', '27', '--ddgi-ray-budget', '108', '--ddgi-intensity', '0'] if ray else [
        '--no-ray-query', '--ray-shadows', 'raster']
    if animated: command += ['--animation-clip', '0', '--animation-time', '.5', '--animation-speed', '0']
    if camera is not None: command += ['--gltf-camera-node', str(camera)]
    if sequence_path: command += ['--capture-sequence', str(sequence_path)]
    if save:
        save.parent.mkdir(parents=True, exist_ok=True); require(not save.exists(), 'Refusing to overwrite exported clone evidence')
        command += ['--save-scene', str(save)]
    row = dict(name=name, technique=technique, command=command, log=str(log), completed=False, expectedExportRejection=export_rejected)
    report['processes'].append(row); report['rendererExecuted'] = True; persist()
    with log.open('x', encoding='utf-8') as stream:
        process = subprocess.run(command, cwd=args.renderer.resolve().parent, stdout=stream, stderr=subprocess.STDOUT, timeout=args.timeout,
            env=dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output)), creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    row.update(returnCode=process.returncode, completed=True, logSha256=sha(log)); persist()
    text = log.read_text(encoding='utf-8', errors='replace'); require(not validation_log_error(text), 'Vulkan validation failed: '+str(log))
    if export_rejected:
        require(process.returncode != 0 and 'post-skin primitive transforms or reparenting require baking before export' in text,
            'Moved skinned primitive did not retain its expected explicit export guard: '+str(log))
        require(not save.exists(), 'Unsupported post-skin export published a scene file')
    else: require(process.returncode == 0, 'Native clone process failed: '+str(log))
    records = {}
    # The expected save rejection happens after the rendering loop has completed,
    # including its final screenshot rename, unlike a rejected capture event.
    for label, frame in labels:
        sampled = image if frame == labels[-1][1] else image.with_name(image.stem+f'.frame-{frame:04d}.png')
        telemetry = sampled.with_suffix('.telemetry.json'); require(sampled.is_file() and telemetry.is_file(), 'Missing clone sample: '+str(sampled))
        state = json.loads(telemetry.read_text(encoding='utf-8-sig'))
        require(finite_json(state) and state['technique'] == technique and state['resolution'] == [WIDTH, HEIGHT] and
            state['msaaSamples'] == 1 and not state['taa']['enabled'] and state['lighting']['environmentIntensity'] == 0 and
            any(pass_row['active'] and pass_row['gpuTimed'] for pass_row in state['passes']), 'Incorrect capture settings/no timed GPU frame')
        close([state['lighting']['directionalIntensity']], [3. if ray else sun], 'retained CLI directional override')
        close(state['lighting']['directionalColor'], [.2, .7, .4], 'retained CLI directional color override')
        item = dict(name=label, technique=technique, process=name, frame=frame, image=str(sampled), telemetry=str(telemetry), log=str(log),
            imageSha256=sha(sampled), telemetrySha256=sha(telemetry), logSha256=sha(log), validationClean=True)
        report['captures'].append(item); records[label] = {**item, 'state': state}
    require({path.resolve() for path in capture_artifacts(target) if path.suffix == '.png'} ==
        {Path(item['image']).resolve() for item in records.values()}, 'Wrong native clone sample count')
    persist(); verify(); return records


COUNTS = {'edited': (22, 6), 'primitive': (6, 1), 'animated': (25, 11), 'binding': (19, 8), 'ray-ddgi': (4, 1)}
CHECK_COUNTS = {'edited': (20, 19, 2), 'primitive': (6, 3, 0), 'animated': (18, 21, 2), 'binding': (12, 15, 1), 'ray-ddgi': (4, 0, 0)}


def run_native(args, output, plan, report):
    require(args.renderer.is_file() and args.manifest.is_file(), 'Missing explicit renderer/frozen manifest')
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/**/*')) and not any(output.glob('portable/**/*')), 'Native clone evidence directories must be fresh')
    if (output/'results.json').exists():
        previous = json.loads((output/'results.json').read_text(encoding='utf-8')); require(previous.get('generatedOnly') and not previous.get('rendererExecuted'), 'Refusing to overwrite native results')
    manifest_hash = sha(args.manifest)
    def persist():
        for key in ('captures', 'stateChecks', 'imageChecks', 'exports', 'processes'): report[key+'Count'] = len(report[key])
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    def verify():
        require(sha(args.manifest) == manifest_hash, 'Clone frozen manifest changed'); result = verify_manifest(args.manifest)
        require(dependencies() == report['sourceDependencies'] and sha(output/'launch-plan.json') == report['launchPlanSha256'], 'Harness/dependency/launch plan changed')
        for item in plan['immutableInputs']:
            path = Path(item['path']); require(path.stat().st_size == item['byteLength'] and sha(path) == item['sha256'], 'Immutable clone fixture/script changed')
        for copied in report['portableCopies']: require(all(sha(Path(item['path'])) == item['sha256'] for item in copied['files']), 'Portable resource changed')
        if 'runtimeShaderInputs' in report: require(sorted(str(path.resolve()) for path in (args.renderer.resolve().parent/'spv_shaders').glob('*.spv')) == report['runtimeShaderInputs'], 'Resident SPIR-V inventory changed')
        return result
    def check(name, technique, result, *, pixels=False):
        report['imageChecks' if pixels else 'stateChecks'].append(dict(check=name, technique=technique, **result)); persist()
    try:
        report['nativeManifestBefore'] = verify_manifest(args.manifest)
        frozen = {Path(path).resolve() for path in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
        require(args.renderer.resolve() in frozen and {Path(path).resolve() for path in report['sourceDependencies']} <= frozen, 'Manifest omits executable or local Python dependency')
        shaders = list((args.renderer.resolve().parent/'spv_shaders').glob('*.spv'))
        require(shaders and {path.resolve() for path in shaders} <= frozen, 'Manifest omits runtime SPIR-V')
        report['runtimeShaderInputs'] = sorted(str(path.resolve()) for path in shaders); report['rendererSha256'] = sha(args.renderer); persist(); verify()
        sources, scripts = ({name: Path(path) for name, path in plan[key].items()} for key in ('sources', 'scripts'))
        for technique in args.technique:
            if args.cohort == 'primitive':
                records = launch(args, output, report, persist, verify, technique, 'primitive', sources['edited'], PRIMITIVE, sequence_path=scripts['primitive'], sun=1.)
                for name, _ in PRIMITIVE: check(name, technique, primitive_state(records[name]['state'], name))
                check('primitive-redo-exact', technique, image_compare(records['created'], records['redo']), pixels=True)
                check('primitive-undo-blank', technique, image_compare(records['undo'], records['undo-again']), pixels=True)
                check('primitive-nonvacuous', technique, image_compare(records['created'], records['undo'], contrast=True, minimum=.1), pixels=True)
                continue
            if args.cohort == 'ray-ddgi':
                records = launch(args, output, report, persist, verify, technique, 'ray', sources['ray'], RAY, sequence_path=scripts['ray'], ray=True)
                previous = None
                for name, _ in RAY:
                    result = ray_state(records[name]['state'], records['prepared']['state'], name, previous)
                    previous = (result['rayGeneration'], result['ddgiContentEpoch']); check(name, technique, result)
                continue
            if args.cohort == 'edited':
                controls = {name: launch(args, output, report, persist, verify, technique, name, sources[name], [(name, 13)], sun=1.)[name] for name in ('baked-before', 'baked-copy')}
                glb = output/'exports'/technique/'binary'/'edited.glb'
                records = launch(args, output, report, persist, verify, technique, 'edited', sources['edited'], EDITED, sequence_path=scripts['edited'], save=glb, sun=1.)
                check('loaded', technique, loaded_state(records['loaded']['state']))
                for name, _ in EDITED[1:]: check(name, technique, edited_state(records[name]['state'], records['prepared']['state'], name))
                for first, second in [('prepared', 'undo-clone'), ('copy', 'undo-transform'), ('copy', 'redo-clone'), ('offset', 'redo-transform'),
                        ('offset', 'undo-empty'), ('empty-owner', 'redo-empty'), ('empty-owner', 'shared')]:
                    check(first+'-'+second+'-exact', technique, image_compare(records[first], records[second]), pixels=True)
                for name, oracle in [('prepared', 'baked-before'), ('offset', 'baked-copy'), ('shared', 'baked-copy')]:
                    check(name+'-independent-literal', technique, image_compare(records[name], controls[oracle], regions=STATIC_ROIS), pixels=True)
                check('independent-control-visible', technique, visible_regions(controls['baked-copy'], [(236, 126), (223, 234), (168, 234), (277, 234), (417, 126), (349, 234), (458, 234)]), pixels=True)
                check('independent-copy-contrast', technique, image_compare(controls['baked-before'], controls['baked-copy'], regions=STATIC_ROIS, contrast=True, minimum=1.), pixels=True)
                # Default MaterialX appearance is view-dependent; its two visible
                # interiors supplement exact shared IDs and exported literal positions.
                check('procedural-duplicate-visible', technique, visible_regions(records['offset'], [(320, 180), (501, 180)]), pixels=True)
                ascii_path = output/'exports'/technique/'ascii'/'edited.gltf'
                ascii_records = launch(args, output, report, persist, verify, technique, 'ascii', sources['edited'], [('ascii-empty', 26)], sequence_path=scripts['ascii'], save=ascii_path, sun=1.)
                check('ascii-empty', technique, edited_state(ascii_records['ascii-empty']['state'], records['prepared']['state'], 'empty', same_session=False))
                for suffix, exported in (('glb', glb), ('gltf', ascii_path)):
                    report['exports'].append(dict(technique=technique, format=suffix, **inspect_edited_export(exported))); persist()
                    copied, files = portable_copy(exported, output/'portable'/technique/suffix); report['portableCopies'].append(dict(document=str(copied), files=files)); persist()
                    labels = [('shared-a', 13), ('shared-b', 26), ('empty', 39)]
                    reloaded = launch(args, output, report, persist, verify, technique, 'reload-'+suffix, copied, labels, sequence_path=scripts['reload'], sun=1.)
                    for scene, (name, _) in enumerate(labels):
                        check('reload-'+suffix+'-'+name, technique, reload_edited_state(reloaded[name]['state'], copied, scene, reloaded['shared-a']['state']))
                        check('reload-'+suffix+'-'+name+'-pixels', technique, image_compare(reloaded[name], records['empty'] if scene == 2 else controls['baked-copy'],
                            regions=None if scene == 2 else STATIC_ROIS), pixels=True)
                continue
            binding = args.cohort == 'binding'; kind = args.cohort
            controls = {}
            for name in ('before', 'copy', 'offset', 'seek', 'undo-transform', 'zero', 'rest'):
                key = kind+'-control-'+name
                controls[name] = launch(args, output, report, persist, verify, technique, key, sources[key], [(name, 13)], camera=plan['controlCameraNodes'][key])[name]
            labels = [('loaded', 13), ('before', 26)]+[(name, frame+13) for name, frame in ANIMATED[1:]] if binding else ANIMATED
            glb = output/'exports'/technique/'binary'/('binding-rejected.glb' if binding else 'animated.glb')
            records = launch(args, output, report, persist, verify, technique, kind, sources['animated'], labels, sequence_path=scripts[kind], save=glb,
                animated=True, camera=3, export_rejected=binding)
            for name, _ in labels:
                check(kind+'-'+name, technique, animated_state(records[name]['state'], records[name if name == 'loaded' else 'before']['state'], name, binding=binding))
                if name == 'loaded': continue
                oracle = {'undo-clone': 'before', 'redo-clone': 'undo-transform', 'redo-transform': 'seek', 'reselect': 'offset'}.get(name, name)
                time, copied, offset = animation_spec(name)
                check(kind+'-'+name+'-independent-world', technique, image_compare(records[name], controls[oracle],
                    regions=dynamic_regions(time, binding=binding, copied=copied, offset=offset)), pixels=True)
            check(kind+'-time-control-contrast', technique, image_compare(controls['offset'], controls['seek'], contrast=True, minimum=.1), pixels=True)
            check(kind+'-authored-rest-contrast', technique, image_compare(controls['rest'], controls['zero'], contrast=True, minimum=.1), pixels=True)
            check(kind+'-undo-clock-rewind', technique, image_compare(records['before'], records['undo-clone']), pixels=True)
            check(kind+'-redo-clock-restore', technique, image_compare(records['seek'], records['redo-transform']), pixels=True)
            if binding:
                report['exports'].append(dict(technique=technique, rejected=True, expectedGuard='post-skin primitive transforms or reparenting require baking before export',
                    postFailureStateObserved=False, portabilityClaim=False)); persist(); continue
            ascii_path = output/'exports'/technique/'ascii'/'animated.gltf'
            ascii_records = launch(args, output, report, persist, verify, technique, 'animated-ascii', sources['animated'], [('ascii', 13)],
                sequence_path=scripts['animated-ascii'], save=ascii_path, animated=True, camera=3)
            check('animated-ascii-midpoint', technique, animated_state(ascii_records['ascii']['state'], records['before']['state'], 'offset', same_session=False))
            for suffix, exported in (('glb', glb), ('gltf', ascii_path)):
                inspection = inspect_animated_export(exported, sources['animated']); report['exports'].append(dict(technique=technique, format=suffix, **inspection)); persist()
                copied, files = portable_copy(exported, output/'portable'/technique/suffix); report['portableCopies'].append(dict(document=str(copied), files=files)); persist()
                labels = [('zero', 13), ('midpoint', 26), ('one', 39)]
                reloaded = launch(args, output, report, persist, verify, technique, 'reload-'+suffix, copied, labels,
                    sequence_path=scripts['animated-reload'], animated=True, camera=inspection['cameraSourceNode'])
                for name, time, oracle in [('zero', 0., 'zero'), ('midpoint', .5, 'offset'), ('one', 1., 'seek')]:
                    check('reload-'+suffix+'-'+name, technique, reload_animated_state(reloaded[name]['state'], inspection, time, reloaded['zero']['state']))
                    check('reload-'+suffix+'-'+name+'-independent', technique, image_compare(reloaded[name], controls[oracle],
                        regions=dynamic_regions(time, copied=True, offset=True)), pixels=True)
        expected_captures, expected_processes = COUNTS[args.cohort]
        require(len(report['captures']) == expected_captures*len(args.technique) and len(report['processes']) == expected_processes*len(args.technique), 'Incomplete clone capture/process cohort')
        require(tuple(len(report[key]) for key in ('stateChecks', 'imageChecks', 'exports')) ==
            tuple(count*len(args.technique) for count in CHECK_COUNTS[args.cohort]), 'Incomplete clone state/image/export checks')
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


def synthetic_edited(name):
    """Literal verifier witness; never counted as renderer/native evidence."""
    loaded = name == 'loaded'; copied = not loaded and name not in ('prepared', 'undo-clone')
    empty_owner = name in ('empty-owner', 'redo-empty', 'shared', 'empty')
    offset = name in ('offset', 'redo-transform', 'empty-owner', 'undo-empty', 'redo-empty', 'shared', 'empty')
    scene = 1 if name == 'shared' else 2 if name == 'empty' else 0
    parents = {0: 7, 1: 0, 2: 1 if loaded else 8, 3: 1 if loaded else 10, 4: 0 if loaded else 9,
        5: 4, 6: 4, 7: INVALID, 8: 0, 9: 8, 10: 0, 11: 9}
    locals_ = {i: np.eye(4) for i in range(12)}
    locals_.update({0: SHEAR, 1: translation(-1.25), 2: np.eye(4) if loaded else translation(-1.25),
        3: np.eye(4) if loaded else translation(-1.25), 4: translation(-1.25, -.75),
        5: translation(-.75)@np.diag([.5, .5, 1., 1.]), 6: translation(.75)@np.diag([.5, .5, 1., 1.])})
    names = {0: 'Shared root', 1: 'Detached owner A', 2: 'Primitive 0', 3: 'Primitive 1', 4: 'Instanced owner B',
        5: 'Primitive 0 instance 0', 6: 'Primitive 0 instance 1', 7: 'Scene Root', 8: E, 9: F, 10: OUTSIDE, 11: PROCEDURAL}
    sources = {0: 0, 1: 1, 4: 2}; primitive_ids = {2: 0, 3: 1, 5: 2, 6: 2, 11: 3}; owners = {2: 1, 3: 1, 5: 4, 6: 4}
    nodes = {}
    def row(i, local, parent, source, label, old=None):
        value = dict(graphNode=i, sourceNode=source, name=label, parentGraphNode=parent, meshOwnerGraphNode=owners.get(i, INVALID),
            renderable=i in primitive_ids, retired=False, active=scene != 2 and i != 7, primitiveIndex=primitive_ids.get(i, INVALID),
            materialIndex={2: 1, 3: 2, 5: 3, 6: 3}.get(i, 0), sourceMesh=0 if i in (2, 3) else 1 if i in (5, 6) else -1,
            sourcePrimitive=1 if i == 3 else 0 if i in (2, 5, 6) else -1, sourceInstance={5: 0, 6: 1}.get(i, -1),
            localColumns=local.T.tolist(), worldColumns=[])
        if old:
            value = dict(copy.deepcopy(old), graphNode=i, sourceNode=source, name=label, parentGraphNode=parent, localColumns=local.T.tolist())
        return value
    for i in range(8 if loaded else 12): nodes[i] = row(i, locals_[i], parents[i], sources.get(i, -1), names[i])
    if copied:
        mapping = {0: 12, 1: 13, 2: 14, 4: 15, 5: 16, 6: 17, 8: 18, 9: 19, 11: 20}
        for old, new in mapping.items():
            source = {0: 3, 1: 4, 4: 5}.get(old, -1); parent = parents[old] if old == 8 else mapping.get(parents[old], parents[old])
            local = translation(OFFSET) if old == 8 and offset else locals_[old]
            nodes[new] = row(new, local, parent, source, COPY if old == 8 else names[old], nodes[old])
            nodes[new]['meshOwnerGraphNode'] = mapping.get(nodes[new]['meshOwnerGraphNode'], nodes[new]['meshOwnerGraphNode'])
            if old in (0, 1): nodes[new].update(renderable=False, primitiveIndex=INVALID, meshOwnerGraphNode=INVALID, sourceMesh=-1, sourcePrimitive=-1, sourceInstance=-1)
        if empty_owner: nodes[21] = row(21, locals_[1], 0, 6, EMPTY_COPY)
    worlds = {}
    def world(i):
        if i not in worlds:
            parent = nodes[i]['parentGraphNode']; worlds[i] = matrix(nodes[i], 'localColumns') if parent == INVALID else world(parent)@matrix(nodes[i], 'localColumns')
        return worlds[i]
    for i in nodes: nodes[i]['worldColumns'] = world(i).T.tolist(); nodes[i]['active'] = scene != 2 and i != 7
    cursors = {'loaded': (0, 0), 'prepared': (8, 0), 'copy': (9, 0), 'offset': (10, 0), 'undo-transform': (9, 1), 'undo-clone': (8, 2),
        'redo-clone': (9, 1), 'redo-transform': (10, 0), 'empty-owner': (11, 0), 'undo-empty': (10, 1), 'redo-empty': (11, 0), 'shared': (0, 0), 'empty': (0, 0)}
    undo, redo = cursors[name]; draws = 0 if scene == 2 else 4 if loaded else 9 if copied else 5
    return dict(gltfMerge=dict(authoringNodes=list(nodes.values()), authoringNodesTruncated=False, graphNodeCount=len(nodes), graphIdentity=101,
            sourceTables=dict(SOURCE_TABLES, nodes=7 if empty_owner else 6 if copied else 3), activeRoots=[] if scene == 2 else [0, 12] if copied else [0],
            sourceReadCount=0, descriptorGeneration=0, commitGeneration=0),
        gltfScenes=dict(authoringNodeCount=len(nodes), selected=scene, activeRenderableCount=draws, editHistoryDepth=undo+redo),
        gltfMaterialVariants=dict(undoCount=undo, redoCount=redo),
        scene=dict(primaryObjectCount=draws, primaryVertexCount=12 if loaded else 16, primaryIndexCount=18 if loaded else 24, primaryImportScale=1.),
        gltfInstancing=dict(drawCount=draws), gui=dict(modelLoading=dict(pending=False), selectedMeshNodeId=2 if loaded else INVALID if scene else 21 if empty_owner else 18 if copied else 11),
        gltfNodeFlags=dict(nodes=[dict(graphNode=i, localVisible=True, localSelectable=True, localHoverable=i not in (4, 15) or loaded) for i in nodes]))


def synthetic_deformation(time, copied=True, offset=True, binding=False):
    values = dynamic_values(time); instances = []
    for i, (local, model, _, _) in enumerate(dynamic_geometry(time, copied=copied, offset=offset, binding=binding)):
        instances.append(dict(sourceNodeIndex=(0, 5)[i], vertexCount=4, indexCount=6, boundsValid=True, weights=[values['weight']],
            firstPosition=local[0].tolist(), boundsMin=local.min(axis=0).tolist(), boundsMax=local.max(axis=0).tolist(), firstNormal=[0., 0., 1.],
            modelColumns=model.T.tolist(), vertexOffset=8+4*i, firstIndex=12+6*i))
    count = len(instances)
    return dict(gltfDeformation=dict(active=True, gpuReady=True, currentBoundToGpu=True, previousBoundToGpu=True,
        instanceCount=count, sourceVertexCount=8, expandedVertexCount=8+4*count, expandedIndexCount=12+6*count, instances=instances))


def synthetic_edited_export(path):
    asset = Asset(); nodes = rows(synthetic_edited('empty')); result = []
    asset.model['materials'] = [material('Synthetic source', [1., .25, .125])]
    asset.model['meshes'] = [dict(primitives=[primitive(asset, points, 0, tag)]) for points, tag in
        [(QUAD+[0., .75, 0.], 'a0'), (QUAD+[0., -.75, 0.], 'a1'), (QUAD, 'b0'),
         (np.asarray([[-.5, -.5, 0.], [.5, -.5, 0.], [.5, .5, 0.], [-.5, .5, 0.]]), 'procedural')]]
    for i, row in nodes.items():
        value = dict(name=row['name'], matrix=matrix(row, 'localColumns').T.reshape(-1).tolist())
        children = [j for j, child in nodes.items() if child['parentGraphNode'] == i and j not in (0, 12)]
        if children: value['children'] = children
        if row['renderable']: value['mesh'] = row['primitiveIndex']
        tag = {0: 'root', 1: 'owner-a', 4: 'owner-b', 12: 'root', 13: 'owner-a', 15: 'owner-b', 21: 'owner-a'}.get(i)
        if tag: value['extras'] = dict(cloneTag=tag)
        if i in (5, 6, 16, 17):
            x = -.75 if i in (5, 16) else .75; identity = 16777217 if x < 0. else 16777219
            value['matrix'] = np.eye(4).reshape(-1).tolist()
            value['extensions'] = {INSTANCING: dict(attributes=dict(TRANSLATION=asset.accessor('VEC3', [[x, 0., 0.]]),
                SCALE=asset.accessor('VEC3', [[.5, .5, 1.]]), _ID=asset.accessor('SCALAR', [identity], 5125)), extras=dict(opaqueInstanceIndex=977))}
        if i in (4, 15): value.setdefault('extensions', {})['KHR_node_hoverability'] = dict(hoverable=False)
        result.append(value)
    asset.model['nodes'] = result; asset.model['scene'] = 2
    asset.model['scenes'] = [dict(name=name, nodes=roots, extras=dict(cloneTag=tag)) for name, roots, tag in
        [('Shared A', [0, 12], 'scene-a'), ('Shared B', [0, 12], 'scene-b'), ('Empty', [], 'scene-empty')]]
    asset.model['extensionsUsed'] = [INSTANCING, 'KHR_materials_unlit', 'KHR_node_hoverability']; write_asset(asset, path)


def synthetic_animated_export(path, source):
    original, buffers = load_asset(source); asset = Asset(); asset.model = copy.deepcopy(original); asset.data = bytearray(buffers[0]); v = dynamic_values(.5)
    asset.model['materials'][0]['pbrMetallicRoughness']['baseColorFactor'] = v['color'].tolist()
    asset.model['cameras'][0]['perspective']['yfov'] = v['fov']
    owner = dict(name=ANIMATION_NAME, translation=[-1.25, 0., 0.], scale=[v['scale'], 1., 1.], extras=dict(cloneTag='skin-owner'))
    ancestor = dict(name=JOINT_PARENT, translation=[0., .25, 0.], extras=dict(cloneTag='joint-context'))
    joint = dict(name=JOINT, translation=v['joint'].tolist(), extras=dict(cloneTag='joint'))
    asset.model['nodes'] = [dict(owner, children=[1]), dict(name='Primitive 0', mesh=0, skin=0, weights=[v['weight']]),
        dict(ancestor, camera=0, children=[3, 4, 12]), dict(name='Primitive 0', mesh=1), dict(joint),
        copy.deepcopy(original['nodes'][3]), dict(name='Authored lamp', translation=[0., 1.5, 3.], children=[13]), dict(name='Scene Root'),
        dict(owner, name='Animated copy', children=[9]), dict(name='Primitive 0', mesh=0, skin=1, weights=[v['weight']]),
        dict(ancestor, translation=[OFFSET, .25, 0.], children=[11]), dict(joint),
        dict(name='Point overlay 0', extensions={PUNCTUAL: dict(light=1)}), dict(name='Point overlay 1', extensions={PUNCTUAL: dict(light=2)})]
    skin = copy.deepcopy(original['skins'][0]); asset.model['skins'] = [dict(skin, joints=[4], skeleton=2), dict(skin, joints=[11], skeleton=10)]
    asset.model['scenes'] = [dict(name='Animated clone', nodes=[0, 2, 5, 6, 8, 10])]
    light = copy.deepcopy(original['extensions'][PUNCTUAL]['lights'][0]); light['intensity'] = v['intensity']
    asset.model['extensions'][PUNCTUAL]['lights'] = [copy.deepcopy(light) for _ in range(3)]
    channels = asset.model['animations'][0]['channels']; channels[0]['target']['node'] = 4; channels[1]['target']['node'] = 1
    for index, node, path_name in [(0, 11, 'translation'), (1, 9, 'weights'), (2, 8, 'scale/0')]:
        channel = copy.deepcopy(original['animations'][0]['channels'][index])
        if index == 2: channel['target']['extensions'][POINTER]['pointer'] = '/nodes/'+str(node)+'/'+path_name
        else: channel['target']['node'] = node
        channels.append(channel)
    for light in (1, 2):
        channel = copy.deepcopy(original['animations'][0]['channels'][4])
        channel['target']['extensions'][POINTER]['pointer'] = '/extensions/KHR_lights_punctual/lights/'+str(light)+'/intensity'; channels.append(channel)
    write_asset(asset, path)


def self_check():
    rejected = []
    def reject(name, function):
        try: function()
        except (RuntimeError, KeyError, IndexError, ValueError): rejected.append(name)
        else: raise RuntimeError('Clone verifier accepted mutation: '+name)
    with tempfile.TemporaryDirectory(prefix='subtree-clone-selfcheck-') as temporary:
        root = Path(temporary); plan = generate(root/'first', 'edited'); again = generate(root/'again', 'edited')
        captures = root/'capture-namespace'; captures.mkdir(); target = captures/'deferred-raster-animated'
        for suffix in ('-control-before.png', '-control-before.telemetry.json', '-control-before.log', '-ascii.png'):
            (captures/(target.name+suffix)).write_text('independent process', encoding='utf-8')
        require_fresh_capture(target)
        require(capture_artifacts(target) == [], 'Capture inventory includes independent prefix siblings')
        for suffix in ('.png', '.log', '.telemetry.json', '.frame-0013.png', '.frame-0013.telemetry.json', '.frame-10000.png'):
            path = captures/(target.name+suffix); path.write_text('retained evidence', encoding='utf-8')
            require(capture_artifacts(target) == [path], 'Exact process/frame output is absent from protected inventory')
            reject('existing-capture'+suffix, lambda: require_fresh_capture(target))
            path.unlink()
        require([row['sha256'] for row in plan['immutableInputs']] == [row['sha256'] for row in again['immutableInputs']], 'Nondeterministic clone inputs')
        source = Path(plan['sources']['edited']); model, _ = load_asset(source)
        attrs = model['nodes'][2]['extensions'][INSTANCING]['attributes']
        require(exact_uint32(source, attrs['_ID']) == [16777217, 16777219], 'Incorrect exact source UINT32 IDs')
        require(len(edited_world_quads(False)) == 4 and len(edited_world_quads(True)) == 7, 'Literal source geometry controls changed')
        loaded_state(synthetic_edited('loaded')); prepared = synthetic_edited('prepared')
        for name, _ in EDITED[1:]: edited_state(synthetic_edited(name), prepared, name)
        for name, mutate in [('retained-source-identity', lambda s: s['gltfMerge']['authoringNodes'][0].__setitem__('sourceNode', 8)),
                ('pulled-external-sibling', lambda s: s['gltfMerge']['authoringNodes'][13].__setitem__('renderable', True)),
                ('copied-source-geometry', lambda s: s['scene'].__setitem__('primaryVertexCount', 20)),
                ('bad-root-order', lambda s: s['gltfMerge'].__setitem__('activeRoots', [12, 0])),
                ('old-source-binding-alias', lambda s: s['gltfMerge']['authoringNodes'][14].__setitem__('meshOwnerGraphNode', 1)),
                ('lost-procedural-local', lambda s: s['gltfMerge']['authoringNodes'][20]['localColumns'][3].__setitem__(0, 5.)),
                ('omitted-full-evidence', lambda s: s['gltfMerge'].__setitem__('authoringNodesTruncated', True)),
                ('wrong-history', lambda s: s['gltfMaterialVariants'].__setitem__('undoCount', 0)),
                ('wrong-root-selection', lambda s: s['gui'].__setitem__('selectedMeshNodeId', 14)),
                ('lost-local-hover', lambda s: s['gltfNodeFlags']['nodes'][15].__setitem__('localHoverable', True))]:
            bad = synthetic_edited('offset'); mutate(bad); reject(name, lambda: edited_state(bad, prepared, 'offset'))
        bad = synthetic_edited('shared'); bad['gltfMaterialVariants'] = {}
        reject('missing-post-authoring-zero-counters', lambda: edited_state(bad, prepared, 'shared'))
        for binding in (False, True):
            for time in (None, 0., .5, 1.):
                check_deformation(synthetic_deformation(time, binding=binding), time, copied=True, offset=True, binding=binding)
        for name, mutate in [('aliased-skin-ranges', lambda s: s['gltfDeformation']['instances'][1].__setitem__('vertexOffset', 8)),
                ('lost-binding-owner', lambda s: s['gltfDeformation']['instances'][1].__setitem__('sourceNodeIndex', 0)),
                ('stale-morph-weight', lambda s: s['gltfDeformation']['instances'][1].__setitem__('weights', [.15])),
                ('wrong-post-skin-world', lambda s: s['gltfDeformation']['instances'][1]['modelColumns'][3].__setitem__(0, -9.)),
                ('wrong-IBM-order', lambda s: s['gltfDeformation']['instances'][1]['firstPosition'].__setitem__(0, 9.)),
                ('lost-authored-frame', lambda s: s['gltfDeformation']['instances'][1].__setitem__('vertexCount', 6))]:
            bad = synthetic_deformation(1., binding=True); mutate(bad)
            reject(name, lambda: check_deformation(bad, 1., copied=True, offset=True, binding=True))
        source = Path(plan['sources']['animated']); animation, _ = load_asset(source)
        require(len(animation['animations'][0]['channels']) == 6 and animation['nodes'][0]['weights'] == [.15] and
            animation['nodes'][2]['translation'] == [-.25, -.25, 0.], 'Independent authored rest was replaced by first-key/evaluated pose')
        for channel in animation['animations'][0]['channels']:
            sampler = animation['animations'][0]['samplers'][channel['sampler']]
            close(decode_accessor(source, sampler['input']).reshape(-1), [0., 2.], 'strict source animation bounds', 0.)
        require(dynamic_values(None)['weight'] != dynamic_values(0.)['weight'] and
            not np.array_equal(dynamic_geometry(.5)[0][2], dynamic_geometry(1.)[0][2]) and
            not np.array_equal(dynamic_geometry(1., binding=True)[0][2], dynamic_geometry(1.)[0][2]), 'Independent animation/binding/rest controls are vacuous')
        for key, sequence in plan['scripts'].items():
            request = json.loads(Path(sequence).read_text()); frames = [row['frame'] for row in request['events']]
            require(len(frames) == len(set(frames)) and all(1 <= frame <= request['frames'] for frame in frames) and
                request['sampleFrames'][-1] == request['frames'], 'Invalid/colliding capture schedule: '+key)
        exported = root/'synthetic-edited.gltf'; synthetic_edited_export(exported); inspect_edited_export(exported)
        original_export = json.loads(exported.read_text())
        for name, mutate in [('export-lost-empty-owner-metadata', lambda m: m['nodes'][21].pop('extras')),
                ('export-lost-shared-scene', lambda m: m['scenes'][1].__setitem__('nodes', [0])),
                ('export-invented-source-primitive', lambda m: m['nodes'][13].__setitem__('mesh', 0)),
                ('export-lost-instance-metadata', lambda m: m['nodes'][16]['extensions'][INSTANCING].__setitem__('extras', {})),
                ('export-stale-copied-world', lambda m: m['nodes'][18]['matrix'].__setitem__(12, 0.))]:
            bad = copy.deepcopy(original_export); mutate(bad); path = root/(name+'.gltf'); document(path, bad)
            reject(name, lambda: inspect_edited_export(path))
        exported = root/'synthetic-animated.gltf'; synthetic_animated_export(exported, source); inspect_animated_export(exported, source)
        original_export = json.loads(exported.read_text())
        for name, mutate in [('export-skin-original-joint-alias', lambda m: m['skins'][1].__setitem__('joints', [4])),
                ('export-stale-saved-morph', lambda m: m['nodes'][9].__setitem__('weights', [.15])),
                ('export-missing-same-clip-component', lambda m: m['animations'][0]['channels'].pop(8)),
                ('export-joint-context-camera', lambda m: m['nodes'][10].__setitem__('camera', 0)),
                ('export-lost-light-alias-track', lambda m: m['animations'][0]['channels'].pop())]:
            bad = copy.deepcopy(original_export); mutate(bad); path = root/(name+'.gltf'); document(path, bad)
            reject(name, lambda: inspect_animated_export(path, source))
        require(COUNTS == {'edited': (22, 6), 'primitive': (6, 1), 'animated': (25, 11), 'binding': (19, 8), 'ray-ddgi': (4, 1)}, 'Capture completeness constants changed')
    return dict(passed=True, rendererExecuted=False, deterministicInputs=True, literalGraphWorldAndUINT32=True,
        staticHistoryStates=len(EDITED), independentDeformationStates=8, verifierMutationsRejected=rejected,
        syntheticExportInspections=2,
        independentPrefixSiblingsAccepted=True,
        plannedCapturesPerTechnique={key: row[0] for key, row in COUNTS.items()})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path); parser.add_argument('--manifest', type=Path); parser.add_argument('--output', type=Path)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES); parser.add_argument('--cohort', choices=COHORTS, default='edited')
    parser.add_argument('--timeout', type=float, default=600.); parser.add_argument('--generate-only', action='store_true'); parser.add_argument('--self-check', action='store_true')
    args = parser.parse_args()
    if args.self_check: print(json.dumps(self_check(), indent=2)); return
    if args.output is None: parser.error('--output is required')
    if not args.generate_only and (args.renderer is None or args.manifest is None): parser.error('Native execution requires explicit --renderer and --manifest')
    if not math.isfinite(args.timeout) or args.timeout <= 0.: parser.error('--timeout must be finite and positive')
    args.technique = tuple(dict.fromkeys(args.technique or TECHNIQUES)); output = args.output.resolve(); plan = generate(output, args.cohort)
    expected_captures, expected_processes = COUNTS[args.cohort]
    report = dict(schemaVersion=1, cohort=args.cohort, passed=None if args.generate_only else False, generatedOnly=args.generate_only,
        rendererExecuted=False, nativeValidationPerformed=False, sourceDependencies=dependencies(), launchPlanSha256=sha(output/'launch-plan.json'),
        renderer=str(args.renderer.resolve()) if args.renderer else None, manifest=str(args.manifest.resolve()) if args.manifest else None,
        expectedCaptureCount=expected_captures*len(args.technique), expectedProcessCount=expected_processes*len(args.technique),
        captures=[], stateChecks=[], imageChecks=[], exports=[], processes=[], portableCopies=[], unverified=UNVERIFIED)
    if args.generate_only:
        document(output/'results.json', report); print('GENERATED', report['expectedCaptureCount'], 'planned captures; renderer not executed')
    else:
        run_native(args, output, plan, report); print('PASS', len(report['captures']), 'captures;', len(report['stateChecks']), 'state checks;', len(report['imageChecks']), 'image checks')


if __name__ == '__main__': main()
