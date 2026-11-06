"""Independent #106 dynamic merge follow-up: 17 captures per raster technique.

Reuses proven fixture writers and independent deformation arithmetic; baked
controls never call the production merger or consume an exported model.
--generate-only/--self-check are pure CPU. Native runs require an explicit
renderer and frozen manifest, checked before every process and after the run.
Per-light manual edits and loop changes have no capture action and are not
claimed. CLI directional overrides and existing authored-camera selection are
checked through actual camera/light telemetry.
"""
from __future__ import annotations

import argparse
import base64
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
from PIL import Image

import gltf_deformation_runtime_regression as deformation
import gltf_instancing_runtime_regression as instancing
from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, require
from gltf_extended_materials_runtime_regression import sha, validation_log_error
from gltf_merge_runtime_regression import document, forest_worlds, placement, portable_copy
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest

TECHNIQUES = ('deferred-raster', 'forward-raster')
UNLIT, VARIANTS, PUNCTUAL, POINTER = ('KHR_materials_unlit', 'KHR_materials_variants',
                                    'KHR_lights_punctual', 'KHR_animation_pointer')
CAMERA_NODE, A_NODE_COUNT = 3, 5
A_OFFSET, B_PLACEMENT = [-1.6, 0., 0.], [1.5, 0., 0.]
COLORS = dict(A=[.8, .2, .08, 1.], Avariant=[.06, .65, .95, 1.],
              B=[.15, .7, .35, 1.], Bvariant=[.85, .06, .35, 1.])
# (A sampled time, B sampled time, merged, selected global variant). None is
# authored rest, deliberately different from A's camera/light animation start.
CONTROLS = dict(before=(.5, None, False, 0), merged=(.5, None, True, 0),
                variant=(.5, None, True, 1), a0=(0., None, True, 0),
                a1=(1., None, True, 0), incoming=(None, 1., True, 0),
                wrong_autoplay=(.5, 1., True, 0))
STATES = [('before', 13, 'before', 0, 0, 0), ('merge', 26, 'merged', 1, 0, 1),
          ('undo', 39, 'before', 0, 1, 2), ('redo', 52, 'merged', 1, 0, 3),
          ('variant', 65, 'variant', 2, 0, 3), ('variant-undo', 78, 'merged', 1, 1, 3)]
RELOADS = [('held', 0, .5, 'merged'), ('a0', 0, 0., 'a0'),
           ('a1', 0, 1., 'a1'), ('incoming', 1, 1., 'incoming')]
UNVERIFIED = ['No per-light manual edit or animation loop-change capture action exists.',
              'Speed zero checks playing-state preservation without wall-clock pose drift; advancing playback is not exercised.',
              'Camera/light values are telemetry checks; unlit pixel comparisons do not prove physical light transport.',
              'No ray-query/DDGI, physics, GPU picking, allocation-failure injection or leak-free claim is made.',
              'No native result is claimed by generation or pure self-check.']


def a_spec(time):
    spec = deformation.spec_for('combined')
    spec['nodes'][0]['name'] = 'A morph skin owner'
    spec['nodes'][1]['name'] = 'A moving joint'
    spec['nodes'].append(dict(name='A retained group', translation=A_OFFSET, children=[0, 1]))
    if time is None:
        spec['tracks'] = []
    return spec


def scalar_mix(first, last, time):
    return float(np.float32(np.float32(first)*(1-time)+np.float32(last)*time))


def camera_fov(time):
    return .65 if time is None else scalar_mix(.7, .9, time)


def a_intensity(time):
    return 7. if time is None else scalar_mix(20., 40., time)


def install_variants(model, origin):
    base = model['materials'][0]
    base.update(name=origin+' default', extensions={UNLIT: {}},
                extras=dict(fixtureOrigin=origin, authoredResource=True))
    base['pbrMetallicRoughness']['baseColorFactor'] = COLORS[origin]
    alternate = copy.deepcopy(base)
    alternate['name'] = origin+' variant'
    alternate['pbrMetallicRoughness']['baseColorFactor'] = COLORS[origin+'variant']
    model['materials'].append(alternate)
    model.setdefault('extensions', {})[VARIANTS] = dict(variants=[dict(name=origin+' color')])
    model['meshes'][0]['primitives'][0].setdefault('extensions', {})[VARIANTS] = dict(
        mappings=[dict(material=1, variants=[0])])
    model.setdefault('extensionsUsed', []).extend([UNLIT, VARIANTS])


def append_camera_and_light(model, origin):
    camera_node, light_node = len(model['nodes']), len(model['nodes'])+1
    model['nodes'].extend([
        dict(name=origin+' authored camera', camera=0, translation=[0., 0., 6. if origin == 'A' else 4.]),
        dict(name=origin+' authored lamp', translation=[-1., 2., 3.] if origin == 'A' else [0., 1., 2.],
             extensions={PUNCTUAL: dict(light=0)})])
    model['scenes'][0]['nodes'].extend([camera_node, light_node])
    model['cameras'] = [dict(name=origin+' camera resource', type='perspective',
        perspective=dict(yfov=.65 if origin == 'A' else .55, znear=.1, zfar=100.))]
    model.setdefault('extensions', {})[PUNCTUAL] = dict(lights=[dict(name=origin+' lamp resource',
        type='point', color=[1., .75, .5] if origin == 'A' else [.4, .6, 1.],
        intensity=7. if origin == 'A' else 13., range=10. if origin == 'A' else 8.)])
    model.setdefault('extensionsUsed', []).append(PUNCTUAL)
    return camera_node


def source(path, origin):
    # Existing writers operate in a temporary directory. Final immutable input
    # files are written only once and never silently replace different bytes.
    with tempfile.TemporaryDirectory(prefix='merge-dynamic-writer-') as temporary:
        temporary_path = Path(temporary)/'source.gltf'
        if origin == 'A':
            deformation.write_fixture(temporary_path, a_spec(.5))
        else:
            instancing.fixture(temporary_path, 'morph', 'source', animated=True)
        model = json.loads(temporary_path.read_text(encoding='utf-8'))
    if origin == 'B':
        for node in model['nodes']:
            node['name'] = 'B '+node['name']
        # Keep exactly the four extension instances, with an empty authored
        # child retained as a hierarchy/provenance witness.
        model['nodes'][2].pop('mesh')
    install_variants(model, origin)
    camera_node = append_camera_and_light(model, origin)
    require(camera_node == CAMERA_NODE, 'Fixture source camera index changed')
    if origin == 'A':
        model['animations'][0]['name'] = 'A combined motion and pointers'
        asset = Asset(); asset.model = model
        asset.data = bytearray(base64.b64decode(model['buffers'][0]['uri'].split(',', 1)[1]))
        clip = model['animations'][0]
        times = clip['samplers'][0]['input']
        for pointer, values in [('/extensions/'+PUNCTUAL+'/lights/0/intensity', [20., 40.]),
                                ('/cameras/0/perspective/yfov', [.7, .9])]:
            clip['channels'].append(dict(sampler=len(clip['samplers']),
                target=dict(path='pointer', extensions={POINTER: dict(pointer=pointer)})))
            clip['samplers'].append(dict(input=times, output=asset.accessor('SCALAR', values), interpolation='LINEAR'))
        model['extensionsUsed'].append(POINTER)
        model['buffers'] = [dict(byteLength=len(asset.data), uri='data:application/octet-stream;base64,'+
                               base64.b64encode(asset.data).decode())]
    else:
        model['animations'][0]['name'] = 'B incoming owner and parent motion'
    model['extensionsUsed'] = sorted(set(model['extensionsUsed']))
    model['asset']['generator'] = 'Container independent dynamic merge source '+origin
    document(path, model)


