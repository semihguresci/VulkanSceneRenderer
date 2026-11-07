"""Native behavior preservation across scene/resource retirement and portable save.

Literal graph-free assets are the image controls. Exact graph/reference inspection,
CPU identities and analytical matrices are checked separately from native raster
images. --self-check and --generate-only never launch the renderer. Native work
requires a frozen executable/DLL/SPIR-V/helper manifest and produces 48 images.
The retained onSelect handler is inspected structurally; this is not a picking test.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import traceback

import numpy as np
from PIL import Image

from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, mesh, require
from gltf_extended_materials_runtime_regression import sha, validation_log_error
from gltf_material_variants_runtime_regression import run
from gltf_merge_runtime_regression import document, immutable, portable_copy
from gltf_resource_retirement_runtime_regression import resource_catalog
from gltf_texture_formats_runtime_regression import verify_manifest
from scene_node_authoring_runtime_regression import history_cursor

BEHAVIOR, LIGHT, SELECT = 'KHR_interactivity', 'KHR_lights_punctual', 'KHR_node_selectability'
TECHNIQUES = ('deferred-raster', 'forward-raster')
WIDTH, HEIGHT = 640, 360
DEAD, SURFACE, CAMERA, LIGHT_NODE = 'Retire unrelated node', 'Behavior surface', 'Behavior camera', 'Behavior light owner'
MATERIAL, LIGHT_NAME = 'Behavior material', 'Behavior light'
POSITIONS = [[-1.25, -.8, 0.], [1.25, -.8, 0.], [1.25, .8, 0.], [-1.25, .8, 0.]]
INDICES = [0, 1, 2, 0, 2, 3]
OPAQUE = dict(label='/nodes/1', material='/materials/1', light='/extensions/KHR_lights_punctual/lights/1', integer=16777217)


def f32(value):
    return np.asarray(value, dtype=np.float32).astype(float).tolist()


def pose(label):
    rows = dict(
        rest=dict(translation=[0., 0., 0.], rotation=[0., 0., 0., 1.], scale=[1., 1., 1.],
                  color=[.65, .45, .25, 1.], yfov=.8, intensity=14., lightColor=[1., .7, .4]),
        dim=dict(translation=[-.3, 0., 0.], rotation=[0., 0., 0., 1.], scale=[.85, .85, 1.],
                 color=[.35, .65, .25, 1.], yfov=.95, intensity=6., lightColor=[.4, .7, 1.]),
        # This rational unit quaternion stays bit-identical through double
        # normalization and both float divide/multiply normalization paths.
        # It keeps the image oracle independent of their one-ULP differences.
        bright=dict(translation=[.25, .1, 0.], rotation=[0., 0., 11/61, 60/61],
                    scale=[1.1, .9, 1.], color=[.7, .3, .15, 1.], yfov=.65, intensity=30., lightColor=[1., .4, .2]))
    result = copy.deepcopy(rows['bright' if label == 'dark' else label])
    if label == 'dark': result['intensity'] = 0.
    return {key: f32(value) for key, value in result.items()}


def config(*values): return dict(value=list(values))
def literal(kind, *values): return dict(type=kind, value=list(values))


def graph(*, on_start=False):
    """Authored graph construction is independent of the production remapper."""
    result = dict(types=[dict(signature=t) for t in ('float', 'float3', 'float4', 'ref')],
        variables=[literal(3, '/materials/1')],
        events=[dict(id=name, values=dict(owner=literal(3, '/nodes/2'))) for name in ('dim', 'bright')],
        declarations=[dict(op='event/receive'), dict(op='pointer/set'),
                      dict(op='event/onSelect', extension=SELECT)], nodes=[], extras=copy.deepcopy(OPAQUE))
    for event, name in enumerate(('dim', 'bright')):
        start = len(result['nodes'])
        result['nodes'].append(dict(declaration=0, configuration=dict(event=config(event)),
                                    flows=dict(out=dict(node=start+1)), extras=dict(handler=name)))
        row = pose(name)
        bindings = [('/nodes/2/translation', 1, row['translation']), ('/nodes/2/rotation', 2, row['rotation']),
                    ('/nodes/2/scale', 1, row['scale']), ('/materials/1/pbrMetallicRoughness/baseColorFactor', 2, row['color']),
                    ('/cameras/0/perspective/yfov', 0, [row['yfov']]),
                    (f'/extensions/{LIGHT}/lights/1/intensity', 0, [row['intensity']]),
                    (f'/extensions/{LIGHT}/lights/1/color', 1, row['lightColor'])]
        for index, (pointer, kind, value) in enumerate(bindings):
            node = dict(declaration=1, configuration=dict(pointer=config(pointer), type=config(kind)),
                        values=dict(value=literal(kind, *value)), extras=copy.deepcopy(OPAQUE))
            if index+1 < len(bindings): node['flows'] = dict(out=dict(node=len(result['nodes'])+1))
            result['nodes'].append(node)
    # This dormant retained handler supplies a real typed selection reference,
    # without pretending capture custom events are native pointer picking.
    result['nodes'].append(dict(declaration=2, configuration=dict(nodeIndex=config(2)),
                                extras=dict(selection=copy.deepcopy(OPAQUE))))
    if on_start:
        result['declarations'].append(dict(op='event/onStart'))
        result['nodes'].append(dict(declaration=3, flows=dict(out=dict(node=len(result['nodes'])+1)), extras=dict(start='bright authored behavior')))
        # Handlers use their own later chain: the supported bounded flow
        # profile deliberately rejects links to an earlier graph node.
        for index, original in enumerate(copy.deepcopy(result['nodes'][9:16])):
            if index < 6: original['flows'] = dict(out=dict(node=len(result['nodes'])+1))
            else: original.pop('flows', None)
            result['nodes'].append(original)
    return result


def write_fixture(path, label='rest', *, behavior=False, on_start=False):
    row = pose(label); asset = Asset(); model = asset.model
    white = np.full((2, 2, 4), 255, dtype=np.uint8)
    for name in ('Unused image', 'Surviving image'):
        index = asset.image(white); model['images'][index]['name'] = name
        model['textures'][index]['name'] = name.replace('image', 'texture')
        model['samplers'][index]['name'] = name.replace('image', 'sampler')
    model['materials'] = [dict(name='Unused original material', pbrMetallicRoughness=dict(
        baseColorFactor=[.2, .2, .2, 1.], baseColorTexture=dict(index=0), metallicFactor=0., roughnessFactor=1.)),
        dict(name=MATERIAL, pbrMetallicRoughness=dict(baseColorFactor=row['color'], baseColorTexture=dict(index=1),
            metallicFactor=0., roughnessFactor=1.), doubleSided=True, extras=copy.deepcopy(OPAQUE))]
    primitive = mesh(asset, asset.accessor('VEC3', POSITIONS),
                     asset.accessor('VEC3', [[0., 0., 1.]]*4), asset.accessor('VEC2', [[0., 0.], [1., 0.], [1., 1.], [0., 1.]]))
    primitive['material'] = 1
    model['meshes'][0].update(name='Surviving geometry', extras=copy.deepcopy(OPAQUE))
    model['nodes'] = [dict(name=DEAD, children=[1], extras=dict(retirement='unrelated empty subtree')),
        dict(name='Retire unrelated child'),
        dict(name=SURFACE, mesh=0, **{key: row[key] for key in ('translation', 'rotation', 'scale')}, extras=copy.deepcopy(OPAQUE)),
        dict(name=CAMERA, camera=0, translation=[0., 0., 4.], extras=dict(cameraOwner=37)),
        dict(name=LIGHT_NODE, translation=[0., 0., 2.], extensions={LIGHT: dict(light=1)}, extras=dict(lightOwner=41))]
    model['cameras'] = [dict(name='Behavior lens', type='perspective', perspective=dict(yfov=row['yfov'], znear=.1, zfar=20.),
                             extras=dict(lens='/cameras/0'))]
    model['scenes'] = [dict(name='Behavior export scene', nodes=[0, 2, 3, 4], extras=copy.deepcopy(OPAQUE))]
    model['extensions'] = {LIGHT: dict(lights=[dict(name='Unused light definition', type='point', intensity=0.),
        dict(name=LIGHT_NAME, type='point', intensity=row['intensity'], color=row['lightColor'], range=10., extras=copy.deepcopy(OPAQUE))])}
    model['extensionsUsed'] = [LIGHT]
    model['extras'] = copy.deepcopy(OPAQUE)
    if behavior:
        model['extensions'][BEHAVIOR] = dict(graphs=[graph(on_start=on_start)], graph=0, extras=dict(graphOwner='preserve extension extras'))
        model['extensionsUsed'] += [BEHAVIOR, SELECT]
        model['extensionsRequired'] = [BEHAVIOR]
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='behavior-export-fixture-', dir=path.parent) as temporary:
        candidate = Path(temporary)/'fixture.gltf'; asset.write(candidate); immutable(path, candidate.read_bytes())


SOURCE_STEPS = [dict(label=label, frame=frame, pose=state, deleted=deleted, retired=retired, history=list(history))
    for label, frame, state, deleted, retired, history in [
        ('rest', 13, 'rest', False, False, (0, 0)), ('deleted', 26, 'rest', True, False, (1, 0)),
        ('undo', 39, 'rest', False, False, (0, 1)), ('redo', 52, 'rest', True, False, (1, 0)),
        ('dim', 78, 'dim', True, True, (3, 0)), ('bright', 91, 'bright', True, True, (3, 0)),
        ('reset', 104, 'rest', True, True, (3, 0))]]
RELOAD_STEPS = [dict(label=label, frame=frame, pose=state, deleted=True, retired=False, history=[0, 0])
                for label, frame, state in [('dim', 26, 'dim'), ('bright', 39, 'bright'), ('reset', 52, 'rest')]]


def generate(output):
    source = output/'sources/behavior.gltf'; write_fixture(source, behavior=True)
    posed = output/'sources/behavior-on-start.gltf'; write_fixture(posed, behavior=True, on_start=True)
    controls = {}
    for name in ('rest', 'dim', 'bright', 'dark'):
        controls[name] = output/'sources'/f'control-{name}.gltf'; write_fixture(controls[name], name)
    scripts = {}
    selection = dict(frame=1, cameraSelection=dict(nodeName=CAMERA))
    scripts['control'] = dict(schemaVersion=1, frames=13, sampleFrames=[13], events=[selection])
    scripts['source'] = dict(schemaVersion=1, frames=104, sampleFrames=[row['frame'] for row in SOURCE_STEPS], events=[selection,
        dict(frame=14, nodeAuthoring=dict(action='delete-subtree', sourceNode=0)), dict(frame=27, sceneHistory='undo'),
        dict(frame=40, sceneHistory='redo'), dict(frame=54, materialRetirement=dict(runtimeMaterial=2, sourceMaterial=0)),
        dict(frame=55, resourceRetirement=dict(domain='archive', kind='texture', row=0)),
        dict(frame=66, interactivityEvent='dim'), dict(frame=79, interactivityEvent='bright'), dict(frame=92, interactivityReset=True)])
    scripts['reload'] = dict(schemaVersion=1, frames=52, sampleFrames=[row['frame'] for row in RELOAD_STEPS], events=[selection,
        dict(frame=14, interactivityEvent='dim'), dict(frame=27, interactivityEvent='bright'), dict(frame=40, interactivityReset=True)])
    scripts['compact-source'] = copy.deepcopy(scripts['source']); scripts['compact-source']['sampleFrames'] = [13, 78, 91, 104]
    scripts['posed-source'] = copy.deepcopy(scripts['source']); scripts['posed-source'].update(frames=91, sampleFrames=[91])
    scripts['posed-source']['events'] = [event for event in scripts['posed-source']['events'] if event['frame'] <= 91]
    scripts['posed-reload'] = copy.deepcopy(scripts['reload']); scripts['posed-reload'].update(frames=39, sampleFrames=[26, 39])
    scripts['posed-reload']['events'] = [event for event in scripts['posed-reload']['events'] if event['frame'] <= 39]
    script_paths = {}
    for name, value in scripts.items():
        script_paths[name] = output/'scripts'/f'{name}.json'; document(script_paths[name], value)
    inputs = [source, posed, *controls.values(), *script_paths.values()]
    plan = dict(schemaVersion=1, source=str(source), posedSource=str(posed), controls={key: str(value) for key, value in controls.items()},
        scripts={key: str(value) for key, value in script_paths.items()}, sourceSteps=SOURCE_STEPS, reloadSteps=RELOAD_STEPS,
        expectedCaptureCount=48, immutableInputs=[dict(path=str(path), byteLength=path.stat().st_size, sha256=sha(path)) for path in inputs])
    document(output/'launch-plan.json', plan)
    return plan


def close(actual, expected, label, tolerance=4e-5):
    a, b = np.asarray(actual, dtype=float), np.asarray(expected, dtype=float)
    require(a.shape == b.shape and np.isfinite(a).all() and np.allclose(a, b, rtol=tolerance, atol=tolerance*.01), label)


def world(row):
    x, y, z, w = row['rotation']; rotation = np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
        [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])
    matrix = np.eye(4); matrix[:3, :3] = rotation@np.diag(row['scale']); matrix[:3, 3] = row['translation']
    return matrix


def projection(row):
    result = np.zeros((4, 4)); scale = 1/math.tan(row['yfov']/2)
    result[0, 0] = scale/(WIDTH/HEIGHT); result[1, 1] = scale; result[3, 2] = -1
    result[2, 2] = .1/(20.-.1); result[2, 3] = 20.*.1/(20.-.1)
    return result


def named(rows, name):
    matches = [row for row in rows if row.get('name') == name]
    require(len(matches) == 1, 'Missing/ambiguous retained object: '+name)
    return matches[0]


def graph_descendants(rows, owner):
    children = {}
    for row in rows: children.setdefault(row['parentGraphNode'], []).append(row)
    found = []; pending = [owner]; visited = set()
    while pending:
        row = pending.pop(); index = row['graphNode']
        require(index not in visited, 'Cyclic/ambiguous graph telemetry hierarchy')
        visited.add(index); found.append(row); pending.extend(children.get(index, []))
    return found


def check_state(state, step, *, behavior=True, reload=False, baseline=None):
    row = pose(step['pose']); merge = state['gltfMerge']
    require(not merge['authoringNodesTruncated'], 'Truncated behavior graph node telemetry')
    nodes = merge['authoringNodes']; surface = named(nodes, SURFACE); camera = named(nodes, CAMERA); light_node = named(nodes, LIGHT_NODE)
    for node in (surface, camera, light_node): require(node['active'] and not node['retired'], 'Behavior target was retired')
    close(np.asarray(surface['worldColumns']).T, world(row), 'Behavior node TRS did not reach the scene graph')
    if not reload:
        dead = named(nodes, DEAD); require(dead['retired'] == step['deleted'] and dead['active'] != step['deleted'], 'Unrelated node deletion/history mismatch')
    else: require(all(node.get('name') != DEAD for node in nodes), 'Portable graph retained deleted node')
    lens = state['gltfCameraAuthoring']; require(not lens['truncated'], 'Truncated behavior camera telemetry')
    require(lens['selectedSourceNode'] == camera['sourceNode'], 'Wrong authored camera selected')
    require(len(lens['evaluated']) == 1, 'Behavior lens definition lost')
    close(lens['evaluated'][0]['values'][:4], [row['yfov'], 0., .1, 20.], 'Behavior lens values wrong')
    close(lens['projection'], projection(row).T.reshape(-1), 'Behavior reverse-Z projection wrong')
    lights = state['lightEdits']; require(not lights['truncated'], 'Truncated behavior light telemetry')
    light_sources = {node['sourceNode'] for node in graph_descendants(nodes, light_node) if node['active'] and not node['retired']}
    imported = [light for light in lights['lights'] if light['source'] == 1 and light.get('sourceNode') in light_sources]
    require(len(imported) == 1 and imported[0]['type'] == 1, 'Behavior punctual light has no unique imported binding')
    close(imported[0]['position'], [0., 0., 2.], 'Behavior punctual light owner/attachment world pose wrong')
    close(imported[0]['intensity'], row['intensity'], 'Behavior punctual intensity wrong')
    close(imported[0]['color'], row['lightColor'], 'Behavior punctual color wrong')
    materials = state['gltfMaterialBindings']; require(not materials['materialsTruncated'], 'Truncated behavior material telemetry')
    material = named([dict(name=value['evaluated'].get('name'), row=value) for value in materials['materials']], MATERIAL)['row']
    close(material['evaluated']['pbrMetallicRoughness']['baseColorFactor'], row['color'], 'Behavior material color wrong')
    if behavior:
        value = state['gltfInteractivity']; require(value['available'] and not value['diagnostic'] and value['activeClips'] == 0, 'Behavior graph failed or was dropped')
        reset = step['label'] == 'reset'; require(value['playing'] != reset and value['started'] != reset, 'Behavior reset/play state wrong')
    history_cursor(state, *step['history'], allow_uncreated=True)
    require(state['gltfScenes']['editHistoryDepth'] == sum(step['history']), 'Behavior execution changed authoring history')
    _, resources = resource_catalog(state)
    if not reload:
        require(materials['sourceToRuntime'] == [2, 3], 'Fixture source/runtime material authority changed')
        require(materials['retiredRuntimeMaterials'] == ([2] if step['retired'] else []), 'Wrong unused material retirement state')
        for domain in ('archive', 'evaluated'):
            for kind in ('texture', 'image', 'sampler'):
                require(resources[(domain, kind, 0)]['active'] != step['retired'], 'Unused resource orphan closure phase wrong')
                require(resources[(domain, kind, 1)]['active'], 'Behavior-independent surviving resource retired')
    else:
        require(not materials['retiredRuntimeMaterials'], 'Portable graph reloaded a material tombstone')
        for domain in ('archive', 'evaluated'):
            for kind in ('texture', 'image', 'sampler'):
                matches = [resource for (d, k, _), resource in resources.items() if (d, k) == (domain, kind)]
                require(len(matches) == 1 and matches[0]['active'], 'Portable graph resource projection not dense/active')
    if baseline is not None:
        old_nodes = baseline['gltfMerge']['authoringNodes']
        for node in (surface, camera, light_node):
            old = named(old_nodes, node['name'])
            for key in ('graphNode', 'sourceNode', 'sourceMesh', 'sourcePrimitive', 'sourceInstance', 'meshOwnerGraphNode'):
                require(node[key] == old[key], 'Behavior/retirement replaced an unrelated target identity')
        _, old_resources = resource_catalog(baseline)
        for key, value in resources.items():
            require(key in old_resources and all(value[field] == old_resources[key][field] for field in ('incarnation', 'domainIdentity')),
                    'Behavior/retirement replaced an existing resource identity')
    return dict(passed=True, label=step['label'], pose=step['pose'], intensity=imported[0]['intensity'], color=imported[0]['color'],
                selectedSourceNode=lens['selectedSourceNode'], evidence='CPU graph/TRS/material/lens/light/history/retirement identities')


def pixels(record):
    value = np.asarray(Image.open(record['image']).convert('RGB'), dtype=np.int16)
    require(value.shape == (HEIGHT, WIDTH, 3), 'Wrong behavior capture extent'); return value


def same_image(actual, expected):
    delta = np.abs(pixels(actual)-pixels(expected)); maximum = int(delta.max(initial=0))
    require(maximum == 0, 'Behavior differs from independent literal image: '+str(actual['image'])+' maximumDelta='+str(maximum))
    return dict(passed=True, maximumDelta=maximum, evidence='Complete decoded native raster RGB against independent graph-free asset')


def export_hierarchy(model):
    nodes = model['nodes']; worlds = {}; parents = {}
    def visit(index, parent_world, parent=None):
        require(type(index) is int and 0 <= index < len(nodes) and index not in worlds,
                'Export hierarchy has an invalid, cyclic or multiply-owned active node')
        node = nodes[index]
        if 'matrix' in node:
            require(not any(key in node for key in ('translation', 'rotation', 'scale')), 'Export mixes matrix and TRS')
            matrix = np.asarray(node['matrix'], dtype=float)
            require(matrix.shape == (16,), 'Malformed exported local matrix'); matrix = matrix.reshape(4, 4).T
        else:
            matrix = world(dict(translation=node.get('translation', [0., 0., 0.]),
                                rotation=node.get('rotation', [0., 0., 0., 1.]), scale=node.get('scale', [1., 1., 1.])))
        require(np.isfinite(matrix).all(), 'Nonfinite exported hierarchy matrix')
        worlds[index] = parent_world@matrix; parents[index] = parent
        for child in node.get('children', []): visit(child, worlds[index], index)
    scene = model['scenes'][model.get('scene', 0)]
    for root in scene['nodes']: visit(root, np.eye(4))
    return worlds, parents


def unique_attachment(model, owner, worlds, predicate, label):
    descendants = []
    def walk(index):
        require(index in worlds, 'Exported behavior owner/attachment is outside the active scene')
        if predicate(model['nodes'][index]): descendants.append(index)
        for child in model['nodes'][index].get('children', []): walk(child)
    walk(owner)
    require(len(descendants) == 1, 'Exported behavior owner has no unique '+label+' descendant')
    return descendants[0]


def inspect_export(path, *, on_start=False, endpoint='rest'):
    model, _ = load_asset(path); source_graph = graph(on_start=on_start)
    require(model.get('extras') == OPAQUE, 'Export modified opaque root extras')
    require(DEAD not in [node.get('name') for node in model['nodes']], 'Export retained unrelated deleted node')
    surface = named(model['nodes'], SURFACE); camera = named(model['nodes'], CAMERA); light_node = named(model['nodes'], LIGHT_NODE)
    node_index = model['nodes'].index(surface); material_index = model['materials'].index(named(model['materials'], MATERIAL))
    require(node_index != 2 and material_index != 1, 'Fixture failed to force node/material graph index remaps')
    require('Unused original material' not in [value.get('name') for value in model['materials']], 'Export retained retired material')
    for table in ('textures', 'images', 'samplers'): require(len(model.get(table, [])) == 1, 'Export resource table not dense: '+table)
    texture = model['textures'][0]; require(texture['source'] == 0 and texture['sampler'] == 0, 'Export resource edges not projected')
    require(model['materials'][material_index]['pbrMetallicRoughness']['baseColorTexture']['index'] == 0, 'Export material texture not remapped')
    rest = pose(endpoint); worlds, parents = export_hierarchy(model)
    geometry_node = unique_attachment(model, node_index, worlds, lambda node: 'mesh' in node, 'geometry')
    require([index for index in worlds if 'mesh' in model['nodes'][index]] == [geometry_node], 'Export added/rebound an unrelated active mesh')
    close(worlds[node_index], world(rest), 'Export behavior parent TRS/hierarchy endpoint changed')
    mesh_index = model['nodes'][geometry_node]['mesh']
    require(type(mesh_index) is int and 0 <= mesh_index < len(model['meshes']), 'Export live geometry mesh index invalid')
    primitives = model['meshes'][mesh_index]['primitives']
    require(len(primitives) == 1 and primitives[0]['material'] == material_index and primitives[0].get('mode', 4) == 4,
            'Export live primitive material/topology edge wrong')
    primitive = primitives[0]; vertices = decode_accessor(path, primitive['attributes']['POSITION'])
    indices = decode_accessor(path, primitive['indices']).reshape(-1)
    require(vertices.ndim == 2 and vertices.shape[1] == 3 and indices.shape == (6,) and
            np.isfinite(vertices).all() and np.isfinite(indices).all() and np.equal(indices, np.floor(indices)).all() and
            (indices >= 0).all() and (indices < len(vertices)).all(), 'Export live geometry accessor shape/index invalid')
    actual = (worlds[geometry_node]@np.c_[vertices, np.ones(len(vertices))].T).T[:, :3]
    expected = (world(rest)@np.c_[POSITIONS, np.ones(len(POSITIONS))].T).T[:, :3]
    close(actual[indices.astype(int)], expected[INDICES], 'Export combined behavior-parent/geometry-child transform or mesh payload changed')
    camera_owner = model['nodes'].index(camera); light_owner = model['nodes'].index(light_node)
    camera_node = unique_attachment(model, camera_owner, worlds, lambda node: 'camera' in node, 'camera')
    light_attachment = unique_attachment(model, light_owner, worlds, lambda node: LIGHT in node.get('extensions', {}), 'punctual light')
    camera_world = np.eye(4); camera_world[:3, 3] = [0., 0., 4.]
    light_world = np.eye(4); light_world[:3, 3] = [0., 0., 2.]
    close(worlds[camera_node], camera_world, 'Export combined camera owner/attachment transform changed')
    close(worlds[light_attachment], light_world, 'Export combined light owner/attachment transform changed')
    camera_index = model['nodes'][camera_node]['camera']; light_index = model['nodes'][light_attachment]['extensions'][LIGHT]['light']
    require(model['cameras'][camera_index]['name'] == 'Behavior lens', 'Export camera edge wrong')
    require(model['extensions'][LIGHT]['lights'][light_index]['name'] == LIGHT_NAME, 'Export light edge wrong')
    remaps = {'/nodes/2': '/nodes/'+str(node_index), '/materials/1': '/materials/'+str(material_index),
              '/cameras/0': '/cameras/'+str(camera_index), f'/extensions/{LIGHT}/lights/1': f'/extensions/{LIGHT}/lights/{light_index}'}
    expected = copy.deepcopy(source_graph)
    def remap(value):
        for before, after in remaps.items():
            if value == before or value.startswith(before+'/'): return after+value[len(before):]
        raise RuntimeError('Unclassified normative graph reference: '+value)
    expected['variables'][0]['value'][0] = remap(expected['variables'][0]['value'][0])
    for event in expected['events']: event['values']['owner']['value'][0] = remap(event['values']['owner']['value'][0])
    for node in expected['nodes']:
        configuration = node.get('configuration', {})
        if 'pointer' in configuration: configuration['pointer']['value'][0] = remap(configuration['pointer']['value'][0])
        if 'nodeIndex' in configuration: configuration['nodeIndex']['value'][0] = node_index
    extension = model['extensions'][BEHAVIOR]
    require(extension == dict(graphs=[expected], graph=0, extras=dict(graphOwner='preserve extension extras')),
            'Export changed graph ops/flows/literals/extras or failed exact typed reference remapping')
    close(model['materials'][material_index]['pbrMetallicRoughness']['baseColorFactor'], rest['color'], 'Export material endpoint changed')
    close(model['cameras'][camera_index]['perspective']['yfov'], rest['yfov'], 'Export camera endpoint changed')
    light = model['extensions'][LIGHT]['lights'][light_index]
    close(light['intensity'], rest['intensity'], 'Export light intensity endpoint changed')
    close(light['color'], rest['lightColor'], 'Export light color endpoint changed')
    require(BEHAVIOR in model.get('extensionsRequired', []), 'Export lost required behavior contract')
    definitions = model['extensions'][LIGHT]['lights']
    return dict(passed=True, path=str(path), sha256=sha(path), node=node_index, material=material_index,
                geometryNode=geometry_node, geometryParent=parents[geometry_node], cameraAttachment=camera_node, lightAttachment=light_attachment,
                camera=camera_index, punctualLight=light_index, lightBindingRemapped=light_index != 1,
                originalLightDefinitionRetainedUnbound=len(definitions) > 1 and light_index != 1 and definitions[1].get('name') == LIGHT_NAME,
                boundLightIntensity=light['intensity'], boundLightColor=light['color'],
                evidence='Portable graph typed references resolve to named surviving objects; opaque graph preserved exactly')


def dependencies():
    directory = Path(__file__).resolve().parent
    return {str(Path(module.__file__).resolve()): sha(Path(module.__file__).resolve()) for module in tuple(sys.modules.values())
            if getattr(module, '__file__', None) and Path(module.__file__).resolve().parent == directory}


def run_native(args, output, plan, report):
    require(args.renderer and args.manifest and args.renderer.is_file() and args.manifest.is_file(), 'Native work requires renderer and frozen manifest')
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/*')) and not (output/'offline-sources').exists(), 'Native evidence directory must be fresh')
    frozen = {Path(name).resolve() for name in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
    runtime = sorted([*args.renderer.parent.glob('*.dll'), *(args.renderer.parent/'spv_shaders').glob('*.spv')])
    require(runtime and any(path.suffix == '.spv' for path in runtime), 'No resident native shader inventory')
    require({args.renderer, *map(Path, report['sourceDependencies']), *map(Path.resolve, runtime)} <= frozen, 'Manifest omits renderer/helper/DLL/SPIR-V inputs')
    manifest_hash, plan_hash = sha(args.manifest), sha(output/'launch-plan.json')
    settings = output/'vk_layer_settings.txt'; immutable(settings, b'khronos_validation.validate_sync = true\n')
    moved_sources = False
    def persist():
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    def verify():
        require(sha(args.manifest) == manifest_hash and sha(output/'launch-plan.json') == plan_hash, 'Behavior frozen manifest/plan changed')
        verify_manifest(args.manifest)
        require(dependencies() == report['sourceDependencies'], 'Behavior harness/helper source changed')
        require(settings.read_bytes() == b'khronos_validation.validate_sync = true\n', 'Validation settings changed')
        current = sorted([*args.renderer.parent.glob('*.dll'), *(args.renderer.parent/'spv_shaders').glob('*.spv')])
        require(current == runtime, 'Resident DLL/SPIR-V inventory changed')
        for entry in plan['immutableInputs']:
            path = Path(entry['path'])
            if moved_sources and path.parent == output/'sources': path = output/'offline-sources'/path.name
            require(path.is_file() and path.stat().st_size == entry['byteLength'] and sha(path) == entry['sha256'], 'Behavior fixture/script changed')
        for portable in report['portableCopies']:
            for entry in portable['files']: require(sha(Path(entry['path'])) == entry['sha256'], 'Portable behavior evidence changed')
    def capture(source, name, technique, script, *, save=None):
        verify(); target = output/'captures'; target.mkdir(exist_ok=True)
        # The common launcher sets validation-layer discovery to its capture
        # directory, so give it the exact same frozen settings there.
        immutable(target/'vk_layer_settings.txt', settings.read_bytes())
        try:
            records = run(args.renderer, target, source, name, technique, 'lit', report['captures'], sequence=script, save=save, timeout=args.timeout,
                extra=('--import-scale', '1', '--sky', 'plain', '--sky-horizon', '0', '0', '0', '--directional-intensity', '0',
                       '--no-ray-query', '--ray-shadows', 'raster'))
            require(not validation_log_error(Path(records[0]['log']).read_text(encoding='utf-8', errors='replace')), 'Behavior capture validation error')
            for record in records:
                state = record['state']
                require(state['resolution'] == [WIDTH, HEIGHT] and state['msaaSamples'] == 1 and not state['taa']['enabled'], 'Wrong behavior native capture settings')
                require(state['lighting']['environmentIntensity'] == state['lighting']['directionalIntensity'] == 0 and
                        any(row['active'] and row['gpuTimed'] for row in state['passes']), 'Behavior capture lacks isolated punctual lighting/GPU frame')
                for key in ('image', 'telemetry', 'log'): record[key+'Sha256'] = sha(Path(record[key]))
                saved_record = next(row for row in report['captures'] if row['telemetry'] == record['telemetry'])
                saved_record.update({key: value for key, value in record.items() if key != 'state'}, validationClean=True)
            require((target/'vk_layer_settings.txt').read_bytes() == settings.read_bytes(), 'Capture validation settings changed')
            return records
        finally: verify(); persist()
    def check(kind, technique, name, result):
        report['checks'].append(dict(kind=kind, technique=technique, name=name, **result)); persist()
    exports = []
    controls = {}
    try:
        report['nativeManifestBefore'] = verify_manifest(args.manifest); report['rendererExecuted'] = True; persist()
        for technique in args.technique:
            controls[technique] = {}
            for name, source in plan['controls'].items():
                record = capture(Path(source), technique+'-control-'+name, technique, Path(plan['scripts']['control']))[0]
                step = dict(label='control', pose=name, deleted=False, retired=False, history=[0, 0])
                check('state', technique, 'control-'+name, check_state(record['state'], step, behavior=False))
                controls[technique][name] = record
            bright, dark = pixels(controls[technique]['bright']), pixels(controls[technique]['dark'])
            delta = np.abs(bright-dark); fraction = float(np.any(delta > 5, axis=2).mean())
            require(fraction > .02 and float(delta.mean()) > 1., 'Punctual-light image oracle is absent/dark/vacuous')
            check('positive-image', technique, 'punctual-light-contribution', dict(passed=True, changedPixelFraction=fraction, meanAbsoluteDelta=float(delta.mean())))
            for suffix in ('gltf', 'glb'):
                name = technique+'-'+suffix; destination = output/'exports'/name/('scene.'+suffix); destination.parent.mkdir(parents=True)
                steps = SOURCE_STEPS if suffix == 'gltf' else [step for step in SOURCE_STEPS if step['label'] in ('rest', 'dim', 'bright', 'reset')]
                script = 'source' if suffix == 'gltf' else 'compact-source'
                records = capture(Path(plan['source']), name+'-source', technique, Path(plan['scripts'][script]), save=destination)
                require(len(records) == len(steps), 'Wrong source behavior capture inventory')
                for step, record in zip(steps, records, strict=True):
                    check('state', technique, name+'-'+step['label'], check_state(record['state'], step, baseline=records[0]['state']))
                    check('image', technique, name+'-'+step['label'], same_image(record, controls[technique][step['pose']]))
                check('serialized', technique, name, inspect_export(destination))
                copied, files = portable_copy(destination, output/'portable'/name)
                report['portableCopies'].append(dict(files=files)); exports.append((technique, name, copied, False)); persist()
            name = technique+'-posed-glb'; destination = output/'exports'/name/'scene.glb'; destination.parent.mkdir(parents=True)
            record = capture(Path(plan['posedSource']), name+'-source', technique, Path(plan['scripts']['posed-source']), save=destination)[0]
            check('state', technique, name, check_state(record['state'], SOURCE_STEPS[-2]))
            check('image', technique, name, same_image(record, controls[technique]['bright']))
            check('serialized', technique, name, inspect_export(destination, on_start=True, endpoint='bright'))
            copied, files = portable_copy(destination, output/'portable'/name)
            report['portableCopies'].append(dict(files=files)); exports.append((technique, name, copied, True)); persist()
        (output/'sources').rename(output/'offline-sources'); moved_sources = True; verify()
        require(not Path(plan['source']).exists(), 'Original graph fixture remains available for source-free reload')
        for technique, name, copied, posed in exports:
            steps = RELOAD_STEPS[:2] if posed else RELOAD_STEPS
            script = 'posed-reload' if posed else 'reload'
            records = capture(copied, name+'-source-free-reload', technique, Path(plan['scripts'][script]))
            require(len(records) == len(steps), 'Wrong portable behavior capture inventory')
            for step, record in zip(steps, records, strict=True):
                check('state', technique, name+'-reload-'+step['label'], check_state(record['state'], step, reload=True))
                check('image', technique, name+'-reload-'+step['label'], same_image(record, controls[technique][step['pose']]))
        require(len(report['captures']) == 24*len(args.technique), 'Unexpected total native behavior capture count')
        report['nativeValidationPerformed'] = True; report['passed'] = True
    finally:
        verify(); report['nativeManifestAfter'] = verify_manifest(args.manifest); persist()


def self_check():
    build = Path(__file__).resolve().parents[2]/'build'; build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='behavior-export-selfcheck-', dir=build) as temporary:
        output = Path(temporary); plan = generate(output)
        require(plan['expectedCaptureCount'] == 48 and len(SOURCE_STEPS) == 7 and len(RELOAD_STEPS) == 3, 'Behavior matrix grew unexpectedly')
        source, _ = load_asset(Path(plan['source'])); original = copy.deepcopy(source)
        for fixture in (plan['source'], plan['posedSource']):
            model, _ = load_asset(Path(fixture))
            for index, node in enumerate(model['extensions'][BEHAVIOR]['graphs'][0]['nodes']):
                require(all(index < edge['node'] < len(model['extensions'][BEHAVIOR]['graphs'][0]['nodes'])
                            for edge in node.get('flows', {}).values()), 'Fixture violates bounded forward-only flow profile')
        require(source['nodes'][2]['name'] == SURFACE and source['materials'][1]['name'] == MATERIAL, 'Fixture target authority wrong')
        require(source['meshes'][0]['primitives'][0]['material'] == 1 and source['textures'][1] == dict(source=1, sampler=1, name='Surviving texture'), 'Fixture fails dense projection preconditions')
        for name, script in plan['scripts'].items():
            value = json.loads(Path(script).read_text()); frames = [event['frame'] for event in value['events']]
            require(frames == sorted(set(frames)) and value['sampleFrames'] == sorted(set(value['sampleFrames'])), 'Invalid behavior sequence '+name)
        # Independent simulated serializer used only to test this inspection
        # oracle: drop leading node/resource/material holes, retain two engine
        # materials, then rewrite normative refs while leaving extras untouched.
        dense = copy.deepcopy(source); dense['nodes'] = dense['nodes'][2:]; dense['scenes'][0]['nodes'] = [0, 1, 2]
        mesh_index = dense['nodes'][0].pop('mesh'); dense['nodes'][0]['children'] = [3]
        dense['nodes'].append(dict(name='Primitive 0', mesh=mesh_index, matrix=np.eye(4).T.reshape(-1).tolist()))
        light_binding = dense['nodes'][2].pop('extensions'); dense['nodes'][2]['children'] = [4]
        dense['nodes'].append(dict(name='Imported Point 0', extensions=light_binding, matrix=np.eye(4).T.reshape(-1).tolist()))
        dense['materials'] = [dict(name='Default'), dict(name='Diagnostic'), dense['materials'][1]]
        dense['meshes'][0]['primitives'][0]['material'] = 2
        dense['materials'][2]['pbrMetallicRoughness']['baseColorTexture']['index'] = 0
        for table in ('textures', 'images', 'samplers'): dense[table] = dense[table][1:]
        dense['textures'][0].update(source=0, sampler=0)
        exported_graph = dense['extensions'][BEHAVIOR]['graphs'][0]
        exported_graph['variables'][0]['value'] = ['/materials/2']
        for event in exported_graph['events']: event['values']['owner']['value'] = ['/nodes/0']
        for node in exported_graph['nodes']:
            configuration = node.get('configuration', {})
            if 'pointer' in configuration:
                value = configuration['pointer']['value'][0]
                configuration['pointer']['value'][0] = value.replace('/nodes/2/', '/nodes/0/').replace('/materials/1/', '/materials/2/')
            if 'nodeIndex' in configuration: configuration['nodeIndex']['value'] = [0]
        target = output/'simulated.gltf'; target.write_text(json.dumps(dense)); inspect_export(target)
        # Check the same oracle with an independently packed binary container.
        encoded = json.dumps(dense, separators=(',', ':')).encode(); encoded += b' ' * (-len(encoded) % 4)
        glb = output/'simulated.glb'; glb.write_bytes(struct.pack('<III', 0x46546C67, 2, 20+len(encoded))+struct.pack('<II', len(encoded), 0x4E4F534A)+encoded)
        inspect_export(glb)
        rejected = []
        for name, mutate in [('stale-node', lambda value: value['extensions'][BEHAVIOR]['graphs'][0]['events'][0]['values']['owner'].__setitem__('value', ['/nodes/1'])),
            ('changed-flow', lambda value: value['extensions'][BEHAVIOR]['graphs'][0]['nodes'][0]['flows']['out'].__setitem__('node', 2)),
            ('changed-opaque', lambda value: value['extensions'][BEHAVIOR]['graphs'][0]['extras'].__setitem__('label', '/nodes/0')),
            ('stale-resource', lambda value: value['textures'][0].__setitem__('source', 1)),
            ('wrong-light', lambda value: value['extensions'][LIGHT]['lights'][1].__setitem__('intensity', 99.)),
            ('wrong-geometry-transform', lambda value: value['nodes'][3]['matrix'].__setitem__(12, 1.)),
            ('wrong-mesh-payload', lambda value: value['meshes'][0]['primitives'][0]['attributes'].__setitem__('POSITION', value['meshes'][0]['primitives'][0]['attributes']['NORMAL'])),
            ('wrong-mesh-material', lambda value: value['meshes'][0]['primitives'][0].__setitem__('material', 0)),
            ('orphan-geometry', lambda value: value['nodes'][0].__setitem__('children', [])),
            ('wrong-light-transform', lambda value: value['nodes'][4]['matrix'].__setitem__(12, 1.))]:
            wrong = copy.deepcopy(dense); mutate(wrong); target.write_text(json.dumps(wrong))
            try: inspect_export(target)
            except (RuntimeError, IndexError): rejected.append(name)
            else: raise RuntimeError('Behavior inspection oracle accepted '+name)
        posed = copy.deepcopy(dense); bright = pose('bright')
        posed_graph = posed['extensions'][BEHAVIOR]['graphs'][0]
        posed_graph['declarations'].append(dict(op='event/onStart'))
        posed_graph['nodes'].append(dict(declaration=3, flows=dict(out=dict(node=len(posed_graph['nodes'])+1)), extras=dict(start='bright authored behavior')))
        for index, node in enumerate(copy.deepcopy(posed_graph['nodes'][9:16])):
            if index < 6: node['flows'] = dict(out=dict(node=len(posed_graph['nodes'])+1))
            else: node.pop('flows', None)
            posed_graph['nodes'].append(node)
        posed['materials'][2]['pbrMetallicRoughness']['baseColorFactor'] = bright['color']
        for key in ('translation', 'rotation', 'scale'): posed['nodes'][0][key] = bright[key]
        posed['cameras'][0]['perspective']['yfov'] = bright['yfov']
        clone = copy.deepcopy(posed['extensions'][LIGHT]['lights'][1]); clone.update(intensity=bright['intensity'], color=bright['lightColor'])
        posed['extensions'][LIGHT]['lights'].append(clone); posed['nodes'][4]['extensions'][LIGHT]['light'] = 2
        for node in posed_graph['nodes']:
            pointer = node.get('configuration', {}).get('pointer')
            if pointer and pointer['value'][0].startswith(f'/extensions/{LIGHT}/lights/1/'):
                pointer['value'][0] = pointer['value'][0].replace('/lights/1/', '/lights/2/')
        target.write_text(json.dumps(posed)); result = inspect_export(target, on_start=True, endpoint='bright')
        require(result['lightBindingRemapped'] and result['originalLightDefinitionRetainedUnbound'], 'Posed inspection lost the original-vs-bound definition distinction')
        stale = next(node['configuration']['pointer'] for node in posed_graph['nodes'] if node.get('configuration', {}).get('pointer', {}).get('value') == [f'/extensions/{LIGHT}/lights/2/intensity'])
        stale['value'] = [f'/extensions/{LIGHT}/lights/1/intensity']; target.write_text(json.dumps(posed))
        try: inspect_export(target, on_start=True, endpoint='bright')
        except RuntimeError: rejected.append('stale-light-bound-clone')
        else: raise RuntimeError('Behavior inspection oracle accepted a stale unbound light definition')
        def synthetic(step):
            row = pose(step['pose']); nodes = []
            for source, name in ((0, DEAD), (2, SURFACE), (3, CAMERA), (4, LIGHT_NODE)):
                matrix = world(row) if name == SURFACE else np.eye(4)
                deleted = source == 0 and step['deleted']
                nodes.append(dict(name=name, graphNode=source+1, sourceNode=source, sourceMesh=0 if source == 2 else -1,
                    parentGraphNode=-1,
                    sourcePrimitive=-1, sourceInstance=-1, meshOwnerGraphNode=source+1, active=not deleted, retired=deleted,
                    worldColumns=matrix.T.tolist()))
            resources = [dict(domain=domain, kind=kind, row=index, active=not (index == 0 and step['retired']),
                              incarnation=1+index, domainIdentity=1)
                         for domain in ('archive', 'evaluated') for kind in ('texture', 'image', 'sampler') for index in (0, 1)]
            reset = step['label'] == 'reset'
            return dict(gltfMerge=dict(authoringNodesTruncated=False, authoringNodes=nodes),
                gltfCameraAuthoring=dict(truncated=False, selectedSourceNode=3, evaluated=[dict(values=[row['yfov'], 0., .1, 20., 0., 0., 0., 0.])],
                                        projection=projection(row).T.reshape(-1).tolist()),
                lightEdits=dict(truncated=False, lights=[dict(source=1, sourceNode=4, type=1, position=[0., 0., 2.], intensity=row['intensity'], color=row['lightColor'])]),
                gltfMaterialBindings=dict(materialsTruncated=False, sourceToRuntime=[2, 3], retiredRuntimeMaterials=[2] if step['retired'] else [],
                    materials=[dict(evaluated=dict(name=MATERIAL, pbrMetallicRoughness=dict(baseColorFactor=row['color'])))]),
                gltfInteractivity=dict(available=True, diagnostic='', activeClips=0, playing=not reset, started=not reset),
                gltfMaterialVariants=dict(undoCount=step['history'][0], redoCount=step['history'][1]),
                gltfScenes=dict(editHistoryDepth=sum(step['history'])),
                gltfResourceCatalog=dict(truncated=False, rows=resources, logicalGenerationValid=True, resourceRevision=1,
                    activeRows=sum(resource['active'] for resource in resources), retiredRows=sum(not resource['active'] for resource in resources),
                    runtimeSlotCount=0, activeRuntimeCount=0, actualImages=[], coveredActualImageCount=0))
        baseline = synthetic(SOURCE_STEPS[0])
        for step in SOURCE_STEPS: check_state(synthetic(step), step, baseline=baseline)
        reloaded = synthetic(RELOAD_STEPS[0]); reloaded['gltfMerge']['authoringNodes'].pop(0)
        light_owner = named(reloaded['gltfMerge']['authoringNodes'], LIGHT_NODE)
        attachment = copy.deepcopy(light_owner); attachment.update(name='Imported Point 0', graphNode=6, sourceNode=5,
                                                                    parentGraphNode=light_owner['graphNode'])
        reloaded['gltfMerge']['authoringNodes'].append(attachment); reloaded['lightEdits']['lights'][0]['sourceNode'] = 5
        resources = reloaded['gltfResourceCatalog']; resources['rows'] = [row for row in resources['rows'] if row['row'] == 0]
        resources.update(activeRows=6, retiredRows=0)
        check_state(reloaded, RELOAD_STEPS[0], reload=True)
        state_rejections = []
        for name, mutate in [('node-trs', lambda value: value['gltfMerge']['authoringNodes'][1]['worldColumns'][3].__setitem__(0, 99.)),
            ('camera-matrix', lambda value: value['gltfCameraAuthoring']['projection'].__setitem__(0, 0.)),
            ('light-color', lambda value: value['lightEdits']['lights'][0]['color'].__setitem__(0, .1)),
            ('material-color', lambda value: value['gltfMaterialBindings']['materials'][0]['evaluated']['pbrMetallicRoughness']['baseColorFactor'].__setitem__(0, .1)),
            ('resource-phase', lambda value: value['gltfResourceCatalog']['rows'][0].__setitem__('active', True)),
            ('history', lambda value: value['gltfMaterialVariants'].__setitem__('undoCount', 0)),
            ('graph-diagnostic', lambda value: value['gltfInteractivity'].__setitem__('diagnostic', 'failure'))]:
            wrong = synthetic(SOURCE_STEPS[-2]); mutate(wrong)
            try: check_state(wrong, SOURCE_STEPS[-2], baseline=baseline)
            except RuntimeError: state_rejections.append(name)
            else: raise RuntimeError('Behavior state oracle accepted '+name)
        require(source == original, 'Self-check modified authored fixture graph')
        require(np.linalg.det(projection(pose('bright'))) != 0 and not np.allclose(world(pose('dim')), world(pose('bright'))), 'Analytical oracles vacuous')
        quaternion = np.asarray(pose('bright')['rotation'], dtype=np.float32)
        norm = np.float32(np.sqrt(np.dot(quaternion, quaternion)))
        normalized_double = (quaternion.astype(float)/np.linalg.norm(quaternion.astype(float))).astype(np.float32)
        require(norm == 1. and all(np.array_equal(quaternion, value) for value in
                (normalized_double, quaternion/norm, quaternion*np.float32(1./norm))),
                'Literal/graph quaternion fixture is unstable across valid normalization paths')
        image = np.full((HEIGHT, WIDTH, 3), [80, 90, 100], dtype=np.uint8)
        reference = output/'image-reference.png'; Image.fromarray(image).save(reference)
        require(same_image(dict(image=str(reference)), dict(image=str(reference)))['maximumDelta'] == 0, 'Exact image oracle rejected equality')
        image_rejections = []
        for name, changed in [('one-lsb', (150, 157, 1)), ('larger-error', None)]:
            wrong = image.copy()
            if changed: wrong[changed] += 1
            else: wrong[10:15, 20:25] += 8
            candidate = output/(name+'.png'); Image.fromarray(wrong).save(candidate)
            try: same_image(dict(image=str(candidate)), dict(image=str(reference)))
            except RuntimeError: image_rejections.append(name)
            else: raise RuntimeError('Behavior image oracle accepted '+name)
        return dict(passed=True, generatedFixtures=6, simulatedFormats=['gltf', 'glb'], rejectedCorruptions=rejected, rejectedTelemetry=state_rejections,
                    rejectedImageErrors=image_rejections, normalizationStable=True, nativeValidationPerformed=False, expectedCaptureCount=48)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path); parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output', type=Path); parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--self-check', action='store_true'); parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--technique', action='append', choices=TECHNIQUES)
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2)); return
    require(args.output is not None, '--output is required')
    output = args.output.resolve(); build = Path(__file__).resolve().parents[2]/'build'
    require(output.is_relative_to(build), 'Behavior artifacts must stay under workspace build/')
    output.mkdir(parents=True, exist_ok=True); args.technique = args.technique or list(TECHNIQUES)
    if args.renderer: args.renderer = args.renderer.resolve()
    if args.manifest: args.manifest = args.manifest.resolve()
    report = dict(schemaVersion=1, passed=False, generatedOnly=args.generate_only, rendererExecuted=False,
        nativeValidationPerformed=False, sourceDependencies=dependencies(), captures=[], checks=[], portableCopies=[],
        limitations=['CPU identity/lens/light metadata is separate from GPU readback.',
                     'Retained onSelect references are structurally validated; native picking is not exercised.'])
    try:
        plan = generate(output); report['launchPlanSha256'] = sha(output/'launch-plan.json')
        if args.generate_only: report['passed'] = True
        else: run_native(args, output, plan, report)
    except BaseException:
        report['error'] = traceback.format_exc(); raise
    finally:
        report.update(captureCount=len(report['captures']), checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    print('PASS behavior export '+('generation only' if args.generate_only else 'native raster/retirement/portable replay'), flush=True)


if __name__ == '__main__': main()
