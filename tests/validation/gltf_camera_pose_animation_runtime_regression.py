"""Native selected-node animated camera default/history/save acceptance.

Generation and self-check never launch a renderer. Literal image controls and
analytical source/world checks are independent of the production pose planner.
"""
from __future__ import annotations
import argparse
import base64
import copy
import json
import math
from pathlib import Path
import re
import struct
import sys
import tempfile

import numpy as np

import gltf_camera_projection_runtime_regression as projection
import gltf_camera_view_pose_runtime_regression as pose
import gltf_material_binding_runtime_regression as binding
from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, require
from gltf_extended_materials_runtime_regression import sha
from gltf_merge_runtime_regression import document, immutable, portable_copy
from gltf_texture_formats_runtime_regression import verify_manifest
from gltf_tangent_uv_runtime_regression import finite_json
from scene_node_authoring_runtime_regression import history_cursor

TECHNIQUES = binding.TECHNIQUES
WIDTH, HEIGHT = 640, 360
POINTER, LIGHT = pose.POINTER, pose.LIGHT
NAV_A, NAV_B = pose.NAV_A, pose.NAV_B
NAMES = {1: 'Main camera', 2: 'Shared camera', 4: 'Ortho camera'}
FIELDS = ('matrix', 'translation', 'rotation', 'scale')
VALIDATION_SETTINGS = b'khronos_validation.validate_sync = true\nkhronos_validation.duplicate_message_limit = 1000\n'
READBACK_WARM_FRAMES = (3, 4)
PNG_READBACK_BYTES = WIDTH*HEIGHT*4
UNVERIFIED = [
    'Generation and checker self-tests are not native renderer acceptance.',
    'Source/rest/graph/light telemetry is CPU evidence; separate Vulkan tests cover GPU camera/object/light buffers.',
    'This cohort uses paused deterministic clocks; CPU/manager/UI tests cover playing, speed and loop preservation.',
    'Export explicitly clears playback after the final history capture, so file defaults are the edited rest pose; separate manager tests also cover saving active sampled defaults.',
    'Capture actions are not native ImGui interaction; separate widget tests exercise the real button.',
    'The bounded fixture does not establish all physics/provider/deformation or graphics parity.']


def close(a, b, label, tolerance=5e-5):
    pose.close(a, b, label, tolerance)


def parent_x(time): return .125 + .25*time


def nodes(main=False, shared=False, *, time=None, shear=False):
    main_t = [.25, 0., 6.] if main else [0., 0., 4.]
    shared_t = pose.f32(np.asarray(NAV_B['position'], np.float32)-np.array([parent_x(.75), 0, 0], np.float32)).tolist() if shared else [0., 0., 6.]
    result = [dict(name='Animated parent', children=[1, 2], translation=[0., 0., 0.]),
              dict(name=NAMES[1], camera=0, translation=main_t),
              dict(name=NAMES[2], camera=0, translation=shared_t, mesh=5, children=[3]),
              dict(name='Camera child light', translation=[.25, .15, -1.], extensions={LIGHT: dict(light=0)}),
              dict(name=NAMES[4], camera=1, translation=[0., 3., 6.]),
              *[dict(name='Surface '+str(i), mesh=i) for i in range(5)]]
    if shear:
        result[0]['scale'] = [2., 1., 1.]
        result[1]['rotation'] = projection.quat_z(35)
    if time is not None:
        if shear:
            result[1]['translation'] = [0., 0., 6.]
        else:
            result[0]['translation'] = [parent_x(time), 0., 0.]
            result[1].update(translation=[-parent_x(time), 0., 6.], rotation=[0., 0., 0., 1.], scale=[1., 1., 1.])
            result[2]['translation'][0] = -parent_x(time)
            result[2]['scale'] = [1., 1., 1.]
    for i, row in enumerate(result):
        row['extras'] = dict(nodeIdentity=2000+i, retained=dict(annotation='animated camera defaults', sourceLike=991+i))
    return result


def fixture(*, shear=False, literal=None):
    asset = Asset();model = asset.model
    model['nodes'] = nodes(shear=shear)
    model['cameras'] = [projection.camera_json(row) for row in projection.camera_records()]
    model['materials'] = [pose.material_record(i) for i in range(6)]
    model['meshes'] = []
    for i in range(6):
        positions = projection.positions(i) if i < 5 else [[1.4, -.7, -3.], [1.7, -.7, -3.], [1.7, -.4, -3.], [1.4, -.4, -3.]]
        attributes = dict(POSITION=asset.accessor('VEC3', positions), NORMAL=asset.accessor('VEC3', [[0., 0., 1.]]*4),
                          TANGENT=asset.accessor('VEC4', [[1., 0., 0., 1.]]*4), TEXCOORD_0=asset.accessor('VEC2', [[0., 0.], [1., 0.], [1., 1.], [0., 1.]]))
        model['meshes'].append(dict(name='Geometry '+str(i), extras=dict(geometryIdentity=i), primitives=[dict(
            attributes=attributes, indices=asset.accessor('SCALAR', projection.INDICES, 5123, indices=True), material=i, mode=4)]))
    model['scenes'] = [dict(name='Animated cameras', nodes=[0, 4, *range(5, 10)]), dict(name='Orthographic scene', nodes=[4, *range(5, 10)])]
    model['scene'] = 0;model['extensionsUsed'] = [projection.UNLIT, LIGHT]
    model['extensions'] = {LIGHT: dict(lights=[dict(name='Camera child point', type='point', color=[1., .7, .2], intensity=20., range=6., extras=dict(lightIdentity=31))])}
    times = asset.accessor('SCALAR', [0., 2.])
    def track(clip, target, kind, values, interpolation='LINEAR'):
        identity = len(clip['channels']);sampler = len(clip['samplers'])
        clip['samplers'].append(dict(input=times, output=asset.accessor(kind, values), interpolation=interpolation, extras=dict(samplerIdentity=100+identity)))
        clip['channels'].append(dict(sampler=sampler, target=target, extras=dict(channelIdentity=200+identity)))
    clip = dict(name='Selected pose curves', extras=dict(curveIdentity=51), samplers=[], channels=[])
    if shear:
        track(clip, dict(node=1, path='translation'), 'VEC3', [[0., 0., 6.], [0., 0., 6.]])
    else:
        track(clip, dict(node=0, path='translation'), 'VEC3', [[.125, 0., 0.], [.625, 0., 0.]])
        track(clip, dict(node=1, path='translation'), 'VEC3', [[-.125, 0., 6.], [-.625, 0., 6.]])
        track(clip, dict(node=1, path='rotation'), 'VEC4', [[0., 0., 0., 0.], [0., 0., 0., 1.], [0., 0., 0., 0.]]*2, 'CUBICSPLINE')
        track(clip, dict(node=1, path='scale'), 'VEC3', [[1., 1., 1.]]*2, 'STEP')
        for prop, kind, values, interpolation in [('translation/0', 'SCALAR', [-.125, -.625], 'LINEAR'),
                                                ('scale/2', 'SCALAR', [0., 1., 0., 0., 1., 0.], 'CUBICSPLINE')]:
            track(clip, dict(path='pointer', extensions={POINTER: dict(pointer='/nodes/2/'+prop)}), kind, values, interpolation)
        model['extensionsUsed'].append(POINTER)
    model['animations'] = [clip]
    if not shear:
        other = dict(name='Unrelated material clip', extras=dict(curveIdentity=52), samplers=[], channels=[])
        track(other, dict(path='pointer', extensions={POINTER: dict(pointer='/materials/0/pbrMetallicRoughness/roughnessFactor')}), 'SCALAR', [.2, .8])
        model['animations'].append(other)
    if literal is not None:
        model['animations'] = [];model['nodes'] = nodes()
        for key in FIELDS: model['nodes'][0].pop(key, None)
        model['nodes'][1].update(translation=[0., 0., 6.]);model['nodes'][2].update(translation=[0., 0., 6.])
        if literal in ('B', 'free-B'): model['nodes'][2]['translation'] = [0., *pose.f32(NAV_B['position'])[1:].tolist()]
        if literal == 'C': model['nodes'][1]['translation'] = [.25, 0., 6.]
        if literal == 'D': model['nodes'] = nodes(True, True)
    model['buffers'] = [dict(byteLength=len(asset.data), uri='data:application/octet-stream;base64,'+base64.b64encode(asset.data).decode())]
    return model