def expected_entries(a_time, b_time, merged):
    entries = deformation.evaluate(a_spec(a_time), 0. if a_time is None else a_time)
    entries[0]['origin'] = 'A'
    if not merged:
        return entries
    positions = np.asarray([[-.65, -.6, 0.], [.65, -.6, 0.], [0., .7, 0.]], dtype=np.float64)
    delta = np.asarray([[.2, .08, 0.], [.25, .08, 0.], [.12, .15, 0.]], dtype=np.float64)
    positions = deformation.f32(positions)+.4*deformation.f32(delta)
    t = 0. if b_time is None else b_time
    parent = deformation.transform(scale=deformation.f32(np.array([.9, 1.2, 1.])*(1-t)+np.array([1.05, .9, 1.])*t))
    owner = deformation.transform(translation=deformation.f32(np.array([.05, .02, 0.])*(1-t)+np.array([.12, .08, 0.])*t))
    placed = deformation.transform(translation=B_PLACEMENT)
    for index, (translation, quaternion, scale) in enumerate(zip(
            instancing.TRANSLATIONS, instancing.ROTATIONS, instancing.SCALES)):
        instance = deformation.local_matrix(dict(translation=deformation.f32(translation).tolist(),
            rotation=deformation.f32(quaternion).tolist(), scale=deformation.f32(scale).tolist()))
        model = placed@parent@owner@instance
        entries.append(dict(origin='B', instance=index, positions=positions,
            normals=np.tile([0., 0., 1.], (3, 1)), tangents=np.tile([1., 0., 0., 1.], (3, 1)),
            weights=[float(np.float32(.4))], model=model,
            worldPositions=(np.column_stack((positions, np.ones(3)))@model.T)[:, :3],
            mesh=dict(indices=np.array([0, 1, 2])), deformed=True))
    return entries


def baked(path, control):
    a_time, b_time, merged, variant = CONTROLS[control]
    asset = Asset(); asset.model['nodes'] = []
    for origin in ('A', 'B'):
        color = COLORS[origin+'variant'] if (variant == 0 and origin == 'A') or (variant == 1 and origin == 'B') else COLORS[origin]
        asset.model['materials'].append(dict(name=origin+' baked', extensions={UNLIT: {}},
            pbrMetallicRoughness=dict(baseColorFactor=color, metallicFactor=0., roughnessFactor=.6)))
    entries = expected_entries(a_time, b_time, merged)
    for index, entry in enumerate(entries):
        world = entry['worldPositions']
        indices = entry['mesh']['indices'].reshape(-1, 3).copy()
        if np.linalg.det(entry['model'][:3, :3]) < 0:
            indices = indices[:, [0, 2, 1]]
        primitive = dict(attributes=dict(POSITION=asset.accessor('VEC3', world),
            NORMAL=asset.accessor('VEC3', np.tile([0., 0., 1.], (len(world), 1)))),
            indices=asset.accessor('SCALAR', indices.reshape(-1), 5123, indices=True),
            material=0 if entry['origin'] == 'A' else 1, mode=4)
        asset.model['meshes'].append(dict(name='Independent baked '+str(index), primitives=[primitive]))
        asset.model['nodes'].append(dict(name='Independent baked '+str(index), mesh=index))
    camera = len(asset.model['nodes'])
    asset.model['nodes'].append(dict(name='Independent fixed camera', camera=0, translation=[0., 0., 6.]))
    asset.model['cameras'] = [dict(type='perspective', perspective=dict(yfov=camera_fov(a_time), znear=.1, zfar=100.))]
    asset.model['scenes'] = [dict(nodes=list(range(len(asset.model['nodes']))))]
    asset.model['extensionsUsed'] = [UNLIT]
    asset.model['asset']['generator'] = 'Independent world-vertex control '+control
    asset.model['buffers'] = [dict(byteLength=len(asset.data), uri='data:application/octet-stream;base64,'+
                                 base64.b64encode(asset.data).decode())]
    document(path, asset.model)
    return camera


def generate(output):
    paths = dict(A=output/'sources'/'A.gltf', B=output/'sources'/'B.gltf')
    for origin, path in paths.items(): source(path, origin)
    oracles = {}
    for control in CONTROLS:
        path = output/'oracles'/(control+'.gltf')
        oracles[control] = dict(path=str(path), camera=baked(path, control))
    sequence = dict(schemaVersion=1, frames=78, sampleFrames=[row[1] for row in STATES], events=[
        dict(frame=14, merge=dict(path=str(paths['B']), parentName='Dynamic B placement',
             placementMatrix=placement(B_PLACEMENT)), guiWaitForLoad=True),
        dict(frame=27, sceneHistory='undo'), dict(frame=40, sceneHistory='redo'),
        dict(frame=53, materialVariant=1), dict(frame=66, sceneHistory='undo')])
    document(output/'sequence.json', sequence)
    from gltf_merge_runtime_regression import immutable
    immutable(output/'vk_layer_settings.txt', b'khronos_validation.validate_sync = true\n')
    files = sorted([*paths.values(), *(Path(row['path']) for row in oracles.values()),
                    output/'sequence.json', output/'vk_layer_settings.txt'])
    return dict(sources={key:str(path) for key,path in paths.items()}, oracles=oracles,
        sequence=str(output/'sequence.json'), capturesPerTechnique=17, processesPerTechnique=12,
        immutableInputs=[dict(path=str(path), sha256=sha(path)) for path in files], unverified=UNVERIFIED)


def close(actual, expected, label, tolerance=3e-4):
    a, b = np.asarray(actual, dtype=float), np.asarray(expected, dtype=float)
    require(a.shape == b.shape and np.isfinite(a).all() and np.isfinite(b).all() and
            np.max(np.abs(a-b), initial=0.) <= tolerance, 'Incorrect dynamic merge '+label)