def state(label, *, main=False, shared=False, selected=1, time=.5, clip=0, pubs=0, history=(0, 0), control='A', nav=NAV_A, failure=None):
    return dict(label=label, main=main, shared=shared, selected=selected, time=time, clip=clip, pubs=pubs,
                history=list(history), control=control, nav=copy.deepcopy(nav), failureAs=failure, resolution=[WIDTH, HEIGHT], msaa=1, scene=0)


def select(node): return dict(cameraSelection=dict(sourceNode=node)) if node >= 0 else dict(cameraSelection=dict(viewport=True))


def generate(output):
    output.mkdir(parents=True, exist_ok=True);inputs=[]
    source=output/'sources/animated.gltf';shear=output/'sources/shear.gltf'
    for path, value in [(source, fixture()), (shear, fixture(shear=True))]:
        path.parent.mkdir(parents=True, exist_ok=True);immutable(path, (json.dumps(value, indent=2)+'\n').encode());inputs.append(path)
    controls=[]
    for name, selected, nav in [('A', 1, NAV_A), ('B', 2, NAV_A), ('C', 1, NAV_A), ('D', 2, NAV_B), ('free-A', -1, NAV_A), ('free-B', -1, NAV_B)]:
        path=output/('sources/control-'+name+'.gltf');immutable(path, (json.dumps(fixture(literal=name), indent=2)+'\n').encode())
        script=output/('scripts/control-'+name+'.json');document(script, dict(schemaVersion=1, frames=13, sampleFrames=[13], camera=[dict(frame=1, **nav)], events=[] if selected<0 else [dict(frame=1, **select(selected))]))
        controls.append(dict(name=name, source=str(path), script=str(script), step=state('control', selected=selected, nav=nav, time=0., clip=-1)))
        inputs += [path, script]
    rows=[(state('before'), [select(1)]),
          (state('write-main', main=True, selected=-1, pubs=1, history=(1,0), control='free-A'), [select(-1), pose.pose_action(1)]),
          (state('main-selected', main=True, pubs=1, history=(1,0)), [select(1)]),
          (state('seek-main', main=True, time=1.5, pubs=1, history=(1,0)), [dict(animationTime=1.5)]),
          (state('undo-main', time=1.5, pubs=2, history=(0,1)), [dict(sceneHistory='undo')]),
          (state('redo-main', main=True, time=1.5, pubs=3, history=(1,0)), [dict(sceneHistory='redo')]),
          (state('clear-main', main=True, time=0., clip=-1, pubs=3, history=(1,0), control='C'), [dict(animationClip=-1)]),
          (state('shared-before', main=True, selected=2, time=.75, pubs=3, history=(1,0)), [dict(animationClip=0, animationTime=.75), select(2)]),
          (state('write-shared', main=True, shared=True, selected=-1, time=.75, pubs=4, history=(2,0), control='free-B', nav=NAV_B), [select(-1), pose.pose_action(2)]),
          (state('seek-shared', main=True, shared=True, selected=2, time=1.25, pubs=4, history=(2,0), control='B', nav=NAV_B), [dict(animationTime=1.25), select(2)]),
          (state('undo-shared', main=True, selected=2, time=1.25, pubs=5, history=(1,1), nav=NAV_B), [dict(sceneHistory='undo')]),
          (state('redo-shared', main=True, shared=True, selected=2, time=1.25, pubs=6, history=(2,0), control='B', nav=NAV_B), [dict(sceneHistory='redo')])]
    steps=[];events=[]
    for i,(expected,actions) in enumerate(rows):
        expected['frame']=13*(i+1);steps.append(expected)
        events += [dict(frame=13*i+1+j, **action) for j,action in enumerate(actions)]
    script=output/'scripts/history.json';camera=[dict(frame=1,**NAV_A),dict(frame=104,**NAV_A),dict(frame=105,**NAV_B)]
    save_events=[*events,dict(frame=157,animationClip=-1)]
    document(script,dict(schemaVersion=1,frames=157,sampleFrames=[s['frame'] for s in steps],camera=camera,events=save_events));inputs.append(script)
    final_script=output/'scripts/glb-save.json';document(final_script,dict(schemaVersion=1,frames=157,sampleFrames=[156],camera=camera,events=save_events));inputs.append(final_script)
    reload_script=output/'scripts/reload.json';reload_steps=[state('default',main=True,shared=True,selected=2,time=0.,clip=-1,control='D',nav=NAV_B),state('animated',main=True,shared=True,selected=2,time=1.25,control='B',nav=NAV_B)]
    for i,s in enumerate(reload_steps):s['frame']=13*(i+1)
    document(reload_script,dict(schemaVersion=1,frames=26,sampleFrames=[13,26],camera=[dict(frame=1,**NAV_B)],events=[dict(frame=1,cameraSelection=dict(nodeName=NAMES[2])),dict(frame=14,animationClip=0,animationTime=1.25)]));inputs.append(reload_script)
    negative_script=output/'scripts/shear.json';negative_steps=[state('warm-slot-a',selected=-1),state('warm-slot-b',selected=-1),state('before',selected=-1),state('rejected',selected=-1,failure='before')]
    for frame,s in zip((*READBACK_WARM_FRAMES,13,26),negative_steps):s['frame']=frame
    document(negative_script,dict(schemaVersion=1,frames=26,sampleFrames=[s['frame'] for s in negative_steps],camera=[dict(frame=1,**NAV_A)],events=[dict(frame=1,**select(1)),dict(frame=2,**select(-1)),dict(frame=14,**pose.pose_action(1),expectCameraPoseFailure=True)]));inputs.append(negative_script)
    for path in inputs:
        if path.parent == output/'scripts':check_camera_event_exclusivity(json.loads(path.read_text()))
    plan=dict(schemaVersion=1,source=str(source),shearSource=str(shear),controls=controls,steps=steps,script=str(script),finalScript=str(final_script),reloadSteps=reload_steps,reloadScript=str(reload_script),negativeSteps=negative_steps,negativeScript=str(negative_script),
              saveFinalization=dict(lastHistoryCaptureFrame=156,clearClipFrame=157,processFinalFrame=157,fileDefaultDomain='edited-rest-after-explicit-clear'),
              exportPreservation=dict(witness='separately captured authored/player-rest endpoint after both edits',
                  independentValidation='existing literal requested-pose, sampled-world/view and image controls',
                  selectedNodeFields='exact binary64 arrays',restoreKeys='exact binary32 arrays including signed zero'),
              readbackWarmup=dict(frames=list(READBACK_WARM_FRAMES),framesInFlight=2,bytesPerPngSlot=PNG_READBACK_BYTES,
                  expectedSecondSlotAllocationDelta=dict(count=1,bytes=PNG_READBACK_BYTES),atomicBaselineFrame=13,
                  reason='PNG capture lazily allocates one host readback buffer per rotating frame slot; warm both before failure atomicity'),
              expectedCaptureCount=27,expectedProcessCount=11,expectedExportCount=2,expectedStateCheckCount=27,expectedImageCheckCount=32,
              immutableInputs=[dict(path=str(p),sha256=sha(p),byteLength=p.stat().st_size) for p in inputs])
    document(output/'launch-plan.json',plan);return plan