def check_deformation(state, entries, merged, *, singleton_reload=False, a_source_node=0):
    actual = state['gltfDeformation']
    require(actual['active'] and actual['gpuReady'] and actual['currentBoundToGpu'] and actual['previousBoundToGpu'],
            'Actual deformation streams are not active and bound')
    streams = 1+(4 if singleton_reload else 1)*int(merged)
    require(actual['instanceCount'] == streams and len(actual['instances']) == len(entries),
            'Missing/extra deformation streams or instance draw rows')
    source_indices = state['scene']['primaryIndexCount']
    require(actual['sourceVertexCount'] == 24+3*int(merged) and source_indices == 36+3*int(merged),
            'Source definitions were aliased or duplicated per placement')
    extra = (15 if singleton_reload else 6)*int(merged)
    require(actual['expandedVertexCount'] == 48+extra and actual['expandedIndexCount'] == 72+extra,
            'Incorrect owner-shared deformation layout')
    unmatched = list(actual['instances']); ranges = {}; b_ranges = []
    for entry in entries:
        matches = [row for row in unmatched if np.max(np.abs(np.asarray(row['modelColumns']).T-entry['model'])) < 3e-4]
        require(len(matches) == 1, 'Missing/aliased per-instance world binding')
        row = matches[0]; unmatched.remove(row)
        if entry['origin'] == 'A': require(row['sourceNodeIndex'] == a_source_node, 'Retained A source identity changed')
        for field, expected in [('firstPosition', entry['positions'][0]), ('firstNormal', entry['normals'][0]),
                                ('firstTangent', entry['tangents'][0]), ('weights', entry['weights']),
                                ('boundsMin', entry['positions'].min(axis=0)), ('boundsMax', entry['positions'].max(axis=0))]:
            close(row[field], expected, field)
        require(row['boundsValid'] and row['vertexCount'] == len(entry['positions']) and
                row['indexCount'] == len(entry['mesh']['indices']), 'Incorrect instance range/bounds')
        require(row['vertexOffset'] >= actual['sourceVertexCount'] and row['firstIndex'] >= source_indices,
                'Deformed output aliases a source definition')
        value = (row['vertexOffset'], row['vertexOffset']+row['vertexCount'])
        if entry['origin'] == 'B': b_ranges.append((value,row['firstIndex']))
        ranges.setdefault(entry['origin'],set()).add(value)
    require(len(ranges['A']) == 1 and (not merged or
            len(ranges['B']) == len(set(b_ranges)) == (4 if singleton_reload else 1)),
            'Incorrect source-owner deformation stream sharing')
    unique_ranges = sorted(value for values in ranges.values() for value in values)
    require(all(a[1] <= b[0] for a,b in zip(unique_ranges,unique_ranges[1:])), 'A/B owners alias GPU vertex ranges')
    require(actual['maxPositionDelta'] < 1e-5 and not actual['historyReset'], 'Held pose did not settle deformation history')


def check_state(record, control, *, clip, time, playing, camera_node, selected_variant, baseline=None, schedule=None,
                singleton_reload=False, a_source_node=0):
    state = record['state']; a_time, b_time, merged, _ = CONTROLS[control]
    animation = state['gltfAnimation']
    require(animation['selectedClip'] == clip and abs(animation['time']-time) < 1e-6 and
            animation['playing'] is playing and animation['speed'] == 0. and animation['loop'] is True,
            'Selected clip/time/playback controls changed')
    require(animation['clipCount'] == 1+int(merged) and animation['diagnostics'] == 0,
            'Old/incoming animation identities or tracks were lost')
    require(animation['selectedCameraNode'] == camera_node and not animation['orthographic'], 'Authored camera selection changed')
    close(state['camera']['position'], [0.,0.,6.], 'effective camera position')
    fov = camera_fov(a_time); projection = np.zeros((4,4)); s = 1/math.tan(fov/2)
    projection[0,0], projection[1,1] = s/(640/360), s
    projection[2,2], projection[2,3], projection[3,2] = .1/99.9, 10/99.9, -1.
    close(np.asarray(animation['cameraProjection']).reshape(4,4).T, projection, 'reverse-Z authored projection', 3e-5)
    close([animation['cameraNear'], animation['cameraFar']], [.1,100.], 'camera clip planes')
    require(state['gltfMaterialVariants']['selected'] == selected_variant and
            state['gltfMaterialVariants']['count'] == 1+int(merged), 'Variant catalog/selection changed')
    expected_lights = [([-1.,2.,3.], [1.,.75,.5], a_intensity(a_time), 10.)]
    if merged: expected_lights.append(([1.5,1.,2.], [.4,.6,1.], 13., 8.))
    lights = animation['pointLights']; require(len(lights) == len(expected_lights), 'Missing/extra authored lights')
    for position, color, intensity, radius in expected_lights:
        matches = [row for row in lights if np.linalg.norm(np.asarray(row['position'])-position) < 1e-4]
        require(len(matches) == 1, 'Authored light binding/placement changed')
        close(matches[0]['color'], color, 'authored light color')
        close([matches[0]['intensity'], matches[0]['range']], [intensity,radius], 'authored light intensity/range')
    close([state['lighting']['directionalIntensity']], [.37], 'CLI directional override')
    close(state['lighting']['directionalColor'], [.2,.7,.4], 'CLI directional color override')
    require(state['lighting']['environmentIntensity'] == 0 and state['scene']['primaryImportScale'] == 1.,
            'Environment override/session scale changed')
    entries = expected_entries(a_time,b_time,merged)
    check_deformation(state,entries,merged,singleton_reload=singleton_reload,a_source_node=a_source_node)
    if schedule is not None:
        _, _, _, undo, redo, publications = schedule
        history, merge = state['gltfMaterialVariants'], state['gltfMerge']
        require((history['undoCount'],history['redoCount']) == (undo,redo), 'Merge/variant history cursor changed')
        require(merge['commitGeneration']-baseline['gltfMerge']['commitGeneration'] == publications and
                merge['generationPublications']-baseline['gltfMerge']['generationPublications'] == publications and
                merge['descriptorGeneration'] == merge['commitGeneration'], 'Dynamic generation publication count changed')
        reads = 0 if schedule[0] == 'before' else 1
        require(merge['sourceReadCount']-baseline['gltfMerge']['sourceReadCount'] == reads,
                'Undo/redo reopened the incoming source')
        require(state['gltfScenes']['graphIdentity'] == baseline['gltfScenes']['graphIdentity'] and
                state['gui']['selectedMeshNodeId'] == baseline['gui']['selectedMeshNodeId'],
                'Retained graph or selection identity changed')
        require(not state['gui']['modelLoading']['pending'] and merge['outcome'] == ('idle' if reads == 0 else 'committed'),
                'Merge request did not settle successfully')
    return dict(passed=True, control=control, deformationStreams=1+(4 if singleton_reload else 1)*int(merged),
                deformedDrawBindings=len(entries), clip=clip, time=time,
                playing=playing, selectedCameraNode=camera_node, authoredLights=len(lights))