def check_save_finalization(script):
    require(script['frames']==157 and max(script['sampleFrames'])==156 and 157 not in script['sampleFrames'],
            'Rest-save clear must follow the final captured history sample without adding a capture')
    require([event for event in script['events'] if event['frame']>156]==[dict(frame=157,animationClip=-1)] and
            script['events'][-1]==dict(frame=157,animationClip=-1),'Rest-save must explicitly clear playback only after history evidence')


def check_camera_event_exclusivity(script):
    camera_actions={'cameraProjection','cameraSelection','cameraPose'}
    # Existing CaptureSequence authored-camera exclusivity in TemporalCapture.h:
    # timeline/history/model/material/physics edits require a different event.
    other_mutations={'resourceRetirement','materialRetirement','materialEdit','materialBinding','materialTextureEdit',
        'merge','gltfScene','materialVariant','nodeFlags','nodeAuthoring','nodeReparent','nodeTransform','addPrimitive',
        'animationPlaying','animationClip','animationTime','interactivityPlaying','interactivityReset','interactivityEvent',
        'sceneHistory','reload','guiReload','guiSampleModel','guiUnload','objectVisible','bimHiddenObject',
        'physicsAttach','physicsAction','physicsJointCreate','physicsJointAction','physicsJointMotorVelocity','physicsSettings','physicsNode'}
    for event in script['events']:
        actions=camera_actions.intersection(event)
        require(not actions or (len(actions)==1 and not other_mutations.intersection(event)),
                'Authored camera actions and other mutations must use separate capture events')


def check_readback_warmup(first,second):
    require(first['framesInFlight']==second['framesInFlight']==2,'PNG readback warmup requires the fixture\'s two frame slots')
    before=pose.failure_signature(first);after=pose.failure_signature(second)
    expected=copy.deepcopy(before)
    # RendererFrontend::ensureScreenshotReadbackBuffer allocates extent*4 bytes
    # on the first PNG use of each frame slot. This is the one expected change
    # before the atomic baseline; all identities, clocks and payloads stay exact.
    expected['gltfMerge']['vmaAllocationCount']+=1
    expected['gltfMerge']['vmaAllocationBytes']+=PNG_READBACK_BYTES
    require(after==expected,'Warm capture changed state beyond the exact second PNG readback allocation')
    return dict(checked=True,frames=list(READBACK_WARM_FRAMES),allocationCountDelta=1,
                allocationBytesDelta=PNG_READBACK_BYTES,allOtherFailureSignatureFieldsExact=True)


def check_state(telemetry, expected, *, reload=False, shear=False, baseline=None):
    require(finite_json(telemetry), 'Nonfinite animated camera telemetry')
    p,c,player=telemetry['gltfCameraPoses'],telemetry['gltfCameraAuthoring'],telemetry['gltfAnimation']
    require(not p['truncated'] and not c['truncated'] and p['publications']==expected['pubs'] and c['publications']==0, 'Wrong pose/lens publication inventory')
    require(p['viewportActive']==(expected['selected']<0), 'Pose write changed navigation mode')
    require(player['selectedClip']==expected['clip'] and player['playing'] is False and player['loop'] is True, 'Pose history rewound clip/playback controls')
    close(player['time'],expected['time'],'current clock',0.);close(player['speed'],1.,'playback speed',0.)
    history_cursor(telemetry,*expected['history'],allow_uncreated=expected['history']==[0,0])
    require(telemetry['gltfScenes']['selected']==0 and telemetry['scene']['primaryObjectCount']==6 and telemetry['scene']['primaryVertexCount']==24 and telemetry['scene']['primaryIndexCount']==36, 'Pose changed source scene/geometry')
    authored=nodes(expected['main'],expected['shared'],shear=shear)
    evaluated=nodes(expected['main'],expected['shared'],time=expected['time'] if expected['clip']==0 else None,shear=shear)
    worlds=pose.world_matrices(evaluated);graph=projection.graph_rows(telemetry)
    rows={row['authored']['name']:row for row in p['nodes']}
    require(len(p['nodes'])==len(rows)==3,'Camera source identity was lost or duplicated')
    for index,name in NAMES.items():
        row=rows[name];require(row['nodeIncarnation']>0,'Missing camera node birth')
        require(all(exact_numbers(row['authored'][field],row['rest'][field],8) for field in FIELDS),
                'Authored and player-rest pose fields differ: '+name)
        if not reload:require(row['sourceNode']==index,'Camera source node was rebound')
        for domain,wanted in [('authored',authored),('rest',authored),('evaluated',evaluated)]:
            pose.check_pose_record(row[domain],wanted[index],domain+' '+name,canonical=True)
            if index in (1,2):require(not row[domain]['matrix'],'Animated camera was serialized/evaluated as a matrix')
        owner=next(r for r in graph if r['graphNode']==row['graphNode']);require(owner['active'] and not owner['retired'],'Camera graph owner unavailable')
        close(np.asarray(owner['worldColumns']).T,worlds[index],name+' sampled world')
        if baseline:
            old=next(r for r in baseline['gltfCameraPoses']['nodes'] if r['authored']['name']==name)
            require(all(row[k]==old[k] for k in ('sourceNode','graphNode','nodeIncarnation')),'History replaced camera lifetime')
    for index in (0,3):
        owner=[r for r in graph if r['name']==evaluated[index]['name'] and r['sourceNode']>=0]
        require(len(owner)==1,'Parent/child source binding unavailable');close(np.asarray(owner[0]['worldColumns']).T,worlds[index],'parent/child world')
    for domain in ('authored','evaluated','rests'):
        require(c[domain]==[dict(name=v['name'],type=v['type'],values=v['values']) for v in projection.camera_records()], 'Pose changed exact lens defaults or samples')
    selected=expected['selected'];source=-1 if selected<0 else rows[NAMES[selected]]['sourceNode']
    require(c['selectedSourceNode']==source and player['selectedCameraNode']==source,'Pose changed selected camera')
    if selected>=0:
        close(c['view'],pose.flat(pose.view_from_world(worlds[selected])),'selected sampled view')
        close(c['projection'],pose.flat(projection.projection_matrix(projection.camera_records()[0 if selected in (1,2) else 1],[WIDTH,HEIGHT])),'kept lens projection')
        close(telemetry['camera']['position'],worlds[selected][:3,3],'current camera eye')
    else:close(telemetry['camera']['position'],expected['nav']['position'],'free viewport eye')
    lights=player['pointLights'];require(len(lights)==1 and telemetry['lighting']['pointLightCount']==1,'Child light inventory changed')
    close(lights[0]['position'],worlds[3][:3,3],'sampled child light position')
    require(lights[0]['intensity']==20. and lights[0]['range']==6.,'Child light photometry changed')
    if baseline:require(c['revision']-expected['pubs']==baseline['gltfCameraAuthoring']['revision']-baseline['gltfCameraPoses']['publications'],'Pose revision skipped or double counted')
    return dict(checked=True,domains=['authored','rest','sampled','graph','current-clock','lens','child-light','history'])