def compare_pixels(record, oracle, control):
    return deformation.check_pixels(record,oracle,expected_entries(*CONTROLS[control][:3]))


def contrast(first, second, label, *, roi=None):
    a, b = (np.asarray(Image.open(row['image']).convert('RGB'),dtype=np.float64) for row in (first,second))
    if roi: a,b = a[roi[1]:roi[3],roi[0]:roi[2]],b[roi[1]:roi[3],roi[0]:roi[2]]
    delta = np.abs(a-b); mean = float(delta.mean()); changed = float((delta.max(axis=2)>8).mean())
    require(mean >= .1 and changed >= .002, 'Vacuous independent dynamic control: '+label)
    return dict(name=label, passed=True, meanAbsoluteByteDelta=mean, changedPixelFraction=changed)


def uint_instance_ids(path,index):
    # The shared FLOAT fixture decoder intentionally narrows to binary32. UINT
    # identity metadata above 2**24 must instead be inspected without conversion.
    model,buffers=load_asset(path);accessor=model['accessors'][index]
    require(accessor['componentType'] == 5125 and accessor['type'] == 'SCALAR' and
            not accessor.get('normalized',False) and 'sparse' not in accessor and 0 < accessor['count'] <= 4,
            'Unexpected exact UINT instance identity representation')
    view=model['bufferViews'][accessor['bufferView']];data=buffers[view['buffer']]
    start=view.get('byteOffset',0)+accessor.get('byteOffset',0);stride=view.get('byteStride',4)
    end=start+(accessor['count']-1)*stride+4
    require(stride >= 4 and view.get('byteOffset',0) <= start and
            end <= view.get('byteOffset',0)+view['byteLength'] and end <= len(data),
            'Invalid exact UINT instance identity range')
    return [struct.unpack_from('<I',data,start+i*stride)[0] for i in range(accessor['count'])]


def mesh_descendants(model,group):
    # forest_worlds validates strict bounded ancestry before this traversal.
    pending=list(model['nodes'][group].get('children',[]));result=[]
    for index in pending:
        node=model['nodes'][index]
        if 'mesh' in node:result.append(index)
        pending.extend(node.get('children',[]))
    return result


def channel_key(model,channel):
    target=channel['target'];path=target['path']
    if path != 'pointer':
        node=target['node']
        require(type(node) is int and 0 <= node < len(model['nodes']) and path in ('translation','scale','weights'),
                'Exported animation node target is invalid')
        if path == 'weights':require('mesh' in model['nodes'][node],'Morph restore targets a meshless source group')
        return ('node',node,path)
    pointer=target['extensions'][POINTER]['pointer'];tokens=pointer.split('/')
    if len(tokens) == 5 and tokens[1] == 'cameras' and tokens[3:] == ['perspective','yfov']:
        require(tokens[2].isdigit(),'Invalid animation camera pointer index');index=int(tokens[2])
        require(index < len(model['cameras']),'Animation camera pointer is out of range')
        return ('camera',index,'yfov')
    if len(tokens) == 6 and tokens[1:4] == ['extensions',PUNCTUAL,'lights'] and tokens[5] == 'intensity':
        require(tokens[4].isdigit(),'Invalid animation light pointer index');index=int(tokens[4])
        require(index < len(model['extensions'][PUNCTUAL]['lights']),'Animation light pointer is out of range')
        return ('light',index,'intensity')
    raise RuntimeError('Unexpected exported dynamic animation target: '+pointer)


def channel_sampler(model,clip,channel):
    index=channel['sampler'];require(type(index) is int and 0 <= index < len(clip['samplers']),'Invalid animation sampler')
    sampler=clip['samplers'][index]
    require(all(type(sampler[key]) is int and 0 <= sampler[key] < len(model['accessors']) for key in ('input','output')),
            'Invalid animation accessor binding')
    return sampler


def encoded_float_accessor(path,index):
    model,buffers=load_asset(path);accessor=model['accessors'][index]
    require(accessor['componentType'] == 5126 and not accessor.get('normalized',False) and
            'sparse' not in accessor and 0 < accessor['count'] <= 64,'Unexpected animation FLOAT accessor')
    width=dict(SCALAR=1,VEC2=2,VEC3=3,VEC4=4)[accessor['type']];size=4*width
    view=model['bufferViews'][accessor['bufferView']];data=buffers[view['buffer']]
    start=view.get('byteOffset',0)+accessor.get('byteOffset',0);stride=view.get('byteStride',size)
    end=start+(accessor['count']-1)*stride+size
    require(stride >= size and start >= view.get('byteOffset',0) and end <= len(data) and
            end <= view.get('byteOffset',0)+view['byteLength'],'Invalid animation accessor encoded range')
    return (accessor['type'],accessor['count'],b''.join(data[start+i*stride:start+i*stride+size] for i in range(accessor['count'])))


def check_restore(path,model,clip,channel,key,values):
    require(channel_key(model,channel) == key,'Authored-rest channel targets a different property/binding')
    sampler=channel_sampler(model,clip,channel)
    for field in ('input','output'):encoded_float_accessor(path,sampler[field])
    require(sampler.get('interpolation','LINEAR') == 'STEP' and
            np.array_equal(decode_accessor(path,sampler['input']).reshape(-1),np.asarray([0.],dtype=np.float32)),
            'Authored-rest channel is not a one-key STEP at zero')
    actual=decode_accessor(path,sampler['output']).reshape(-1)
    require(np.array_equal(actual,np.asarray(values,dtype=np.float32).reshape(-1)),
            'Authored-rest STEP values differ from exact source FLOAT values')