def check_literal_control(telemetry, control):
    model,_=load_asset(Path(control['source']));worlds=pose.world_matrices(model['nodes']);expected=control['step']
    require(not model.get('animations') and finite_json(telemetry),'Literal control unexpectedly has playback or nonfinite telemetry')
    require(telemetry['scene']['primaryObjectCount']==6 and telemetry['scene']['primaryVertexCount']==24 and
            telemetry['scene']['primaryIndexCount']==36,'Literal control changed geometry inventory')
    history_cursor(telemetry,0,0,allow_uncreated=True)
    poses=telemetry['gltfCameraPoses'];cameras=telemetry['gltfCameraAuthoring'];graph=projection.graph_rows(telemetry)
    require(poses['publications']==cameras['publications']==0 and not poses['truncated'] and not cameras['truncated'],
            'Literal control has edits or incomplete identities')
    require(poses['viewportActive']==(expected['selected']<0) and len(poses['nodes'])==3,'Literal camera mode/inventory changed')
    for index,name in NAMES.items():
        rows=[r for r in poses['nodes'] if r['authored']['name']==name];require(len(rows)==1,'Literal camera identity ambiguous')
        row=rows[0];require(row['sourceNode']==index,'Literal camera source rebound')
        for domain in ('authored','rest','evaluated'):pose.check_pose_record(row[domain],model['nodes'][index],domain+' literal '+name,canonical=True)
        graph_row=next(r for r in graph if r['graphNode']==row['graphNode'])
        require(graph_row['active'] and not graph_row['retired'],'Literal camera graph unavailable')
        close(np.asarray(graph_row['worldColumns']).T,worlds[index],'literal world '+name)
    for domain in ('authored','evaluated','rests'):require(cameras[domain]==projection.camera_records(),'Literal lens changed')
    selected=expected['selected'];require(cameras['selectedSourceNode']==selected and
        telemetry['gltfAnimation']['selectedCameraNode']==selected,'Literal selected the wrong camera')
    if selected>=0:
        close(cameras['view'],pose.flat(pose.view_from_world(worlds[selected])),'literal camera view')
        close(cameras['projection'],pose.flat(projection.projection_matrix(projection.camera_records()[0],[WIDTH,HEIGHT])),'literal lens projection')
        close(telemetry['camera']['position'],worlds[selected][:3,3],'literal camera position')
    else:close(telemetry['camera']['position'],expected['nav']['position'],'literal free eye')
    lights=telemetry['gltfAnimation']['pointLights'];require(len(lights)==1 and telemetry['lighting']['pointLightCount']==1,'Literal child light lost')
    close(lights[0]['position'],worlds[3][:3,3],'literal child light')
    return dict(checked=True,independentStaticControl=control['name'],domains=['literal-source','world','lens','selection','child-light','geometry'])


def edge(model,target):
    if 'node' in target:return ('node',model['nodes'][target['node']]['name'],target['path'])
    pointer=target.get('extensions',{}).get(POINTER,{}).get('pointer','')
    match=re.fullmatch(r'/(nodes|materials)/(\d+)/(.+)',pointer);require(match,'Unsupported animation pointer in proof')
    table,index,property=match.groups();return ('node' if table=='nodes' else 'material',model[table][int(index)]['name'],property)


def curve_rows(path):
    model,_=load_asset(path);result=[]
    for clip in model.get('animations',[]):
        channels=[]
        for channel in clip['channels']:
            sampler=clip['samplers'][channel['sampler']]
            channels.append(dict(edge=edge(model,channel['target']),channelExtras=channel.get('extras'),samplerExtras=sampler.get('extras'),interpolation=sampler.get('interpolation','LINEAR'),
                input=np.asarray(decode_accessor(path,sampler['input'])).reshape(-1).tolist(),output=np.asarray(decode_accessor(path,sampler['output'])).reshape(-1).tolist(),
                inputType=model['accessors'][sampler['input']]['type'],outputType=model['accessors'][sampler['output']]['type'],
                inputComponentType=model['accessors'][sampler['input']]['componentType'],outputComponentType=model['accessors'][sampler['output']]['componentType']))
        result.append(dict(name=clip['name'],extras=clip.get('extras'),channels=channels))
    return result


def exact_numbers(first,second,width):
    # Numeric equality alone equates signed zero. Preserve the encoded FLOAT
    # restore and authored binary64 pose contracts independently.
    dtype='<f4' if width==4 else '<f8'
    a=np.asarray(first,dtype=dtype).reshape(-1);b=np.asarray(second,dtype=dtype).reshape(-1)
    return a.shape==b.shape and a.tobytes()==b.tobytes()


def pose_fields(node):
    return {field:copy.deepcopy(node.get(field,[])) for field in FIELDS}


def validate_rest_witness(witness):
    require(set(witness)=={NAMES[1],NAMES[2]} and finite_json(witness),'Missing/nonfinite authored-rest witness')
    nominal=nodes(True,True)
    for index in (1,2):
        fields=witness[NAMES[index]]
        require(set(fields)==set(FIELDS) and not fields['matrix'] and
                len(fields['translation']) in (0,3) and len(fields['rotation']) in (0,4) and len(fields['scale']) in (0,3),
                'Authored-rest witness is not a valid TRS endpoint')
        # Keep the independent literal requested-pose check. Controller
        # binary32 lookAt can retain a tiny rotation instead of exact identity;
        # export must preserve that separately verified endpoint exactly.
        close(pose.node_matrix(fields),pose.node_matrix(nominal[index]),'independent authored-rest witness')
    return witness


def rest_values(fields,field):
    return fields[field] or {'translation':[0.,0.,0.],'rotation':[0.,0.,0.,1.],'scale':[1.,1.,1.]}[field]


def captured_rest_witness(telemetry,expected):
    require(expected['main'] and expected['shared'],'Preservation witness must follow both pose edits')
    check_state(telemetry,expected)
    rows={row['authored']['name']:row for row in telemetry['gltfCameraPoses']['nodes']}
    result={}
    for index in (1,2):
        row=rows[NAMES[index]]
        require(all(exact_numbers(row['authored'][field],row['rest'][field],8) for field in FIELDS),
                'Captured authored/rest endpoint differs')
        result[NAMES[index]]=pose_fields(row['authored'])
    return validate_rest_witness(result)