def inspect_animation_export(path,model,source_a,source_b,a_child,joint,camera,a_lights):
    a,_=load_asset(source_a);b,_=load_asset(source_b)
    require(len(a['animations']) == len(b['animations']) == 1,'Source animation fixture catalog changed')
    original_a,original_b=a['animations'][0],b['animations'][0]
    exported_a,exported_b=model['animations']
    require(exported_a['name'] == original_a['name'] and exported_b['name'] == original_b['name'],
            'Export changed append-only clip identities')
    a_mapping={('node',1,'translation'):('node',joint,'translation'),('node',0,'weights'):('node',a_child,'weights'),
               ('camera',0,'yfov'):('camera',camera,'yfov'),('light',0,'intensity'):('light',0,'intensity')}
    source_a_channels={channel_key(a,c):c for c in original_a['channels']}
    require(set(source_a_channels) == set(a_mapping),'Original A fixture track coverage changed')
    a_tracks={a_mapping[key]:(source_a,original_a,c,True) for key,c in source_a_channels.items()}
    require(0 in a_lights,'Original A light source identity changed')
    for light in a_lights:
        a_tracks[('light',light,'intensity')]=(source_a,original_a,source_a_channels[('light',0,'intensity')],True)
    b_tracks={};a_rest={};b_rest={}
    for origin,key in [(('node',1,'translation'),'B Instanced owner'),(('node',0,'scale'),'B Nested parent')]:
        source_channel=next(c for c in original_b['channels'] if channel_key(b,c) == origin)
        matches=[i for i,node in enumerate(model['nodes']) if node.get('name') == key]
        require(len(matches) == 2,'Canonical/placed incoming animated source groups were collapsed')
        for node in matches:
            mapped=('node',node,origin[2]);b_tracks[mapped]=(source_b,original_b,source_channel,False)
            b_rest[mapped]=b['nodes'][origin[1]][origin[2]]
    a_rest[('node',joint,'translation')]=a['nodes'][1]['translation']
    a_rest[('node',a_child,'weights')]=a['nodes'][0]['weights']
    a_rest[('camera',camera,'yfov')]=[a['cameras'][0]['perspective']['yfov']]
    for light in a_lights:a_rest[('light',light,'intensity')]=[a['extensions'][PUNCTUAL]['lights'][0]['intensity']]
    for clip,tracks,rest in [(exported_a,a_tracks,b_rest),(exported_b,b_tracks,a_rest)]:
        channels={}
        for channel in clip['channels']:
            key=channel_key(model,channel);require(key not in channels,'Duplicate exported animation property target')
            channel_sampler(model,clip,channel);channels[key]=channel
        require(set(channels) == set(tracks)|set(rest),'Export omitted or added an unexpected animation binding')
        for key,(source_path,source_clip,source_channel,stable_indices) in tracks.items():
            original=channel_sampler(load_asset(source_path)[0],source_clip,source_channel)
            actual=channel_sampler(model,clip,channels[key])
            require(actual.get('interpolation','LINEAR') == original.get('interpolation','LINEAR'),
                    'Export changed an original animation interpolation mode')
            for field in ('input','output'):
                require(encoded_float_accessor(path,actual[field]) == encoded_float_accessor(source_path,original[field]),
                        'Export changed original animation accessor metadata or encoded FLOAT bytes')
                if stable_indices:require(actual[field] == original[field],'Retained A animation accessor identity changed')
        for key,values in rest.items():check_restore(path,model,clip,channels[key],key,values)
    # File defaults intentionally describe the saved t=.5 pose. Rest remains
    # encoded in the incoming clip's independent STEP restore channels above.
    def midpoint(source_key):
        c=source_a_channels[source_key];sampler=channel_sampler(a,original_a,c)
        values=decode_accessor(source_a,sampler['output']).astype(np.float64).reshape(2,-1)
        return (values[0]+values[1])*.5
    require(np.array_equal(model['nodes'][a_child]['weights'],midpoint(('node',0,'weights'))) and
            np.array_equal(model['nodes'][joint]['translation'],midpoint(('node',1,'translation'))),
            'Export file node defaults do not describe the independent saved pose')
    require(model['cameras'][camera]['perspective']['yfov'] == midpoint(('camera',0,'yfov'))[0],
            'Export file camera default does not describe the independent saved pose')
    for light in a_lights:
        require(model['extensions'][PUNCTUAL]['lights'][light]['intensity'] == midpoint(('light',0,'intensity'))[0],
                'Export file light default does not describe the independent saved pose')
    return dict(savedPoseDefaults=True,authoredRestStepBindings=len(a_rest),incomingRestStepBindings=len(b_rest),
                originalAnimationAccessorBytesPreserved=True,retainedAAccessorIndicesPreserved=True)


def inspect_export(path,source_a,source_b):
    model,_ = load_asset(path); worlds,_ = forest_worlds(model)
    require(len(model['scenes']) == 2 and len(model['skins']) == 1 and len(model['animations']) == 2,
            'Dynamic export lost scenes/skin/animation catalog')
    for i in range(len(model['accessors'])):
        require(np.isfinite(decode_accessor(path,i)).all(), 'Nonresident/invalid exported dynamic accessor')
    owners = [i for i,node in enumerate(model['nodes']) if node.get('name') == 'A morph skin owner']
    require(len(owners) == 1, 'Export lost/duplicated A authored source group')
    require('mesh' not in model['nodes'][owners[0]] and 'skin' not in model['nodes'][owners[0]],
            'Export retained a duplicate mesh/skin on A source group')
    children=mesh_descendants(model,owners[0]);require(len(children) == 1, 'Export lost/duplicated A primitive child')
    a_child=children[0];a_node=model['nodes'][a_child]
    require('skin' in a_node and 0 <= a_node['skin'] < len(model['skins']), 'A primitive child lost its skin')
    primitive=model['meshes'][a_node['mesh']]['primitives'][0]
    require(len(primitive.get('targets',[])) == 2 and 'JOINTS_0' in primitive['attributes'] and
            'WEIGHTS_0' in primitive['attributes'], 'A primitive lost morph/skin source streams')
    joint=next(i for i,node in enumerate(model['nodes']) if node.get('name') == 'A moving joint')
    require(model['skins'][a_node['skin']]['joints'] == [joint], 'Export changed A skin joint identity')
    cameras=[i for i,row in enumerate(model['cameras']) if row.get('name') == 'A camera resource']
    require(len(cameras) == 1,'Export lost/duplicated A camera resource identity');camera=cameras[0]
    lights = model['extensions'][PUNCTUAL]['lights']
    a_lights=[i for i,row in enumerate(lights) if row.get('name') == 'A lamp resource']
    require(a_lights,'Export lost A lamp resource identity')
    lamp=next(i for i,node in enumerate(model['nodes']) if node.get('name') == 'A authored lamp')
    pending=[lamp];bound=[]
    for node in pending:
        definition=model['nodes'][node]
        if PUNCTUAL in definition.get('extensions',{}):bound.append(definition['extensions'][PUNCTUAL]['light'])
        pending.extend(definition.get('children',[]))
    require(len(bound) == 1 and bound[0] in a_lights,'Active A lamp alias is absent or bound to a different source light')
    b_owners = [i for i,node in enumerate(model['nodes']) if node.get('name') == 'B Instanced owner']
    require(len(b_owners) == 2, 'Canonical and placed instancing owners were collapsed')
    expected_ids=[16777217,16777219,16777221,16777223]
    for owner in b_owners:
        group=model['nodes'][owner]
        require('mesh' not in group and 'EXT_mesh_gpu_instancing' not in group.get('extensions',{}),
                'Export left a packed instancing extension on its meshless source group')
        children=mesh_descendants(model,owner);require(len(children) == 4, 'Export lost an instance primitive descendant')
        observed=[]
        for child in children:
            node=model['nodes'][child];extension=node['extensions']['EXT_mesh_gpu_instancing'];attrs=extension['attributes']
            ids=uint_instance_ids(path,attrs['_ID'])
            require(len(ids) == 1 and ids[0] in expected_ids, 'Export changed exact UINT singleton identity')
            index=expected_ids.index(ids[0]);observed.append(ids[0])
            require(all(model['accessors'][accessor]['count'] == 1 for accessor in attrs.values()),
                    'Primitive descendant retained a packed instance accessor')
            require(extension.get('extras') == dict(retain='instance-source'), 'Export lost opaque instance metadata')
            values={}
            for semantic,table in [('TRANSLATION',instancing.TRANSLATIONS),('ROTATION',instancing.ROTATIONS),('SCALE',instancing.SCALES)]:
                values[semantic]=decode_accessor(path,attrs[semantic]).reshape(-1)
                close(values[semantic],table[index],'exported singleton '+semantic,1e-6)
            local=deformation.local_matrix(dict(translation=values['TRANSLATION'],rotation=values['ROTATION'],scale=values['SCALE']))
            expected=deformation.local_matrix(dict(translation=instancing.TRANSLATIONS[index],rotation=instancing.ROTATIONS[index],
                                                   scale=instancing.SCALES[index]))
            close(worlds[child]@local,worlds[owner]@expected,'exported singleton transform order',1e-5)
        require(sorted(observed) == expected_ids, 'Export collapsed or duplicated an instance identity within an owner')
    animation_checks=inspect_animation_export(path,model,source_a,source_b,a_child,joint,camera,a_lights)
    require([row['name'] for row in model['extensions'][VARIANTS]['variants']] == ['A color','B color'],
            'Variant source identities changed in export')
    return dict(path=str(path), sha256=sha(path), passed=True, authoredRestRetainedByRestoreChannels=True,aSourceNode=a_child,
                completeCatalog=True, distinctInstancingOwners=True,singletonsPerOwner=4,allAccessorsDecoded=True,
                animation=animation_checks)


def dependencies():
    directory = Path(__file__).resolve().parent
    paths = {Path(__file__).resolve()}
    for module in tuple(sys.modules.values()):
        value = getattr(module,'__file__',None)
        if value and Path(value).resolve().parent == directory: paths.add(Path(value).resolve())
    return {str(path):sha(path) for path in sorted(paths)}


def launch(args, output, name, technique, source_path, report, persist, verify, *, camera_node,
           sequence=None, save=None, clip=None, time=0., variant=None):
    verify()
    target = output/'captures'/(technique+'-'+name)
    target.parent.mkdir(parents=True,exist_ok=True)
    require(not any(target.parent.glob(target.name+'*')), 'Refusing to overwrite native evidence: '+str(target))
    image,log = target.with_suffix('.png'),target.with_suffix('.log')
    command = [str(args.renderer.resolve()),'--hidden','--no-ui','--validation','--width','640','--height','360',
        '--msaa','1','--no-taa','--no-bloom','--fixed-dt','.016666667','--warmup-frames','12','--capture-frame','13',
        '--render-technique',technique,'--display-mode','lit','--exposure','.35','--import-scale','1',
        '--environment-intensity','0','--directional-intensity','.37','--directional-color','.2','.7','.4',
        '--camera-position','0','0','6','--camera-target','0','0','0','--camera-fov','45',
        '--no-ray-query','--ray-shadows','raster','--gltf-camera-node',str(camera_node),
        '--model',str(source_path),'--screenshot',str(image)]
    if clip is not None: command += ['--animation-clip',str(clip),'--animation-time',str(time),'--animation-speed','0']
    if variant is not None: command += ['--gltf-variant',str(variant)]
    if sequence: command += ['--capture-sequence',str(sequence)]
    if save:
        save.parent.mkdir(parents=True,exist_ok=True); require(not save.exists(),'Refusing to overwrite native export')
        command += ['--save-scene',str(save)]
    process_record = dict(name=name,technique=technique,command=command,log=str(log),completed=False)
    report['processes'].append(process_record);report['rendererExecuted']=True;persist()
    with log.open('x',encoding='utf-8') as stream:
        process = subprocess.run(command,cwd=args.renderer.resolve().parent,stdout=stream,stderr=subprocess.STDOUT,
            timeout=args.timeout,env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(output)),
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    process_record.update(returnCode=process.returncode,completed=True,logSha256=sha(log));persist()
    require(process.returncode == 0,'Native dynamic merge failed: '+str(log))
    require(not validation_log_error(log.read_text(encoding='utf-8',errors='replace')),'Vulkan validation failed: '+str(log))
    frames = [(row[0],row[1]) for row in STATES] if sequence else [(name,13)]
    records = {}
    for label,frame in frames:
        sampled = image if frame == frames[-1][1] else image.with_name(image.stem+f'.frame-{frame:04d}.png')
        telemetry = sampled.with_suffix('.telemetry.json')
        require(sampled.is_file() and telemetry.is_file(),'Missing fresh dynamic capture: '+str(sampled))
        state = json.loads(telemetry.read_text(encoding='utf-8-sig'))
        require(finite_json(state) and state['technique'] == technique and state['resolution'] == [640,360] and
                state['msaaSamples'] == 1 and not state['taa']['enabled'],'Invalid native dynamic capture settings')
        require(any(row['active'] and row['gpuTimed'] for row in state['passes']),'No actual GPU-timed pass')
        record = dict(name=label,technique=technique,frame=frame,image=str(sampled),telemetry=str(telemetry),
                      log=str(log),imageSha256=sha(sampled),telemetrySha256=sha(telemetry),validationClean=True)
        report['captures'].append(record);records[label]={**record,'state':state}
    persist();verify();return records