def inspect_export(path,original,*,restores=True,rest_witness=None):
    model,buffers=load_asset(path);source,_=load_asset(original)
    require(finite_json(model) and sum(map(len,buffers))<16*1024*1024 and len(model['nodes'])<=64,'Unbounded/nonfinite portable camera model')
    expected=nodes(True,True);mapped={}
    require(not restores or rest_witness is not None,'Restoration proof requires a separately captured/validated authored-rest witness')
    if rest_witness is not None:validate_rest_witness(rest_witness)
    for index,wanted in enumerate(expected):
        matches=[i for i,row in enumerate(model['nodes']) if row.get('extras',{}).get('nodeIdentity')==wanted['extras']['nodeIdentity']]
        require(len(matches)==1,'Portable source metadata/identity lost or duplicated');mapped[index]=matches[0];actual=model['nodes'][matches[0]]
        require(actual.get('extras')==wanted['extras'] and actual['name']==wanted['name'],'Portable node metadata changed')
        require(not ('matrix' in actual and any(k in actual for k in FIELDS[1:])),'Portable node has simultaneous matrix and TRS')
        if index in (0,1,2):require(not actual.get('matrix'),'Portable TRS-animated node has matrix representation')
        close(pose.node_matrix(actual),pose.node_matrix(wanted),'portable authored default')
        if rest_witness is not None and wanted['name'] in rest_witness:
            require(all(exact_numbers(actual.get(field,[]),rest_witness[wanted['name']][field],8) for field in FIELDS),
                    'Portable selected-node pose differs from separately verified authored/rest endpoint')
    for parent,children in [(0,[1,2]),(2,[3])]:
        require([i for i in model['nodes'][mapped[parent]].get('children',[]) if i in mapped.values()]==[mapped[c] for c in children],'Portable authored hierarchy changed')
    original_curves=curve_rows(original);actual_curves=curve_rows(path);require(len(actual_curves)==2,'Portable clip inventory changed')
    values=rest_witness or {NAMES[i]:pose_fields(expected[i]) for i in (1,2)}
    restore_values=[{('material','Surface 0','pbrMetallicRoughness/roughnessFactor'):[source['materials'][0]['pbrMetallicRoughness']['roughnessFactor']]},
        {('node','Animated parent','translation'):[0.,0.,0.],('node',NAMES[1],'translation'):rest_values(values[NAMES[1]],'translation'),
         ('node',NAMES[1],'rotation'):rest_values(values[NAMES[1]],'rotation'),('node',NAMES[1],'scale'):rest_values(values[NAMES[1]],'scale'),
         ('node',NAMES[2],'translation/0'):[rest_values(values[NAMES[2]],'translation')[0]],('node',NAMES[2],'scale/2'):[rest_values(values[NAMES[2]],'scale')[2]]}]
    for number,(actual,wanted) in enumerate(zip(actual_curves,original_curves)):
        require(actual['name']==wanted['name'] and actual['extras']==wanted['extras'],'Portable clip metadata changed')
        count=len(wanted['channels']);require(actual['channels'][:count]==wanted['channels'],'Original curve/target/interpolation/metadata/decoded FLOAT keys or tangents changed')
        extra=actual['channels'][count:];allowed=restore_values[number] if restores else {}
        require(len(extra)==len(allowed),'Unexpected portable restoration curve count')
        for row in extra:
            key=tuple(row['edge']);require(key in allowed,'Unexpected/duplicate restoration target')
            require(row['channelExtras'] is None and row['samplerExtras'] is None and row['interpolation']=='STEP' and row['input']==[0.],'Restoration is not a single constant STEP')
            expected_type='VEC4' if key[-1]=='rotation' else 'VEC3' if key[-1] in ('translation','scale') else 'SCALAR'
            require(row['inputType']=='SCALAR' and row['outputType']==expected_type and row['inputComponentType']==row['outputComponentType']==5126,
                    'Restoration accessor changed its required FLOAT scalar/vector shape')
            require(exact_numbers(row['output'],allowed.pop(key),4),'Restoration changed exact binary32 authored default')
    require(len(model['cameras'])==2,'Portable lens definitions lost')
    for actual,wanted in zip(model['cameras'],source['cameras']):binding.deep_contains(actual,wanted,'portable untouched lens')
    geometry={mesh.get('extras',{}).get('geometryIdentity'):i for i,mesh in enumerate(model['meshes'])}
    require(set(geometry)==set(range(6)) and len(model['meshes'])==6,'Portable geometry identities changed')
    for identity,index in geometry.items():
        primitives=model['meshes'][index]['primitives'];original_primitive=source['meshes'][identity]['primitives'][0]
        require(len(primitives)==1 and primitives[0].get('mode',4)==4,'Portable primitive shape changed')
        for attribute,accessor in original_primitive['attributes'].items():
            require(np.array_equal(decode_accessor(path,primitives[0]['attributes'][attribute]),decode_accessor(original,accessor)),'Portable geometry accessor changed')
        require(np.array_equal(decode_accessor(path,primitives[0]['indices']),decode_accessor(original,original_primitive['indices'])),'Portable indices changed')
    return dict(path=str(path),sha256=sha(path),originalClips=2,originalCurves=7,authoredDefaults=True,exactKeysAndTangents=True,
                separatelyVerifiedAuthoredRest=rest_witness,exactBinary32Restores=restores)


def dependencies():
    directory=Path(__file__).resolve().parent
    return dict(sorted((str(Path(m.__file__).resolve()),sha(Path(m.__file__).resolve())) for m in tuple(sys.modules.values())
        if getattr(m,'__file__',None) and Path(m.__file__).resolve().parent==directory))


def run_native(args,output,plan,report):
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/*')) and not (output/'offline-sources').exists(),'Native evidence must be fresh')
    if (output/'results.json').exists():
        previous=json.loads((output/'results.json').read_text());require(previous.get('generatedOnly') and not previous.get('rendererExecuted'),'Refusing to overwrite native evidence')
    manifest_hash=sha(args.manifest);runtime=sorted([*args.renderer.parent.glob('*.dll'),*(args.renderer.parent/'spv_shaders').glob('*.spv')]);moved=False
    frozen={Path(name).resolve() for name in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
    require(runtime and {args.renderer,*map(Path,report['sourceDependencies']),*map(Path.resolve,runtime)}<=frozen,'Frozen manifest omits renderer/helper/DLL/SPIR-V input')
    immutable(output/'vk_layer_settings.txt',VALIDATION_SETTINGS)
    def persist():
        for key in ('captures','processes','stateChecks','imageChecks','exports','readbackWarmupChecks'):report[key+'Count']=len(report[key])
        (output/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    def verify():
        require(sha(args.manifest)==manifest_hash and dependencies()==report['sourceDependencies'] and sha(output/'launch-plan.json')==report['launchPlanSha256'],'Frozen native manifest/helper/plan changed')
        result=verify_manifest(args.manifest)
        require((output/'vk_layer_settings.txt').read_bytes()==VALIDATION_SETTINGS,'Synchronization settings changed')
        require(sorted([*args.renderer.parent.glob('*.dll'),*(args.renderer.parent/'spv_shaders').glob('*.spv')])==runtime,'Resident runtime inventory changed')
        for item in plan['immutableInputs']:
            path=Path(item['path'])
            if moved and path.is_relative_to(output/'sources'):path=output/'offline-sources'/path.relative_to(output/'sources')
            require(path.stat().st_size==item['byteLength'] and sha(path)==item['sha256'],'Generated immutable input changed')
            if path.parent == output/'scripts':check_camera_event_exclusivity(json.loads(path.read_text()))
        for key in ('script','finalScript'):check_save_finalization(json.loads(Path(plan[key]).read_text()))
        for bundle in report['portableCopies']:require(all(sha(Path(row['path']))==row['sha256'] for row in bundle['files']),'Portable payload changed')
        return result
    def check(kind,technique,name,value):report[kind].append(dict(technique=technique,check=name,**value));persist()
    reloads=[]
    try:
        report['nativeManifestBefore']=verify();report['rendererSha256']=sha(args.renderer);persist()
        for technique in args.technique:
            controls={}
            for control in plan['controls']:
                step=copy.deepcopy(control['step']);step['frame']=13
                record=binding.launch(args,output,report,persist,verify,technique,'control-'+control['name'],Path(control['source']),[step],script=Path(control['script']),initial=step)['control']
                check('stateChecks',technique,'control-'+control['name'],check_literal_control(record['state'],control))
                check('imageChecks',technique,'control-'+control['name']+'-visible',projection.visible(record));controls[control['name']]=record
            records=binding.launch(args,output,report,persist,verify,technique,'history',Path(plan['source']),plan['steps'],script=Path(plan['script']),save=output/'exports'/technique/'scene.gltf',animation_time=.5)
            baseline=records['before']['state'];projection.temporal_check(records,plan['steps'])
            for expected in plan['steps']:
                record=records[expected['label']];check('stateChecks',technique,expected['label'],check_state(record['state'],expected,baseline=baseline))
                check('imageChecks',technique,expected['label'],projection.compare_images(record,controls[expected['control']]))
            for first,second,label in [('before','main-selected','fully-animated-sample-unchanged'),('seek-main','undo-main','current-clock-undo-unchanged'),('undo-main','redo-main','current-clock-redo-unchanged')]:
                check('imageChecks',technique,label,projection.compare_images(records[first],records[second]))
            for first,second,label in [('main-selected','clear-main','clear-reveals-default'),('shared-before','seek-shared','partial-default-visible'),('seek-shared','undo-shared','partial-undo-visible')]:
                check('imageChecks',technique,label,projection.changed(records[first],records[second]))
            endpoint=copy.deepcopy(plan['steps'][-1]);endpoint['label']='export-endpoint'
            extra=binding.launch(args,output,report,persist,verify,technique,'glb-save',Path(plan['source']),[endpoint],script=Path(plan['finalScript']),save=output/'exports'/technique/'scene.glb',animation_time=.5)['export-endpoint']
            check('stateChecks',technique,'glb-save',check_state(extra['state'],endpoint));check('imageChecks',technique,'glb-save',projection.compare_images(extra,controls['B']))
            witness_records={'gltf':records[plan['steps'][-1]['label']],'glb':extra}
            witnesses={extension:captured_rest_witness(record['state'],endpoint) for extension,record in witness_records.items()}
            negative=binding.launch(args,output,report,persist,verify,technique,'shear',Path(plan['shearSource']),plan['negativeSteps'],script=Path(plan['negativeScript']),animation_time=.5)
            log=Path(negative['before']['log']).read_text(encoding='utf-8',errors='replace');require(log.count('Expected capture camera pose failure: ')==1 and 'shear' in log.lower(),'Missing precise atomic shear rejection')
            for expected in plan['negativeSteps']:check('stateChecks',technique,'shear-'+expected['label'],check_state(negative[expected['label']]['state'],expected,shear=True))
            check('readbackWarmupChecks',technique,'two-png-frame-slots',check_readback_warmup(negative['warm-slot-a']['state'],negative['warm-slot-b']['state']))
            require(pose.failure_signature(negative['warm-slot-b']['state'])==pose.failure_signature(negative['before']['state']),
                    'Atomic baseline changed state or allocations after the PNG frame slots were warm')
            check('imageChecks',technique,'shear-warm-slots-identical',projection.compare_images(negative['warm-slot-a'],negative['warm-slot-b']))
            check('imageChecks',technique,'shear-warm-baseline-identical',projection.compare_images(negative['warm-slot-b'],negative['before']))
            require(pose.failure_signature(negative['before']['state'])==pose.failure_signature(negative['rejected']['state']),'Shear rejection changed source/rest/graph/history/clock')
            check('imageChecks',technique,'shear-atomicity',projection.compare_images(negative['before'],negative['rejected']))
            for extension in ('gltf','glb'):
                exported=output/'exports'/technique/('scene.'+extension);record=witness_records[extension]
                report['exports'].append(dict(technique=technique,witnessTelemetry=record['telemetry'],witnessTelemetrySha256=record['telemetrySha256'],
                    **inspect_export(exported,Path(plan['source']),rest_witness=witnesses[extension])));persist()
                copied,files=portable_copy(exported,output/'portable'/technique/extension);report['portableCopies'].append(dict(files=files));reloads.append((technique,extension,copied,controls))
        # Prove offline reload: original generated glTF files cease to exist.
        source_dir=(output/'sources').resolve();offline=(output/'offline-sources').resolve()
        require(source_dir.parent==output and offline.parent==output and not offline.exists(),'Unsafe source-isolation move')
        source_dir.rename(offline);moved=True;report['originalSourcesUnavailable']=True;persist();verify()
        for technique,extension,copied,controls in reloads:
            records=binding.launch(args,output,report,persist,verify,technique,'offline-'+extension,copied,plan['reloadSteps'],script=Path(plan['reloadScript']))
            for expected in plan['reloadSteps']:
                record=records[expected['label']];check('stateChecks',technique,'offline-'+extension+'-'+expected['label'],check_state(record['state'],expected,reload=True))
                check('imageChecks',technique,'offline-'+extension+'-'+expected['label'],projection.compare_images(record,controls[expected['control']]))
        for key in ('captures','processes','exports','stateChecks','imageChecks','portableCopies','readbackWarmupChecks'):
            per_technique={'captures':27,'processes':11,'exports':2,'stateChecks':27,'imageChecks':32,'portableCopies':2,'readbackWarmupChecks':1}[key]
            require(len(report[key])==per_technique*len(args.technique),'Incomplete animated-pose native '+key+' inventory')
        report['nativeManifestAfter']=verify();report.update(passed=True,nativeValidationPerformed=True)
    except Exception as error:report.update(passed=False,failure=str(error));persist();raise
    finally:
        if report['rendererExecuted']:
            try:report['nativeManifestAfter']=verify()
            except Exception as error:report.update(passed=False,afterManifestFailure=str(error))
        persist()
    require(report['passed'],'Animated camera provenance failed after execution')


def self_check():
    checks=[]
    temporary_root=Path(__file__).resolve().parents[2]/'build/codex-gltf-validation/self-checks'
    temporary_root.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='animated-camera-',dir=temporary_root) as directory:
        require(Path(directory).resolve().parent==temporary_root.resolve(),'Unsafe self-check cleanup directory')
        output=Path(directory);plan=generate(output)
        require((plan['expectedCaptureCount']*2,plan['expectedProcessCount']*2)==(54,22),'Native inventory drift')
        checks.append('declared-native-inventory')
        negative=json.loads(Path(plan['negativeScript']).read_text())
        require(negative['sampleFrames']==[3,4,13,26] and [s['frame'] for s in plan['negativeSteps']]==[3,4,13,26] and
                max(READBACK_WARM_FRAMES)<13<negative['events'][-1]['frame']==14<26,
                'PNG frame-slot warming must finish before atomic baseline and rejected edit')
        checks.append('bounded-two-slot-png-warm-captures-before-atomic-baseline')
        warm_first=dict(framesInFlight=2,gltfMerge=dict(vmaAllocationCount=100,vmaAllocationBytes=1_000_000,graphIdentity=7),
            gltfAnimation=dict(time=.5),gltfCameraPoses=dict(publications=0),
            taa=dict(epoch=3,resetReason='camera selection'),camera=dict(position=[.5,0,6],unjitteredViewProjColumns=[1,2,3]),
            lighting=dict(pointLightCount=1,areaLightCount=0))
        warm_second=copy.deepcopy(warm_first);warm_second['gltfMerge']['vmaAllocationCount']+=1
        warm_second['gltfMerge']['vmaAllocationBytes']+=PNG_READBACK_BYTES
        check_readback_warmup(warm_first,warm_second);checks.append('exact-png-readback-allocation-proof')
        for label,change in [('extra-allocation',lambda s:s['gltfMerge'].__setitem__('vmaAllocationCount',102)),
                ('wrong-byte-size',lambda s:s['gltfMerge'].__setitem__('vmaAllocationBytes',s['gltfMerge']['vmaAllocationBytes']+4)),
                ('clock-change',lambda s:s['gltfAnimation'].__setitem__('time',.75)),
                ('graph-change',lambda s:s['gltfMerge'].__setitem__('graphIdentity',8)),
                ('pose-change',lambda s:s['gltfCameraPoses'].__setitem__('publications',1))]:
            wrong=copy.deepcopy(warm_second);change(wrong)
            try:check_readback_warmup(warm_first,wrong)
            except RuntimeError:checks.append('reject-png-warmup-'+label)
            else:raise AssertionError('Warmup oracle accepted '+label)
        for item in plan['immutableInputs']:
            path=Path(item['path'])
            if path.parent == output/'scripts':check_camera_event_exclusivity(json.loads(path.read_text()))
        checks.append('all-generated-camera-events-obey-parser-exclusivity')
        history=json.loads(Path(plan['script']).read_text())
        for start,action in [(92,dict(animationClip=0,animationTime=.75)),(118,dict(animationTime=1.25))]:
            require(next(event for event in history['events'] if event['frame']==start)==dict(frame=start,**action) and
                    next(event for event in history['events'] if event['frame']==start+1)==dict(frame=start+1,**select(2)),
                    'Timeline must precede shared-camera selection on consecutive frames')
        checks.append('shared-timeline-then-selection-consecutive-frame-order')
        for key in ('animationTime','sceneHistory','cameraPose'):
            bad=dict(schemaVersion=1,frames=13,sampleFrames=[13],events=[dict(frame=1,**select(2),**{
                key:.75 if key=='animationTime' else 'undo' if key=='sceneHistory' else dict(sourceNode=2)})])
            try:check_camera_event_exclusivity(bad)
            except RuntimeError:checks.append('reject-mixed-camera-'+key)
            else:raise AssertionError('Oracle accepted mixed camera '+key)
        for key in ('script','finalScript'):check_save_finalization(json.loads(Path(plan[key]).read_text()))
        checks.append('post-history-explicit-clear-before-application-save')
        bad_order=json.loads(Path(plan['script']).read_text());bad_order['events'][-1]['frame']=156
        try:check_save_finalization(bad_order)
        except RuntimeError:checks.append('reject-clear-before-final-history-sample')
        else:raise AssertionError('Oracle accepted early rest-save clear')
        for time in (.5,.75,1.25,1.5):
            world=pose.world_matrices(nodes(True,True,time=time))
            close(world[1][:3,3],[0,0,6],'independent parent/full-translation cancellation',0.)
            close(world[2][:3,3],[0,*pose.f32(NAV_B['position'])[1:]],'independent partial-translation world',0.)
        checks.append('independent-world-controls')
        shear=pose.world_matrices(nodes(time=.5,shear=True))[1];rigid=np.linalg.inv(pose.view_from_world(shear))[:3,:3]
        local=np.linalg.inv(pose.node_matrix(nodes(shear=True)[0]))[:3,:3]@(rigid.T@shear[:3,:3])
        require(abs(np.dot(local[:,0],local[:,1]))/(np.linalg.norm(local[:,0])*np.linalg.norm(local[:,1]))>.01,'Negative shear fixture is representable/vacuous')
        checks.append('specific-local-shear-negative')
        model=fixture();model['nodes']=nodes(True,True);positive=output/'synthetic-export.gltf';immutable(positive,(json.dumps(model)+'\n').encode())
        inspect_export(positive,Path(plan['source']),restores=False);checks.append('exact-original-curves-defaults')
        restored=Asset();restored.model=copy.deepcopy(model);restored.data=bytearray(base64.b64decode(model['buffers'][0]['uri'].split(',',1)[1]))
        zero=restored.accessor('SCALAR',[0.])
        literal_restores=[(0,None,'/materials/0/pbrMetallicRoughness/roughnessFactor','SCALAR',[.7]),
            (1,0,'translation','VEC3',[[0.,0.,0.]]),(1,1,'translation','VEC3',[[.25,0.,6.]]),
            (1,1,'rotation','VEC4',[[0.,0.,0.,1.]]),(1,1,'scale','VEC3',[[1.,1.,1.]]),
            (1,None,'/nodes/2/translation/0','SCALAR',[float(np.float32(-.6)-np.float32(.3125))]),
            (1,None,'/nodes/2/scale/2','SCALAR',[1.])]
        for clip_index,node,property,kind,values in literal_restores:
            clip=restored.model['animations'][clip_index];index=len(clip['samplers'])
            clip['samplers'].append(dict(input=zero,output=restored.accessor(kind,values),interpolation='STEP'))
            target=dict(node=node,path=property) if node is not None else dict(path='pointer',extensions={POINTER:dict(pointer=property)})
            clip['channels'].append(dict(sampler=index,target=target))
        restored.model['buffers']=[dict(byteLength=len(restored.data),uri='data:application/octet-stream;base64,'+base64.b64encode(restored.data).decode())]
        restore_path=output/'synthetic-restores.gltf';immutable(restore_path,(json.dumps(restored.model)+'\n').encode())
        literal_witness=validate_rest_witness({NAMES[i]:pose_fields(model['nodes'][i]) for i in (1,2)})
        inspect_export(restore_path,Path(plan['source']),rest_witness=literal_witness);checks.append('exact-cross-clip-restoration-tracks')
        wrong=copy.deepcopy(restored.model);wrong['animations'][1]['channels'].pop();path=output/'missing-restore.gltf';immutable(path,(json.dumps(wrong)+'\n').encode())
        try:inspect_export(path,Path(plan['source']),rest_witness=literal_witness)
        except (AssertionError,ValueError,RuntimeError):checks.append('reject-missing-cross-clip-restore')
        else:raise AssertionError('Oracle accepted missing cross-clip restoration')
        # A valid requested axis view can retain binary32 lookAt roundoff.
        # Use an independently bounded nonzero endpoint, then insist export
        # preserves its raw doubles and FLOAT restore rather than snapping it.
        tiny=copy.deepcopy(restored.model)
        tiny['nodes'][1]['rotation']=[0.,2.**-26,0.,math.sqrt(1.-2.**-52)]
        tiny_witness=validate_rest_witness({NAMES[i]:pose_fields(tiny['nodes'][i]) for i in (1,2)})
        rotation_channel=next(channel for channel in tiny['animations'][1]['channels']
            if channel['target'].get('node')==1 and channel['target'].get('path')=='rotation')
        rotation_sampler=tiny['animations'][1]['samplers'][rotation_channel['sampler']]
        rotation_accessor=tiny['accessors'][rotation_sampler['output']]
        rotation_view=tiny['bufferViews'][rotation_accessor['bufferView']]
        rotation_offset=rotation_view.get('byteOffset',0)+rotation_accessor.get('byteOffset',0)
        def write_rotation_float(value,component,number):
            data=bytearray(base64.b64decode(value['buffers'][0]['uri'].split(',',1)[1]))
            struct.pack_into('<f',data,rotation_offset+4*component,number)
            value['buffers'][0]['uri']='data:application/octet-stream;base64,'+base64.b64encode(data).decode()
        write_rotation_float(tiny,1,2.**-26)
        tiny_path=output/'nonzero-rest-quaternion.gltf';immutable(tiny_path,(json.dumps(tiny)+'\n').encode())
        inspect_export(tiny_path,Path(plan['source']),rest_witness=tiny_witness)
        checks.append('exact-nonzero-authored-quaternion-and-float-restore')
        for label,component,value in [('zeroed-tiny-rotation',1,0.),('tampered-tiny-rotation',1,2.**-25),('restore-signed-zero',0,-0.)]:
            wrong=copy.deepcopy(tiny);write_rotation_float(wrong,component,value)
            path=output/(label+'.gltf');immutable(path,(json.dumps(wrong)+'\n').encode())
            try:inspect_export(path,Path(plan['source']),rest_witness=tiny_witness)
            except (AssertionError,ValueError,RuntimeError):checks.append('reject-'+label)
            else:raise AssertionError('Oracle accepted '+label)
        wrong=copy.deepcopy(tiny);wrong['nodes'][1]['rotation']=[0.,0.,0.,1.]
        path=output/'snapped-authored-rotation.gltf';immutable(path,(json.dumps(wrong)+'\n').encode())
        try:inspect_export(path,Path(plan['source']),rest_witness=tiny_witness)
        except (AssertionError,ValueError,RuntimeError):checks.append('reject-snapped-authored-rotation')
        else:raise AssertionError('Oracle accepted snapped authored quaternion')
        bad_witness=copy.deepcopy(tiny_witness);bad_witness[NAMES[1]]['translation'][0]+=.25
        try:validate_rest_witness(bad_witness)
        except (AssertionError,ValueError,RuntimeError):checks.append('reject-physically-wrong-preservation-witness')
        else:raise AssertionError('Oracle accepted a physically wrong authored endpoint')
        def change_tangent(value):
            accessor=value['accessors'][value['animations'][0]['samplers'][2]['output']]
            view=value['bufferViews'][accessor['bufferView']];data=bytearray(base64.b64decode(value['buffers'][0]['uri'].split(',',1)[1]))
            struct.pack_into('<f',data,view.get('byteOffset',0)+accessor.get('byteOffset',0),.125)
            value['buffers'][0]['uri']='data:application/octet-stream;base64,'+base64.b64encode(data).decode()
        for label,mutate in [('curve-tangent',change_tangent),
                             ('curve-interpolation',lambda m:m['animations'][0]['samplers'][2].__setitem__('interpolation','LINEAR')),
                             ('retargeted-component',lambda m:m['animations'][0]['channels'][4]['target']['extensions'][POINTER].__setitem__('pointer','/nodes/2/translation/1')),
                             ('sample-saved-as-default',lambda m:m['nodes'][2].__setitem__('translation',[0,.2,5.4])),
                             ('lost-metadata',lambda m:m['nodes'][1]['extras'].clear())]:
            wrong=copy.deepcopy(model);mutate(wrong);path=output/(label+'.gltf');immutable(path,(json.dumps(wrong)+'\n').encode())
            try:inspect_export(path,Path(plan['source']),restores=False)
            except (AssertionError,ValueError,RuntimeError):checks.append('reject-'+label)
            else:raise AssertionError('Oracle accepted '+label)
    return dict(passed=True,checkCount=len(checks),checks=checks,rendererExecuted=False,nativeValidationPerformed=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path);parser.add_argument('--renderer',type=Path);parser.add_argument('--manifest',type=Path)
    parser.add_argument('--technique',choices=TECHNIQUES,action='append');parser.add_argument('--timeout',type=float,default=600.)
    parser.add_argument('--self-check',action='store_true');parser.add_argument('--generate-only',action='store_true');args=parser.parse_args()
    if args.self_check:print(json.dumps(self_check(),indent=2));return
    if args.output is None:parser.error('--output is required')
    if not math.isfinite(args.timeout) or args.timeout<=0:parser.error('--timeout must be finite and positive')
    if not args.generate_only and (args.renderer is None or args.manifest is None):parser.error('Native work requires --renderer and --manifest')
    args.technique=tuple(dict.fromkeys(args.technique or TECHNIQUES));args.output=args.output.resolve()
    if args.renderer:args.renderer=args.renderer.resolve()
    if args.manifest:args.manifest=args.manifest.resolve()
    plan=generate(args.output)
    report=dict(schemaVersion=1,passed=None if args.generate_only else False,generatedOnly=args.generate_only,rendererExecuted=False,nativeValidationPerformed=False,
                sourceDependencies=dependencies(),launchPlanSha256=sha(args.output/'launch-plan.json'),expectedCaptureCount=27*len(args.technique),expectedProcessCount=11*len(args.technique),expectedExportCount=2*len(args.technique),
                expectedStateCheckCount=27*len(args.technique),expectedImageCheckCount=32*len(args.technique),
                processes=[],captures=[],stateChecks=[],imageChecks=[],exports=[],portableCopies=[],readbackWarmupChecks=[],unverified=UNVERIFIED)
    if args.generate_only:document(args.output/'results.json',report);print('GENERATED',report['expectedCaptureCount'],'captures /',report['expectedProcessCount'],'processes; renderer not executed')
    else:run_native(args,args.output,plan,report);print('PASS',len(report['captures']),'animated camera captures')


if __name__=='__main__':main()