def run_native(args,output,plan,report):
    require(args.renderer.is_file() and args.manifest.is_file(),'Missing explicit renderer/frozen manifest')
    renderer_hash, manifest_hash = sha(args.renderer), sha(args.manifest)
    report['rendererSha256'] = renderer_hash
    def persist():
        report.update(captureCount=len(report['captures']),checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    def verify():
        require(sha(args.renderer) == renderer_hash,'Renderer changed during dynamic acceptance')
        require(sha(args.manifest) == manifest_hash,'Frozen native manifest changed during dynamic acceptance')
        verify_manifest(args.manifest)
        for row in plan['immutableInputs']: require(sha(Path(row['path'])) == row['sha256'],'Generated native input changed')
        require(dependencies() == report['sourceDependencies'],'Dynamic harness dependency changed')
    report['nativeManifestBefore'] = verify_manifest(args.manifest)
    manifest = json.loads(args.manifest.read_text(encoding='utf-8-sig'))
    require(any(Path(path).resolve() == args.renderer.resolve() for path in manifest['files']),
            'Frozen manifest does not include the selected renderer')
    try:
        verify()
        for technique in args.technique:
            oracles = {}
            for name,row in plan['oracles'].items():
                oracles[name] = launch(args,output,'oracle-'+name,technique,Path(row['path']),report,persist,verify,
                    camera_node=row['camera'])['oracle-'+name]
            saved = output/'exports'/technique/'dynamic-merged.glb'
            records = launch(args,output,'transaction',technique,Path(plan['sources']['A']),report,persist,verify,
                camera_node=CAMERA_NODE,sequence=Path(plan['sequence']),save=saved,clip=0,time=.5,variant=0)
            baseline = records['before']['state']
            for schedule in STATES:
                name,_,control,*_ = schedule
                report['checks'].append(dict(name=technique+'-'+name,**check_state(records[name],control,
                    clip=0,time=.5,playing=True,camera_node=CAMERA_NODE,selected_variant=CONTROLS[control][3],
                    baseline=baseline,schedule=schedule)))
                report['checks'].append(dict(name=technique+'-'+name+'-baked',passed=True,
                    **compare_pixels(records[name],oracles[control],control)))
            for first,second,label,roi in [('before','merged','incoming-visible',(350,60,620,300)),
                    ('merged','wrong_autoplay','incoming-motion-negative-control',(350,60,620,300)),
                    ('merged','variant','variant-negative-control',None),('a0','a1','old-motion-negative-control',(80,60,320,300))]:
                report['checks'].append(contrast(oracles[first],oracles[second],technique+'-'+label,roi=roi))
            require(saved.is_file(),'Dynamic transaction did not save a GLB')
            export_inspection=inspect_export(saved,Path(plan['sources']['A']),Path(plan['sources']['B']))
            report['exports'].append(export_inspection)
            copied,files = portable_copy(saved,output/'portable'/technique)
            report['portableCopies'].append(dict(path=str(copied),files=files))
            copied_model,_ = load_asset(copied)
            camera_node = next(i for i,node in enumerate(copied_model['nodes']) if node.get('name') == 'A authored camera')
            for name,clip,time,control in RELOADS:
                record = launch(args,output,'reload-'+name,technique,copied,report,persist,verify,
                    camera_node=camera_node,clip=clip,time=time,variant=0)['reload-'+name]
                report['checks'].append(dict(name=technique+'-reload-'+name,
                    **check_state(record,control,clip=clip,time=time,playing=True,camera_node=camera_node,selected_variant=0,
                                  singleton_reload=True,a_source_node=export_inspection['aSourceNode'])))
                report['checks'].append(dict(name=technique+'-reload-'+name+'-baked',passed=True,
                    **compare_pixels(record,oracles[control],control)))
            persist()
        require(len(report['captures']) == 17*len(args.technique) and len(report['checks']) == 24*len(args.technique) and
                len(report['exports']) == len(args.technique) and len(report['processes']) == 12*len(args.technique),
                'Native dynamic cohort completeness count changed')
        report['passed']=True;report['nativeValidationPerformed']=True
    except Exception as error:
        report['failure']=str(error);raise
    finally:
        # Failed captures still need an after-run provenance audit. Preserve the
        # original failure while recording a second, independent freeze error.
        try:
            verify();report['nativeManifestAfter'] = verify_manifest(args.manifest)
            report['afterFreezeVerified']=True
        except Exception as audit_error:
            report['afterFreezeVerified']=False;report['freezeAuditFailure']=str(audit_error)
            if 'failure' not in report:
                report['passed']=False;report['failure']='After-run freeze audit failed: '+str(audit_error)
                persist();raise
        persist()


def self_check():
    with tempfile.TemporaryDirectory(prefix='merge-dynamic-self-check-') as temporary:
        output=Path(temporary);first=generate(output);second=generate(output)
        require(first == second,'Dynamic fixture generation is not deterministic')
        a,_=load_asset(Path(first['sources']['A']));b,_=load_asset(Path(first['sources']['B']))
        require(len(a['nodes']) == A_NODE_COUNT and len(b['nodes']) == 5 and len(a['animations'][0]['channels']) == 4,
                'Dynamic source topology or pointer coverage changed')
        ids=b['nodes'][1]['extensions']['EXT_mesh_gpu_instancing']['attributes']['_ID']
        require(uint_instance_ids(Path(first['sources']['B']),ids) == [16777217,16777219,16777221,16777223] and
                not np.array_equal(decode_accessor(Path(first['sources']['B']),ids).reshape(-1),
                                   [16777217,16777219,16777221,16777223]),
                'Exact UINT metadata control is vacuous or was narrowed to FLOAT')
        require(camera_fov(None) != camera_fov(.5) and a_intensity(None) != a_intensity(.5),
                'Authored-vs-evaluated pointer-rest controls are vacuous')
        a0,a1=expected_entries(0.,None,False),expected_entries(1.,None,False)
        require(np.max(np.abs(a0[0]['worldPositions']-a1[0]['worldPositions'])) > .2,'Old motion control is vacuous')
        rest,moving=expected_entries(.5,None,True),expected_entries(.5,1.,True)
        require(max(np.max(np.abs(a['worldPositions']-b['worldPositions'])) for a,b in zip(rest[1:],moving[1:])) > .05,
                'Incoming autoplay control is vacuous')
        for name,control in CONTROLS.items():
            model,_=load_asset(Path(first['oracles'][name]['path']))
            entries=expected_entries(*control[:3]);require(len(model['meshes']) == len(entries),'Baked oracle has wrong instance count')
            for mesh,entry in zip(model['meshes'],entries):
                close(decode_accessor(Path(first['oracles'][name]['path']),mesh['primitives'][0]['attributes']['POSITION']),
                      entry['worldPositions'],'independent baked vertices',1e-6)
        entries=rest
        rows=[]
        for index,entry in enumerate(entries):
            rows.append(dict(modelColumns=entry['model'].T.tolist(),sourceNodeIndex=0 if index == 0 else 12,
                firstPosition=entry['positions'][0].tolist(),firstNormal=entry['normals'][0].tolist(),
                firstTangent=entry['tangents'][0].tolist(),weights=entry['weights'],
                boundsMin=entry['positions'].min(axis=0).tolist(),boundsMax=entry['positions'].max(axis=0).tolist(),boundsValid=True,
                vertexOffset=27 if index == 0 else 51,vertexCount=len(entry['positions']),
                firstIndex=39 if index == 0 else 75,indexCount=len(entry['mesh']['indices'])))
        state=dict(scene=dict(primaryIndexCount=39),gltfDeformation=dict(active=True,gpuReady=True,currentBoundToGpu=True,previousBoundToGpu=True,
            instanceCount=2,instances=rows,sourceVertexCount=27,
            expandedVertexCount=54,expandedIndexCount=78,maxPositionDelta=0.,historyReset=False))
        check_deformation(state,entries,True);rejected=[]
        for name,mutate in [('changed-position',lambda s:s['gltfDeformation']['instances'][1]['firstPosition'].__setitem__(0,9.)),
                            ('missing-instance',lambda s:s['gltfDeformation']['instances'].pop()),
                            ('aliased-range',lambda s:s['gltfDeformation']['instances'][1].__setitem__('vertexOffset',27)),
                            ('wrong-source-layout',lambda s:s['gltfDeformation'].__setitem__('sourceVertexCount',24))]:
            changed=copy.deepcopy(state);mutate(changed)
            try:check_deformation(changed,entries,True)
            except RuntimeError:rejected.append(name)
            else:raise RuntimeError('Dynamic verifier accepted changed synthetic evidence: '+name)
        reload_state=copy.deepcopy(state);actual=reload_state['gltfDeformation']
        actual.update(instanceCount=5,expandedVertexCount=63,expandedIndexCount=87)
        actual['instances'][0]['sourceNodeIndex']=17
        for index,row in enumerate(actual['instances'][1:]):
            row.update(sourceNodeIndex=20+index,vertexOffset=51+3*index,firstIndex=75+3*index)
        check_deformation(reload_state,entries,True,singleton_reload=True,a_source_node=17)
        for name,mutate in [('collapsed-reload-streams',lambda s:s['gltfDeformation'].__setitem__('instanceCount',2)),
                            ('aliased-reload-output',lambda s:s['gltfDeformation']['instances'][-1].__setitem__('vertexOffset',51)),
                            ('changed-reload-source-id',lambda s:s['gltfDeformation']['instances'][0].__setitem__('sourceNodeIndex',0))]:
            changed=copy.deepcopy(reload_state);mutate(changed)
            try:check_deformation(changed,entries,True,singleton_reload=True,a_source_node=17)
            except RuntimeError:rejected.append(name)
            else:raise RuntimeError('Dynamic verifier accepted changed synthetic reload evidence: '+name)
        def restore_fixture(label,values=(.3,.2),target=0,interpolation='STEP',time=0.):
            asset=Asset();asset.model['nodes']=[dict(mesh=0),dict(mesh=0)]
            asset.model['meshes']=[dict(primitives=[dict(attributes={},targets=[{},{}])])]
            input_accessor=asset.accessor('SCALAR',[time]);output_accessor=asset.accessor('SCALAR',values)
            clip=dict(samplers=[dict(input=input_accessor,output=output_accessor,interpolation=interpolation)],
                      channels=[dict(sampler=0,target=dict(node=target,path='weights'))])
            asset.model['animations']=[clip];path=output/('restore-'+label+'.gltf');asset.write(path)
            model,_=load_asset(path);return path,model,model['animations'][0],model['animations'][0]['channels'][0]
        check_restore(*restore_fixture('correct'),('node',0,'weights'),[.3,.2])
        for name,parameters in [('sampled-rest-values',dict(values=(.55,.4))),
                                ('one-ulp-rest-value',dict(values=(float(np.nextafter(np.float32(.3),np.float32(1.))),.2))),
                                ('wrong-restore-target',dict(target=1)),('out-of-range-restore-target',dict(target=2)),
                                ('linear-restore',dict(interpolation='LINEAR')),('nonzero-restore-time',dict(time=.5))]:
            fixture=restore_fixture(name,**parameters)
            try:check_restore(*fixture,('node',0,'weights'),[.3,.2])
            except RuntimeError:rejected.append(name)
            else:raise RuntimeError('Dynamic verifier accepted a changed authored-rest channel: '+name)
        require(validation_log_error('WRITE_AFTER_WRITE hazard detected') and not validation_log_error('No hazards detected'),
                'Strict validation scanner changed')
        return dict(pureSelfCheck=True,nativeValidationPerformed=False,deterministicInputs=True,
            capturesPerTechnique=17,independentBakedControls=7,syntheticMutationsRejected=rejected,
            originalAndIncomingMotionNonvacuous=True,authoredPointerRestDistinct=True,unverified=UNVERIFIED)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path);parser.add_argument('--renderer',type=Path);parser.add_argument('--manifest',type=Path)
    parser.add_argument('--technique',action='append',choices=TECHNIQUES)
    parser.add_argument('--timeout',type=float,default=300.)
    parser.add_argument('--generate-only',action='store_true');parser.add_argument('--self-check',action='store_true')
    args=parser.parse_args()
    if args.self_check:print(json.dumps(self_check(),indent=2));return
    if args.output is None:parser.error('--output is required')
    if not args.generate_only and (args.renderer is None or args.manifest is None):
        parser.error('Native runs require explicit --renderer and --manifest')
    if not math.isfinite(args.timeout) or args.timeout <= 0:parser.error('--timeout must be positive and finite')
    args.technique=tuple(dict.fromkeys(args.technique or ('deferred-raster',)))
    output=args.output.resolve();plan=generate(output);document(output/'launch-plan.json',plan)
    report=dict(schemaVersion=1,passed=None if args.generate_only else False,generatedOnly=args.generate_only,
        rendererExecuted=False,nativeValidationPerformed=False,sourceDependencies=dependencies(),
        immutableInputs=plan['immutableInputs'],expectedCaptureCount=17*len(args.technique),expectedCheckCount=24*len(args.technique),
        captures=[],checks=[],exports=[],processes=[],portableCopies=[],unverified=UNVERIFIED)
    if args.generate_only:
        document(output/'results.json',report)
        print('GENERATED',len(plan['immutableInputs']),'immutable inputs;',report['expectedCaptureCount'],'planned captures; no renderer execution')
    else:
        run_native(args,output,plan,report)
        print('PASS',report['captureCount'],'captures;',report['checkCount'],'dynamic/oracle checks')


if __name__ == '__main__':main()
