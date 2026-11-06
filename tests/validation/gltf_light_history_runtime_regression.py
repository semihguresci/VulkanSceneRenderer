"""Bounded native light inspector/gizmo history and portable-save regression.

Calibration/generation do not run the renderer. Literal PBR assets are authored
independently; CPU telemetry, complete raster RGB, and the separate Vulkan
descriptor readback suite remain distinct evidence.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
from pathlib import Path
import tempfile

import numpy as np

import gltf_camera_projection_runtime_regression as projection
import gltf_material_binding_runtime_regression as binding
from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, require
from gltf_extended_materials_runtime_regression import sha
from gltf_merge_runtime_regression import document, immutable, portable_copy
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest
from scene_node_authoring_runtime_regression import history_cursor

TECHNIQUES = binding.TECHNIQUES
WIDTH, HEIGHT = 640, 360
LIGHT, AREA, POINTER = 'KHR_lights_punctual', 'EXT_lights_area', 'KHR_animation_pointer'
TYPE = dict(directional=0, point=1, spot=2, area=3)
SOURCE = dict(generated=0, imported=1, manual=2)
NAMES = dict(point='Point owner', spot='Spot owner', directional='Sun owner', area='Area owner', manual='Manual Point 0')
QUADS = [(-1.35, .55, .7, .65), (.25, .55, .65, .65), (1.65, .55, .55, .65), (-.8, -.9, .9, .55), (1.2, -.9, .9, .55)]
COLORS = [[.8,.12,.08,1], [.1,.7,.15,1], [.12,.2,.85,1], [.7,.55,.1,1], [.6,.1,.65,1]]
UNVERIFIED = [
    'Generated inputs and pure calibration are not native renderer acceptance.',
    'CPU light/source/history telemetry is separate from the actual live-descriptor Vulkan probe suite.',
    'Native telemetry checks sampled light/material targets and clock, not every authored/rest record or encoded key byte; separate backend tests cover selected rest preservation.',
    'Complete RGB literal comparisons cover these finite raster fixtures, not every ray/DDGI/provider/BIM configuration.',
    'Typed capture gestures use the same backend; this is not native Windows desktop or mouse acceptance.']


def close(actual, expected, label, tolerance=4e-5):
    a, b = np.asarray(actual, dtype=float), np.asarray(expected, dtype=float)
    require(a.shape == b.shape and np.isfinite(a).all() and np.isfinite(b).all() and
            np.allclose(a, b, rtol=tolerance, atol=tolerance), 'Incorrect light '+label)


def f32(value): return np.asarray(value, dtype=np.float32).astype(float)
def positions(i):
    x,y,w,h = QUADS[i]
    return [[x-w,y-h,0], [x+w,y-h,0], [x+w,y+h,0], [x-w,y+h,0]]


def material(i):
    return dict(name='Surface '+str(i), pbrMetallicRoughness=dict(baseColorFactor=COLORS[i], metallicFactor=0., roughnessFactor=.5),
                doubleSided=True, extras=dict(materialIdentity=100+i))


def base_lights(time=None):
    records = dict(
        point=dict(type=1, source=1, index=0, position=[-1.,1.,3.], direction=[0.,0.,-1.], color=[1.,.2,.1], intensity=18., range=8.),
        spot=dict(type=2, source=1, index=1, position=[1.5,1.,3.], direction=[0.,0.,-1.], color=[.2,1.,.2], intensity=12., range=8., cones=[10.,30.]),
        directional=dict(type=0, source=1, index=0, direction=[0.,0.,-1.], color=[.3,.4,1.], intensity=.8),
        area=dict(type=3, source=1, index=0, position=[0.,-1.,2.5], direction=[0.,0.,-1.], color=[1.,.5,.2], intensity=9., range=8.,
                  halfSize=[1.,.5], shape=0., tangent=[1.,0.,0.], bitangent=[0.,1.,0.]))
    if time is not None: records['point']['intensity'] = 18.+8.*time/2.
    return records


def default_manual():
    all_positions = f32([p for i in range(5) for p in positions(i)])
    minimum, maximum = all_positions.min(axis=0), all_positions.max(axis=0)
    center = f32((minimum+maximum)*.5)
    radius = max(float(np.float32(np.linalg.norm(f32(maximum-minimum))*.5)), 1.)
    return dict(type=1, source=2, index=0, position=f32(center+[0., np.float32(radius*.25), 0.]).tolist(),
                direction=[0.,-1.,0.], color=[1.,.9,.78], intensity=6., range=float(np.float32(radius*1.25)))


def target(kind, *, manual=False):
    return dict(type='point' if kind=='manual' else kind, source='manual', index=0) if manual or kind=='manual' else dict(type=kind, source='imported', nodeName=NAMES[kind])


def edit(kind, values=None, *, phase='commit', gesture=None, gizmo=None, revision=None):
    value = dict(target=target(kind), phase=phase)
    if values is not None: value['values'] = values
    if gizmo is not None: value['gizmo'] = gizmo
    if gesture is not None: value['gesture'] = gesture
    if revision is not None: value['expectedLightRevision'] = revision
    return dict(lightEdit=value)


def state(label, rows=None, *, selected='point', tool=None, axis='free', pubs=0, commits=0, replays=0, gesture=0,
          history=(0,0), clip=-1, time=None, failure=None):
    return dict(label=label, rows=copy.deepcopy(rows or base_lights()), selected=selected, tool=tool, axis=axis,
                publications=pubs, commits=commits, replays=replays, gesture=gesture, history=list(history), clip=clip, time=time,
                failureAs=failure, resolution=[WIDTH,HEIGHT], msaa=1)


def profiles():
    runs = []
    rows=base_lights();steps=[];events=[dict(frame=1, lightSelection=target('point'))]
    def add(value, action=None):
        value['frame']=13*(len(steps)+1);steps.append(value)
        if action: events.append(dict(frame=value['frame']-1, **action))
    add(state('before',rows))
    rows['point']['position'][0]=-.5
    add(state('drag-begin',rows,tool='translate',pubs=1,gesture=101), edit('point',phase='begin',gesture=101,gizmo=dict(tool='translate',amount=[.5,0,0])))
    rows['point']['position'][0]=0.
    add(state('drag-update',rows,tool='translate',pubs=2,gesture=101), edit('point',phase='update',gesture=101,gizmo=dict(tool='translate',amount=[1.,0,0])))
    add(state('drag-end',rows,tool='translate',pubs=2,commits=1,history=(1,0)), edit('point',phase='end',gesture=101))
    old=copy.deepcopy(rows);rows['point']['position'][0]=-1.
    add(state('undo-drag',rows,tool='translate',pubs=3,commits=1,replays=1,history=(0,1)), dict(sceneHistory='undo'))
    rows=old
    add(state('redo-drag',rows,tool='translate',pubs=4,commits=1,replays=2,history=(1,0)), dict(sceneHistory='redo'))
    rows['point']['position'][0]=-.5
    add(state('cancel-preview',rows,tool='translate',pubs=5,commits=1,replays=2,gesture=102,history=(1,0)), edit('point',phase='begin',gesture=102,gizmo=dict(tool='translate',amount=[-.5,0,0])))
    rows['point']['position'][0]=0.
    add(state('cancelled',rows,tool='translate',pubs=6,commits=1,replays=2,history=(1,0)), edit('point',phase='cancel',gesture=102))
    add(state('stale-rejected',rows,tool='translate',pubs=6,commits=1,replays=2,history=(1,0),failure='cancelled'),
        {**edit('point',dict(intensity=99),revision=9007199254740991), 'expectLightEditFailure':True})
    rows['spot']['direction']=[-.5,0.,-math.sqrt(3)/2]
    add(state('spot-rotated',rows,selected='spot',tool='rotate',axis='y',pubs=7,commits=2,replays=2,history=(2,0)),
        edit('spot',gizmo=dict(tool='rotate',amount=[0.,30.,0.],axis='y',space='world')))
    rows['area'].update(halfSize=[2.,1.],intensity=20.)
    add(state('area-numeric',rows,selected='area',tool='rotate',axis='y',pubs=8,commits=3,replays=2,history=(3,0)),edit('area',dict(areaSize=[4.,2.],intensity=20.)))
    rows['area'].update(halfSize=[3.,1.5],range=12.)
    add(state('area-scaled',rows,selected='area',tool='scale',pubs=9,commits=4,replays=2,history=(4,0)),edit('area',gizmo=dict(tool='scale',amount=[.5,.5,.5])))
    rows['directional'].update(color=[.15,.2,1.],intensity=1.7)
    add(state('sun-numeric',rows,selected='directional',tool='scale',pubs=10,commits=5,replays=2,history=(5,0)),edit('directional',dict(color=[.15,.2,1.],intensity=1.7)))
    rows['directional']=base_lights()['directional']
    add(state('sun-undone',rows,selected='directional',tool='scale',pubs=11,commits=5,replays=3,history=(4,1)),dict(sceneHistory='undo'))
    add(state('missing-rejected',rows,selected='directional',tool='scale',pubs=11,commits=5,replays=3,history=(4,1),failure='sun-undone'),
        dict(lightSelection=dict(type='point',source='imported',nodeName='Absent light'),expectLightEditFailure=True))
    rows['directional'].update(color=[.15,.2,1.],intensity=1.7)
    add(state('sun-redone',rows,selected='directional',tool='scale',pubs=12,commits=5,replays=4,history=(5,0)),dict(sceneHistory='redo'))
    runs.append(dict(name='imported-gestures',mode='static',steps=steps,events=events,save='gltf'))
    rows=base_lights(.5);steps=[];events=[dict(frame=1,lightSelection=target('point'))]
    add(state('before',rows,clip=0,time=.5))
    rows['point']['intensity']=35.
    add(state('override',rows,pubs=1,commits=1,history=(1,0),clip=0,time=.5),edit('point',dict(intensity=35.)))
    add(state('seek',rows,pubs=1,commits=1,history=(1,0),clip=0,time=1.5),dict(animationTime=1.5))
    rows['point']['intensity']=24.
    add(state('undo-today',rows,pubs=2,commits=1,replays=1,history=(0,1),clip=0,time=1.5),dict(sceneHistory='undo'))
    add(state('wrong-node-rejected',rows,pubs=2,commits=1,replays=1,history=(0,1),clip=0,time=1.5,failure='undo-today'),
        dict(lightSelection=dict(type='point',source='imported',sourceNode=2147483647),expectLightEditFailure=True))
    rows['point']['intensity']=35.
    add(state('redo-today',rows,pubs=3,commits=1,replays=2,history=(1,0),clip=0,time=1.5),dict(sceneHistory='redo'))
    add(state('clear-clip',rows,pubs=3,commits=1,replays=2,history=(1,0)),dict(animationClip=-1))
    rows['point']['intensity']=18.
    add(state('undo-rest',rows,pubs=4,commits=1,replays=3,history=(0,1)),dict(sceneHistory='undo'))
    rows['point']['intensity']=35.
    add(state('redo-rest',rows,pubs=5,commits=1,replays=4,history=(1,0)),dict(sceneHistory='redo'))
    runs.append(dict(name='animated-current-clock',mode='animated',steps=steps,events=events))
    rows=base_lights();steps=[];events=[dict(frame=1,lightSelection=target('point'))]
    add(state('before',rows))
    rows['manual']=default_manual()
    add(state('added',rows,selected='manual',pubs=1,commits=1,history=(1,0)),dict(lightAdd=dict(type='point')))
    rows['manual'].update(position=[0.,1.,2.],color=[1.,.8,.2],intensity=12.,range=5.)
    add(state('edited',rows,selected='manual',pubs=2,commits=2,history=(2,0)),edit('manual',dict(position=[0.,1.,2.],color=[1.,.8,.2],intensity=12.,range=5.)))
    rows['manual']=default_manual()
    add(state('undo-edit',rows,selected='manual',pubs=3,commits=2,replays=1,history=(1,1)),dict(sceneHistory='undo'))
    del rows['manual']
    add(state('undo-add',rows,selected='point',pubs=4,commits=2,replays=2,history=(0,2)),dict(sceneHistory='undo'))
    rows['manual']=default_manual()
    add(state('redo-add',rows,selected='manual',pubs=5,commits=2,replays=3,history=(1,1)),dict(sceneHistory='redo'))
    rows['manual'].update(position=[0.,1.,2.],color=[1.,.8,.2],intensity=12.,range=5.)
    add(state('redo-edit',rows,selected='manual',pubs=6,commits=2,replays=4,history=(2,0)),dict(sceneHistory='redo'))
    runs.append(dict(name='manual-original-birth',mode='static',steps=steps,events=events,save='glb'))
    return runs


def rotation(direction):
    direction=np.asarray(direction,dtype=float);direction/=np.linalg.norm(direction);initial=np.array([0.,0.,-1.])
    cross=np.cross(initial,direction);dot=float(np.dot(initial,direction))
    if dot>1-1e-12: return [0.,0.,0.,1.]
    require(dot>-1+1e-12,'Fixture antipodal direction not supported')
    q=np.r_[cross,1+dot];return (q/np.linalg.norm(q)).tolist()


def write_fixture(path, rows=None, *, animated=False, roughness=.5):
    rows=copy.deepcopy(rows or base_lights());asset=Asset();model=asset.model
    model['materials']=[material(i) for i in range(5)];model['meshes']=[];model['nodes']=[]
    model['materials'][0]['pbrMetallicRoughness']['roughnessFactor']=roughness
    for i in range(5):
        attrs=dict(POSITION=asset.accessor('VEC3',positions(i)),NORMAL=asset.accessor('VEC3',[[0,0,1]]*4),TANGENT=asset.accessor('VEC4',[[1,0,0,1]]*4))
        model['meshes'].append(dict(name='Geometry '+str(i),extras=dict(geometryIdentity=i),primitives=[dict(attributes=attrs,
            indices=asset.accessor('SCALAR',projection.INDICES,5123,indices=True),material=i,mode=4)]))
        model['nodes'].append(dict(name='Surface '+str(i),mesh=i,extras=dict(ownerIdentity=100+i)))
    punctual=[];areas=[]
    for kind,row in rows.items():
        node=dict(name=NAMES[kind],extras=dict(lightOwner=kind),translation=row.get('position',[0,0,0]),rotation=rotation(row['direction']))
        if kind=='spot' and row['direction'][0]!=0 and row['direction'][1]==0:
            # Literal Y-axis rotation is authored directly as the analytical
            # float basis, avoiding an unrelated quaternion round trip.
            d=f32(row['direction']);matrix=np.eye(4);matrix[:3,0]=[-d[2],0,d[0]];matrix[:3,2]=-d;matrix[:3,3]=row['position']
            node.pop('translation');node.pop('rotation');node['matrix']=matrix.T.reshape(-1).tolist()
        definition=dict(name='Definition '+kind,color=row['color'],intensity=row['intensity'],extras=dict(lightIdentity=kind))
        if kind=='area':
            definition.update(type='rect',width=row['halfSize'][0]*2,height=row['halfSize'][1]*2,range=row['range'])
            node['extensions']={AREA:dict(light=len(areas))};areas.append(definition)
        else:
            definition['type']='point' if kind=='manual' else kind
            if kind!='directional': definition['range']=row['range']
            if kind=='spot': definition['spot']=dict(innerConeAngle=math.radians(row['cones'][0]),outerConeAngle=math.radians(row['cones'][1]))
            node['extensions']={LIGHT:dict(light=len(punctual))};punctual.append(definition)
        model['nodes'].append(node)
    model['extensions']={LIGHT:dict(lights=punctual),AREA:dict(lights=areas)};model['extensionsUsed']=[LIGHT,AREA]
    model['scenes']=[dict(name='Light editing',nodes=list(range(len(model['nodes']))))];model['scene']=0
    if animated:
        times=asset.accessor('SCALAR',[0.,2.]);intensity=asset.accessor('SCALAR',[18.,26.]);rough=asset.accessor('SCALAR',[.2,.8])
        model['animations']=[dict(name='Light plus unrelated material',extras=dict(curveIdentity=31),
            samplers=[dict(input=times,output=intensity,interpolation='LINEAR'),dict(input=times,output=rough,interpolation='LINEAR')],
            channels=[dict(sampler=0,target=dict(path='pointer',extensions={POINTER:dict(pointer='/extensions/KHR_lights_punctual/lights/0/intensity')})),
                      dict(sampler=1,target=dict(path='pointer',extensions={POINTER:dict(pointer='/materials/0/pbrMetallicRoughness/roughnessFactor')}))])]
        model['extensionsUsed'].append(POINTER)
    path.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='light-history-fixture-') as directory:
        temporary=Path(directory)/'scene.gltf';asset.write(temporary);immutable(path,temporary.read_bytes())


def presentation_events(expected, *, reload=False):
    events=[];frame=1
    def selector(kind):
        result=target(kind)
        if kind=='manual':
            return dict(type='point',source='imported',nodeName=NAMES['manual'])
        if reload and expected['label']=='sun-redone':
            # The imported-gestures export retains the owners and binds edited
            # definitions to their carrier children. Select the actual binding.
            result['nodeName']=dict(point='Imported Point 0',spot='Imported Spot 1',
                                    directional='Imported Directional',area='Imported Area 0')[kind]
        return result
    if expected['tool']:
        compatible=dict(translate='point',rotate='spot',scale='area')[expected['tool']]
        action=edit(compatible,gizmo=dict(tool=expected['tool'],axis=expected['axis'],amount=[0,0,0]))
        action['lightEdit']['target']=selector(compatible)
        events.append(dict(frame=frame,**action));frame+=1
    selected=expected['selected']
    if selected is None: events.append(dict(frame=frame,gltfScene=0))
    else:
        events.append(dict(frame=frame,lightSelection=selector(selected)))
    return events


def sampled_roughness(value): return .2+.6*value['time']/2. if value['clip']==0 else .5


def visual_key(value): return json.dumps(dict(rows=value['rows'],selected=value['selected'],tool=value['tool'],axis=value['axis'],roughness=sampled_roughness(value)),sort_keys=True)


def generate(output):
    output.mkdir(parents=True,exist_ok=True);sources={}
    for mode in ('static','animated'):
        sources[mode]=output/'sources'/(mode+'.gltf');write_fixture(sources[mode],animated=mode=='animated')
    controls=[];by_key={};runs=profiles();inputs=list(sources.values())
    for run in runs:
        for expected in run['steps']:
            key=visual_key(expected)
            if key not in by_key:
                name='literal-'+str(len(controls));source=output/'sources'/(name+'.gltf');write_fixture(source,expected['rows'],roughness=sampled_roughness(expected))
                script=output/'scripts'/(name+'.json');control=copy.deepcopy(expected);control.update(label='control',frame=13)
                document(script,dict(schemaVersion=1,frames=13,sampleFrames=[13],events=presentation_events(control)))
                controls.append(dict(name=name,source=str(source),script=str(script),step=control));inputs.extend([source,script]);by_key[key]=name
            expected['control']=by_key[key]
        script=output/'scripts'/(run['name']+'.json');document(script,dict(schemaVersion=1,frames=run['steps'][-1]['frame'],sampleFrames=[s['frame'] for s in run['steps']],events=run['events']))
        run['script']=str(script);inputs.append(script)
        if 'save' in run:
            script=output/'scripts'/(run['name']+'-reload.json');document(script,dict(schemaVersion=1,frames=13,sampleFrames=[13],events=presentation_events(run['steps'][-1],reload=True)))
            run['reloadScript']=str(script);inputs.append(script)
    captures=len(controls)+sum(len(r['steps'])+int('save'in r) for r in runs)
    exports=sum('save'in r for r in runs)
    plan=dict(schemaVersion=1,sources={k:str(v) for k,v in sources.items()},controls=controls,runs=runs,
        expectedCaptureCount=captures,expectedProcessCount=len(controls)+len(runs)+exports,
        expectedExportCount=exports,expectedImageCheckCount=captures+sum(bool(s['failureAs']) for r in runs for s in r['steps'])+8,
        immutableInputs=[dict(path=str(p),byteLength=p.stat().st_size,sha256=sha(p)) for p in inputs])
    require(captures*2<=160,'Light native capture plan exceeded bounded inventory');document(output/'launch-plan.json',plan);return plan


def check_state(value, expected, *, literal=False, reload=False):
    require(finite_json(value),'Nonfinite light telemetry');out=value['lightEdits']
    require(out['truncated'] is False and type(out['revision'])is int and out['revision']>0,'Missing complete light authority telemetry')
    if not literal and not reload:
        for key in ('publications','commits','replays','gesture'): require(out[key]==expected[key],'Wrong light '+key+' count')
        history_cursor(value,*expected['history'],allow_uncreated=True)
    rows=out['lights'];require(len(rows)==len(expected['rows']),'Unexpected live light count')
    selected=[]
    for kind,wanted in expected['rows'].items():
        source=1 if (literal or reload) and kind=='manual' else wanted['source']
        index=2 if source==1 and kind=='manual' else wanted['index']
        matches=[r for r in rows if (r['type'],r['source'],r['index'])==(wanted['type'],source,index)]
        require(len(matches)==1,'Missing/duplicate logical light '+kind);row=matches[0]
        for field in ('position','direction','color','intensity','range','cones','halfSize','shape','tangent','bitangent'):
            # A glTF point light has no emission direction. Its imported
            # placeholder can differ from the in-session manual payload.
            if field=='direction' and kind=='manual' and (literal or reload): continue
            if field in wanted: close(row[field],wanted[field],kind+'/'+field)
        if row['selected']: selected.append(kind)
    require(selected==([]if expected['selected']is None else [expected['selected']]),'Wrong selected light/outline context')
    require(value['lighting']['pointLightCount']==2+int('manual'in expected['rows']) and value['lighting']['areaLightCount']==1,'Wrong actual renderer light counts')
    if not literal and not reload:
        animation=value['gltfAnimation'];require(animation['selectedClip']==expected['clip'],'Wrong selected light animation clip')
        if expected['clip']==0:
            close(animation['time'],expected['time'],'current animation clock',1e-9);require(not animation['playing'],'Light native clock must stay paused')
            material_rows=[r for r in animation['materials']if r['sourceIndex']==0];require(len(material_rows)==1,'Missing unrelated animated material')
            close(material_rows[0]['roughness'],.2+.6*expected['time']/2,'unrelated material at current clock')
            target_rows=[r for r in animation['targets']if r.get('property')=='intensity'];require(len(target_rows)==1,'Missing raw sampled light target')
            close(target_rows[0]['value'],[18.+8.*expected['time']/2.],'sampled source independent of override')
    return dict(passed=True,evidence='CPU light payload/selection/history and current clock; not GPU buffer proof')


def failure_signature(value):
    result=projection.failure_signature(value);result['lightEdits']=copy.deepcopy(value['lightEdits']);result['lightEdits'].pop('error',None)
    result['lighting']={key:value['lighting'][key]for key in ('pointLightCount','areaLightCount','environmentIntensity','directionalIntensity')}
    result['camera']={key:value['camera'][key]for key in ('position','unjitteredViewProjColumns')};return result


def worlds(model):
    nodes=model['nodes'];result={};parents={};active=set()
    require(len(nodes)<=40,'Portable light node inventory is unbounded')
    for node in nodes:
        require(not('matrix'in node and any(k in node for k in ('translation','rotation','scale'))),'Portable node mixes matrix and TRS')
        for field,count in [('matrix',16),('translation',3),('rotation',4),('scale',3)]:
            if field not in node:continue
            data=node[field];require(isinstance(data,list)and len(data)==count and all(type(x)in(int,float)and math.isfinite(x)for x in data),'Malformed portable node '+field)
        if'matrix'in node:require([node['matrix'][i]for i in (3,7,11,15)]==[0,0,0,1],'Portable raw affine row is invalid')
    def visit(index,parent,world):
        require(type(index)is int and 0<=index<len(nodes) and index not in active and index not in result,'Portable hierarchy alias/cycle/index')
        active.add(index);node=nodes[index]
        if 'matrix'in node: require(len(node['matrix'])==16 and [node['matrix'][i]for i in (3,7,11,15)]==[0,0,0,1],'Portable raw affine row is invalid')
        result[index]=world@projection.local_matrix(node);parents[index]=parent
        for child in node.get('children',[]): visit(child,index,result[index])
        active.remove(index)
    scene=model.get('scene',0);require(0<=scene<len(model['scenes']),'Invalid active portable scene')
    for root in model['scenes'][scene]['nodes']:visit(root,-1,np.eye(4))
    return result,parents


def inspect_export(path, expected):
    require(path.stat().st_size<=16*1024*1024,'Portable light model is unbounded');model,buffers=load_asset(path)
    require(finite_json(model) and sum(map(len,buffers))<=16*1024*1024,'Invalid portable binary/model');world,parents=worlds(model);nodes=model['nodes']
    require(len(nodes)<=40 and len(model['scenes'])==1 and len(model['meshes'])==5,'Portable scene/geometry inventory changed')
    require(all(not model.get(table)for table in ('images','textures','samplers','skins','cameras')),'Portable light fixture acquired unrelated resource owners')
    for index in range(len(model.get('accessors',[]))):require(np.isfinite(np.asarray(decode_accessor(path,index),dtype=float)).all(),'Invalid portable accessor/reference')
    require(all('camera'not in node and 'skin'not in node for node in nodes),'Portable light fixture acquired camera/skin associations')
    known=set();materials=set()
    for i in range(5):
        owners=[j for j,n in enumerate(nodes)if n.get('name')=='Surface '+str(i)];require(len(owners)==1,'Portable surface owner identity changed')
        owner=owners[0];node=nodes[owner];known.add(owner);require(node.get('extras')==dict(ownerIdentity=100+i) and parents.get(owner)==-1,'Portable surface metadata/parent changed')
        close(world[owner],np.eye(4),'surface world',0.)
        mesh_node=owner
        if 'mesh'not in node:
            require(len(node.get('children',[]))==1,'Portable primitive carrier count changed');mesh_node=node['children'][0];known.add(mesh_node)
            child=nodes[mesh_node];require(child.get('name')=='Primitive '+str(i) and parents[mesh_node]==owner and not child.get('children'),'Portable primitive identity/parent changed')
            close(projection.local_matrix(child),np.eye(4),'primitive local',0.)
        else:require(not node.get('children'),'Flat portable primitive acquired children')
        mesh_id=nodes[mesh_node]['mesh'];require(type(mesh_id)is int and 0<=mesh_id<5,'Invalid portable mesh index');mesh=model['meshes'][mesh_id]
        require(mesh.get('extras')==dict(geometryIdentity=i)and len(mesh['primitives'])==1,'Portable geometry metadata changed');primitive=mesh['primitives'][0]
        require(primitive.get('mode',4)==4,'Portable primitive mode changed')
        for attr,wanted in [('POSITION',f32(positions(i))),('NORMAL',[[0,0,1]]*4),('TANGENT',[[1,0,0,1]]*4)]:close(decode_accessor(path,primitive['attributes'][attr]),wanted,'portable '+attr,0.)
        close(np.asarray(decode_accessor(path,primitive['indices'])).reshape(-1),projection.INDICES,'portable indices',0.)
        material_id=primitive['material'];require(0<=material_id<len(model['materials']),'Invalid portable material');materials.add(material_id)
        binding.deep_contains(model['materials'][material_id],material(i),'portable semantic material')
    require(len(materials)==5,'Portable materials were merged/rebound')
    punctual=model.get('extensions',{}).get(LIGHT,{}).get('lights',[]);area_defs=model.get('extensions',{}).get(AREA,{}).get('lights',[])
    active=[];referenced=set()
    for index,matrix in world.items():
        node=nodes[index];extensions=node.get('extensions',{});associations=[(dialect,extensions[dialect]['light'])for dialect in (LIGHT,AREA)if dialect in extensions]
        require(len(associations)<=1,'Portable light has ambiguous associations')
        if not associations:continue
        dialect,definition=associations[0];table=punctual if dialect==LIGHT else area_defs
        require(type(definition)is int and 0<=definition<len(table),'Invalid portable light reference');raw=table[definition];referenced.add((dialect,definition))
        metadata=raw.get('extensions',{}).get('KHR_lights_area');is_area=dialect==AREA or metadata is not None
        kind='area'if is_area else raw['type'];record=dict(type=TYPE[kind],direction=(-matrix[:3,2]/np.linalg.norm(matrix[:3,2])).tolist(),
            color=raw.get('color',[1,1,1]),intensity=raw.get('intensity',1))
        if kind!='directional':record.update(position=matrix[:3,3].tolist(),range=(metadata or raw).get('range',raw.get('range',0)))
        if kind=='spot':record['cones']=[math.degrees(raw.get('spot',{}).get(k,default))for k,default in [('innerConeAngle',0),('outerConeAngle',math.pi/4)]]
        if is_area:
            area=metadata or raw;record['halfSize']=[area['width']/2,area['height']/2];record['shape']=1. if area.get('shape',area.get('type'))=='disk'else 0.
        active.append((index,record))
    require(len(active)==len(expected['rows']),'Portable active light count changed');matched=set()
    for kind,wanted in expected['rows'].items():
        owner_name=NAMES[kind];owners=[j for j,n in enumerate(nodes)if n.get('name')==owner_name];require(len(owners)==1,'Portable light owner name changed')
        owner=owners[0];known.add(owner)
        candidates=[(j,r)for j,r in active if j==owner or parents[j]==owner]
        require(len(candidates)==1,'Portable light owner association changed');index,record=candidates[0];matched.add(index);known.add(index)
        if kind!='manual':
            require(nodes[owner].get('extras')==dict(lightOwner=kind) and parents[owner]==-1,'Portable authored light owner metadata/parent changed')
            original=base_lights()[kind];wanted_local=dict(translation=original.get('position',[0,0,0]),rotation=rotation(original['direction']))
            close(projection.local_matrix(nodes[owner]),projection.local_matrix(wanted_local),'portable original light owner local',0.)
            require(nodes[owner].get('children',[])==([]if index==owner else [index]),'Portable light carrier count changed')
            if index!=owner:
                label='Imported '+dict(point='Point 0',spot='Spot 1',directional='Directional',area='Area 0')[kind]
                require(nodes[index].get('name')==label and not nodes[index].get('children') and 'mesh'not in nodes[index]and 'camera'not in nodes[index],'Portable light carrier identity changed')
        else:require(parents[owner]==-1 and not nodes[owner].get('children')and 'mesh'not in nodes[owner],'Portable manual original birth hierarchy changed')
        require(record['type']==wanted['type'],'Portable light type changed')
        for field in ('position','direction','color','intensity','range','cones','halfSize','shape'):
            if field in wanted:close(record[field],wanted[field],kind+'/portable/'+field)
    require(matched=={j for j,_ in active},'Portable light active owner was duplicated')
    unused=[j for j in range(len(nodes))if j not in known];require(len(unused)<=1,'Extra unvisited editor roots')
    for j in unused:
        node=nodes[j];require(node.get('name')=='Scene Root' and j not in world and set(node)<=set(('name','matrix','translation','rotation','scale','children')) and not node.get('children'),'Unexpected portable node')
        close(projection.local_matrix(node),np.eye(4),'unused editor root local',0.)
    # Every extra punctual definition must be the retained original source,
    # never an arbitrary unreferenced resource hidden beside the carriers.
    original=base_lights()
    for i,kind in enumerate(('point','spot','directional')):
        require(i<len(punctual),'Portable original source light missing');raw=punctual[i]
        require(raw.get('name')=='Definition '+kind and raw.get('extras')==dict(lightIdentity=kind)and raw.get('type')==kind,'Portable original definition metadata changed')
        close(raw.get('color',[1,1,1]),original[kind]['color'],'retained source color');close(raw.get('intensity',1),original[kind]['intensity'],'retained source intensity')
        if kind!='directional':close(raw.get('range',0),original[kind]['range'],'retained source range')
        if kind=='spot':close([raw['spot']['innerConeAngle'],raw['spot']['outerConeAngle']],np.radians(original[kind]['cones']),'retained spot cones')
    require(all((LIGHT,i)in referenced or i<3 for i in range(len(punctual)))and len(area_defs)<=1,'Unreferenced portable light definition')
    require(len(area_defs)==1 and area_defs[0].get('name')=='Definition area'and area_defs[0].get('extras')==dict(lightIdentity='area')and area_defs[0].get('type')=='rect','Original area definition metadata lost')
    for field,wanted in [('color',original['area']['color']),('intensity',9.),('range',8.),('width',2.),('height',1.)]:close(area_defs[0][field],wanted,'retained area '+field)
    require(not model.get('animations'),'Static light export acquired animation channels')
    return dict(path=str(path),sha256=sha(path),passed=True,geometryCount=5,activeLightCount=len(active),evidence='Raw portable metadata/hierarchy/all used resource references; reload rendering checked separately')


def synthetic_state(expected):
    rows=[]
    for kind,wanted in expected['rows'].items():
        row=copy.deepcopy(wanted);row['selected']=kind==expected['selected'];rows.append(row)
    animation=dict(selectedClip=expected['clip'],time=expected['time']or 0.,playing=False,materials=[],targets=[])
    if expected['clip']==0:
        animation['materials']=[dict(sourceIndex=0,roughness=sampled_roughness(expected))]
        animation['targets']=[dict(property='intensity',value=[18.+8.*expected['time']/2.])]
    out=dict(revision=17,truncated=False,lights=rows,error='')
    for key in ('publications','commits','replays','gesture'):out[key]=expected[key]
    return dict(lightEdits=out,lighting=dict(pointLightCount=2+int('manual'in expected['rows']),areaLightCount=1),gltfAnimation=animation,
                gltfMaterialVariants=dict(undoCount=expected['history'][0],redoCount=expected['history'][1]))


def write_export_oracle(path,expected):
    """Independent retained-original plus exact child-carrier serialization."""
    original=path.with_name(path.stem+'-raw.gltf');write_fixture(original);model,_=load_asset(original)
    rows=base_lights();definitions=model['extensions'][LIGHT]['lights'];known=dict(point=5,spot=6,directional=7,area=8)
    for kind,wanted in expected['rows'].items():
        if kind in rows and wanted==rows[kind]:continue
        if kind=='area':
            raw=copy.deepcopy(model['extensions'][AREA]['lights'][0]);raw['type']='point'
            raw['extensions']={'KHR_lights_area':dict(shape='rectangle',width=wanted['halfSize'][0]*2,height=wanted['halfSize'][1]*2,range=wanted['range'])}
            for key in ('width','height','range'):raw.pop(key,None)
        elif kind=='manual':raw=dict(name=NAMES[kind],type='point')
        else:raw=copy.deepcopy(definitions[dict(point=0,spot=1,directional=2)[kind]])
        raw.update(color=wanted['color'],intensity=wanted['intensity'])
        if kind!='directional':raw['range']=wanted['range']
        if kind=='spot':raw['spot']=dict(innerConeAngle=math.radians(wanted['cones'][0]),outerConeAngle=math.radians(wanted['cones'][1]))
        desired=projection.local_matrix(dict(translation=wanted.get('position',[0,0,0]),rotation=rotation(wanted['direction'])))
        label=NAMES['manual']if kind=='manual'else 'Imported '+dict(point='Point 0',spot='Spot 1',directional='Directional',area='Area 0')[kind]
        if kind!='manual':
            owner=known[kind];local=np.linalg.inv(projection.local_matrix(model['nodes'][owner]))@desired
            model['nodes'][owner]['extensions'].pop(AREA if kind=='area'else LIGHT)
            model['nodes'][owner]['children']=[len(model['nodes'])]
        else:local=desired;model['scenes'][0]['nodes'].append(len(model['nodes']))
        local[3]=[0,0,0,1]
        model['nodes'].append(dict(name=label,matrix=local.T.reshape(-1).tolist(),extensions={LIGHT:dict(light=len(definitions))}));definitions.append(raw)
    # The established exporter keeps source owners and puts canonical
    # identity primitive children underneath them. Both forms must validate.
    for i in range(5):
        mesh=model['nodes'][i].pop('mesh');model['nodes'][i]['children']=[len(model['nodes'])]
        model['nodes'].append(dict(name='Primitive '+str(i),mesh=mesh))
    document(path,model)


def dependencies():
    result=dict(projection.dependencies());result[str(Path(__file__).resolve())]=sha(Path(__file__))
    schema=Path(__file__).resolve().parents[1]/'fixtures/rendering/temporal_capture.schema.json';result[str(schema)]=sha(schema)
    return dict(sorted(result.items()))


def visible(record):
    pixels=projection.pixels(record);require(np.isfinite(pixels).all(),'Invalid literal light pixels')
    count=int(np.count_nonzero(pixels.max(axis=2)>15));spread=float(pixels.std())
    require(count>10000 and spread>8,'Literal light fixture is blank/unlit/constant');return dict(passed=True,nonblackPixels=count,standardDeviation=spread,evidence='Positive actual native PBR visibility')


def changed(a,b):
    pixels=np.abs(projection.pixels(a)-projection.pixels(b));count=int(np.count_nonzero(pixels.max(axis=2)>5));mean=float(pixels.mean())
    require(count>250 and mean>.1,'Positive light illumination witness is vacuous');return dict(passed=True,changedPixels=count,meanByteDelta=mean,evidence='Nonvacuous native light illumination change')


def run_native(args,output,plan,report):
    require(args.renderer.is_file()and args.manifest.is_file(),'Missing light renderer/frozen manifest')
    require(not any(output.glob('captures/*'))and not any(output.glob('exports/**/*'))and not any(output.glob('portable/**/*')),'Native light evidence must be fresh')
    if(output/'results.json').exists():
        old=json.loads((output/'results.json').read_text());require(old.get('generatedOnly')and not old.get('rendererExecuted'),'Refusing to overwrite native light evidence')
    manifest_hash=sha(args.manifest)
    def persist():
        for key in ('captures','processes','stateChecks','imageChecks','exports'):report[key+'Count']=len(report[key])
        (output/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    def runtime():return sorted([*args.renderer.parent.glob('*.dll'),*(args.renderer.parent/'spv_shaders').glob('*.spv')])
    def verify():
        require(sha(args.manifest)==manifest_hash and dependencies()==report['sourceDependencies'],'Frozen light helper/manifest changed')
        result=verify_manifest(args.manifest);require(sha(output/'launch-plan.json')==report['launchPlanSha256'],'Light launch plan changed')
        require((output/'vk_layer_settings.txt').read_bytes()==b'khronos_validation.validate_sync = true\n','Light sync validation settings changed')
        for row in plan['immutableInputs']:
            path=Path(row['path']);require(path.stat().st_size==row['byteLength']and sha(path)==row['sha256'],'Light fixture/script changed')
        if'runtimeInputs'in report:require([str(p.resolve())for p in runtime()]==report['runtimeInputs'],'Light resident runtime inventory changed')
        for copy_record in report['portableCopies']:require(all(sha(Path(row['path']))==row['sha256']for row in copy_record['files']),'Portable light evidence changed')
        return result
    def checked(kind,technique,name,result):report[kind].append(dict(technique=technique,check=name,**result));persist()
    try:
        report['nativeManifestBefore']=verify_manifest(args.manifest);frozen={Path(p).resolve()for p in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
        resident=runtime();require(any(p.suffix=='.spv'for p in resident),'Missing native shader inputs')
        require({args.renderer,*map(Path,report['sourceDependencies']),*map(Path.resolve,resident)}<=frozen,'Frozen manifest omits transitive light inputs')
        report.update(rendererSha256=sha(args.renderer),runtimeInputs=[str(p.resolve())for p in resident]);immutable(output/'vk_layer_settings.txt',b'khronos_validation.validate_sync = true\n');persist();verify()
        for technique in args.technique:
            controls={}
            for control in plan['controls']:
                expected=control['step'];record=binding.launch(args,output,report,persist,verify,technique,control['name'],Path(control['source']),[expected],script=Path(control['script']),initial=expected)['control']
                checked('stateChecks',technique,control['name'],check_state(record['state'],expected,literal=True));checked('imageChecks',technique,control['name']+'-visible',visible(record));controls[control['name']]=record
            cohorts={}
            for run in plan['runs']:
                save=output/'exports'/technique/run['name']/('scene.'+run['save'])if'save'in run else None
                records=binding.launch(args,output,report,persist,verify,technique,run['name'],Path(plan['sources'][run['mode']]),run['steps'],script=Path(run['script']),save=save,animation_time=.5 if run['mode']=='animated'else None)
                cohorts[run['name']]=records
                for expected in run['steps']:
                    record=records[expected['label']];checked('stateChecks',technique,run['name']+'/'+expected['label'],check_state(record['state'],expected))
                    checked('imageChecks',technique,run['name']+'/'+expected['label'],projection.compare_images(record,controls[expected['control']]))
                    if expected['failureAs']:
                        error=record['state']['lightEdits']['error'];require(type(error)is str and 0<len(error)<=503,'Missing explicit bounded light rejection reason')
                        old=records[expected['failureAs']];require(failure_signature(record['state'])==failure_signature(old['state']),'Rejected light action changed source/clock/history/render state')
                        checked('imageChecks',technique,expected['label']+'-unchanged',projection.compare_images(record,old))
                if save:
                    expected=run['steps'][-1];report['exports'].append(dict(technique=technique,**inspect_export(save,expected)));persist()
                    portable,files=portable_copy(save,output/'portable'/technique/run['name']);report['portableCopies'].append(dict(files=files));persist()
                    reloaded=copy.deepcopy(expected);reloaded.update(label='reload',frame=13)
                    record=binding.launch(args,output,report,persist,verify,technique,run['name']+'-reload',portable,[reloaded],script=Path(run['reloadScript']),initial=reloaded)['reload']
                    checked('stateChecks',technique,run['name']+'-reload',check_state(record['state'],reloaded,reload=True));checked('imageChecks',technique,run['name']+'-reload',projection.compare_images(record,records[expected['label']]))
            a,b,c=(cohorts[n]for n in ('imported-gestures','animated-current-clock','manual-original-birth'))
            for name,first,second in [('point-drag',a['before'],a['drag-end']),('spot-rotation',a['cancelled'],a['spot-rotated']),
                ('area-numeric',a['spot-rotated'],a['area-numeric']),('area-scale',a['area-numeric'],a['area-scaled']),('sun-numeric',a['area-scaled'],a['sun-numeric']),
                ('animated-today',b['override'],b['undo-today']),('manual-added',c['before'],c['added']),('manual-edited',c['added'],c['edited'])]:
                checked('imageChecks',technique,name+'-positive',changed(first,second))
        factor=len(args.technique)
        for key in ('Capture','Process','ImageCheck','Export'):
            actual=dict(Capture='captures',Process='processes',ImageCheck='imageChecks',Export='exports')[key]
            require(len(report[actual])==plan['expected'+key+'Count']*factor,'Incomplete light '+key+' inventory')
        require(len(report['stateChecks'])==len(report['captures']),'Incomplete light state inventory');report['nativeManifestAfter']=verify();report.update(passed=True,nativeValidationPerformed=True)
    except Exception as error:report.update(passed=False,failure=str(error));persist();raise
    finally:
        if report['rendererExecuted']:
            try:report['nativeManifestAfter']=verify()
            except Exception as error:report.update(passed=False,afterManifestFailure=str(error));report.setdefault('failure',str(error))
        persist()
    require(report['passed'],'Native light provenance failed after execution')


def self_check():
    import jsonschema
    checks=[]
    def reject(name,callback):
        try:callback()
        except(RuntimeError,jsonschema.ValidationError):checks.append(name);return
        raise RuntimeError('Calibration falsely accepted '+name)
    schema=json.loads((Path(__file__).resolve().parents[1]/'fixtures/rendering/temporal_capture.schema.json').read_text())
    with tempfile.TemporaryDirectory(prefix='light-history-calibration-')as directory:
        output=Path(directory);plan=generate(output)
        for run in plan['runs']:
            script=json.loads(Path(run['script']).read_text());jsonschema.validate(script,schema);checks.append(run['name']+'-typed-schema')
            require(len({s['frame']for s in run['steps']})==len(run['steps']),'Duplicate light capture frame')
            for expected in run['steps']:
                synthetic=synthetic_state(expected);check_state(synthetic,expected);checks.append(run['name']+'/'+expected['label']+'-state')
                for key in ('publications','commits','replays','gesture'):
                    bad=copy.deepcopy(synthetic);bad['lightEdits'][key]+=1
                    reject(run['name']+'/'+expected['label']+'-wrong-'+key,lambda bad=bad,expected=expected:check_state(bad,expected))
                for key in ('undoCount','redoCount'):
                    bad=copy.deepcopy(synthetic);bad['gltfMaterialVariants'][key]+=1
                    reject(run['name']+'/'+expected['label']+'-wrong-'+key,lambda bad=bad,expected=expected:check_state(bad,expected))
                bad=copy.deepcopy(synthetic);bad['lightEdits']['lights'][0]['source']=2
                reject(run['name']+'/'+expected['label']+'-wrong-source',lambda bad=bad,expected=expected:check_state(bad,expected))
                bad=copy.deepcopy(synthetic);bad['lightEdits']['lights'].append(copy.deepcopy(bad['lightEdits']['lights'][0]))
                reject(run['name']+'/'+expected['label']+'-duplicate-light',lambda bad=bad,expected=expected:check_state(bad,expected))
                if expected['clip']==0:
                    for field in ('time','material','source'):
                        bad=copy.deepcopy(synthetic)
                        if field=='time':bad['gltfAnimation']['time']+=.25
                        elif field=='material':bad['gltfAnimation']['materials'][0]['roughness']+=.1
                        else:bad['gltfAnimation']['targets'][0]['value'][0]+=1
                        reject(expected['label']+'-rewound-'+field,lambda bad=bad,expected=expected:check_state(bad,expected))
        for control in plan['controls']:
            jsonschema.validate(json.loads(Path(control['script']).read_text()),schema);checks.append(control['name']+'-presentation-schema')
            expected=control['step'];synthetic=synthetic_state(expected)
            for row in synthetic['lightEdits']['lights']:
                if row['source']==2:row.update(source=1,index=2)
            check_state(synthetic,expected,literal=True);checks.append(control['name']+'-literal-state')
            model,_=load_asset(Path(control['source']));close(model['materials'][0]['pbrMetallicRoughness']['roughnessFactor'],sampled_roughness(expected),'literal current material clock')
        for run in plan['runs']:
            if'save'not in run:continue
            expected=run['steps'][-1];path=output/(run['name']+'.gltf');write_export_oracle(path,expected);inspect_export(path,expected);checks.append(run['name']+'-canonical-export-gltf')
            binary=path.with_suffix('.glb');projection.glb_oracle(path,binary);inspect_export(binary,expected);checks.append(run['name']+'-canonical-export-glb')
            model,_=load_asset(path);world,parents=worlds(model);require(len(world)==len(model['nodes']),'Independent light literal has unreachable nodes');checks.append(run['name']+'-literal-all-references')
            for i in range(5):close(decode_accessor(path,model['meshes'][i]['primitives'][0]['attributes']['POSITION']),f32(positions(i)),'literal geometry',0.);checks.append(run['name']+'-geometry-'+str(i))
            bad=copy.deepcopy(model);bad['nodes'][0]['matrix']=np.eye(4).T.reshape(-1).tolist();bad['nodes'][0]['matrix'][15]=float(np.nextafter(1.,2.))
            reject('one-ulp-affine-'+run['name'],lambda:worlds(bad))
            bad=copy.deepcopy(model);bad['nodes'][0]['children']=[0];reject('cycle-'+run['name'],lambda:worlds(bad))
            def corrupt(name,mutation):
                bad=copy.deepcopy(model);mutation(bad);destination=output/(run['name']+'-'+name+'.gltf');document(destination,bad)
                reject(run['name']+'-'+name,lambda:inspect_export(destination,expected))
            corrupt('geometry-owner-name',lambda m:m['nodes'][0].__setitem__('name','Lost owner'))
            corrupt('geometry-owner-transform',lambda m:m['nodes'][0].__setitem__('translation',[1,0,0]))
            corrupt('geometry-owner-material',lambda m:m['meshes'][0]['primitives'][0].__setitem__('material',1))
            corrupt('source-light-metadata',lambda m:m['extensions'][LIGHT]['lights'][0].__setitem__('extras',dict(lightIdentity='wrong')))
            corrupt('source-light-value',lambda m:m['extensions'][LIGHT]['lights'][0].__setitem__('intensity',99))
            corrupt('source-light-type',lambda m:m['extensions'][LIGHT]['lights'][0].__setitem__('type','spot'))
            corrupt('source-light-range',lambda m:m['extensions'][LIGHT]['lights'][0].__setitem__('range',99))
            corrupt('source-spot-cone',lambda m:m['extensions'][LIGHT]['lights'][1]['spot'].__setitem__('outerConeAngle',.9))
            corrupt('source-area-missing',lambda m:m['extensions'][AREA].__setitem__('lights',[]))
            corrupt('source-area-type',lambda m:m['extensions'][AREA]['lights'][0].__setitem__('type','disk'))
            corrupt('source-area-metadata',lambda m:m['extensions'][AREA]['lights'][0].__setitem__('extras',dict(lightIdentity='other')))
            for field in ('intensity','range','width','height'):
                corrupt('source-area-'+field,lambda m,field=field:m['extensions'][AREA]['lights'][0].__setitem__(field,99))
            corrupt('source-area-color',lambda m:m['extensions'][AREA]['lights'][0].__setitem__('color',[0,0,0]))
            corrupt('extra-unused-definition',lambda m:m['extensions'][LIGHT]['lights'].append(copy.deepcopy(m['extensions'][LIGHT]['lights'][0])))
            corrupt('missing-owner-light',lambda m:next(n for n in m['nodes']if LIGHT in n.get('extensions',{}))['extensions'].pop(LIGHT))
            corrupt('source-owner-moved',lambda m:m['nodes'][5].__setitem__('translation',[3,4,5]))
            corrupt('invalid-light-reference',lambda m:next(n for n in m['nodes']if LIGHT in n.get('extensions',{}))['extensions'][LIGHT].__setitem__('light',2147483647))
            corrupt('extra-animation',lambda m:m.__setitem__('animations',[dict(channels=[],samplers=[])]))
            corrupt('duplicate-unused-roots',lambda m:m['nodes'].extend([dict(name='Scene Root'),dict(name='Scene Root')]))
            corrupt('transformed-unused-root',lambda m:m['nodes'].append(dict(name='Scene Root',translation=[1,0,0])))
        valid=dict(schemaVersion=1,frames=2,events=[dict(frame=1,**edit('point',dict(intensity=5)))])
        for key,value in [('sceneHistory','undo'),('animationTime',.5),('areaLightPosition',[0,0,0]),('lightAdd',dict(type='point')),('unknown',True)]:
            bad=copy.deepcopy(valid);bad['events'][0][key]=value;reject('isolated-'+key,lambda bad=bad:jsonschema.validate(bad,schema))
        for value in (True,None,'1',-1):
            bad=copy.deepcopy(valid);bad['events'][0]['lightEdit']['expectedLightRevision']=value;reject('revision-'+str(value),lambda bad=bad:jsonschema.validate(bad,schema))
        for phase in ('end','cancel'):
            bad=copy.deepcopy(valid);bad['events'][0]['lightEdit'].update(phase=phase,gesture=1);reject('terminal-payload-'+phase,lambda bad=bad:jsonschema.validate(bad,schema))
        for source,selector in [('manual',dict(nodeName='Point owner')),('imported',dict(index=0))]:
            bad=copy.deepcopy(valid);bad['events'][0]['lightEdit']['target']=dict(type='point',source=source,**selector);reject('foreign-domain-'+source,lambda bad=bad:jsonschema.validate(bad,schema))
        require(plan['expectedCaptureCount']*2<=160,'Unbounded light capture cohort');checks.append('bounded-native-plan')
    return dict(passed=True,rendererExecuted=False,nativeValidationPerformed=False,calibrationCount=len(checks),checks=checks,unverified=UNVERIFIED)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path);parser.add_argument('--renderer',type=Path);parser.add_argument('--manifest',type=Path)
    parser.add_argument('--technique',choices=TECHNIQUES,action='append');parser.add_argument('--timeout',type=float,default=600.)
    parser.add_argument('--self-check',action='store_true');parser.add_argument('--generate-only',action='store_true');args=parser.parse_args()
    if args.self_check:print(json.dumps(self_check(),indent=2));return
    if args.output is None:parser.error('--output is required')
    if not math.isfinite(args.timeout)or args.timeout<=0:parser.error('--timeout must be finite and positive')
    if not args.generate_only and(args.renderer is None or args.manifest is None):parser.error('Native light run requires --renderer and --manifest')
    args.technique=tuple(dict.fromkeys(args.technique or TECHNIQUES))
    if args.renderer:args.renderer=args.renderer.resolve()
    if args.manifest:args.manifest=args.manifest.resolve()
    output=args.output.resolve();plan=generate(output)
    report=dict(schemaVersion=1,passed=None if args.generate_only else False,generatedOnly=args.generate_only,rendererExecuted=False,nativeValidationPerformed=False,
        sourceDependencies=dependencies(),launchPlanSha256=sha(output/'launch-plan.json'),processes=[],captures=[],stateChecks=[],imageChecks=[],exports=[],portableCopies=[],unverified=UNVERIFIED)
    for key in ('Capture','Process','ImageCheck','Export'):report['expected'+key+'Count']=plan['expected'+key+'Count']*len(args.technique)
    if args.generate_only:document(output/'results.json',report);print('GENERATED',report['expectedCaptureCount'],'light captures; renderer not executed')
    else:run_native(args,output,plan,report);print('PASS',len(report['captures']),'light captures;',len(report['imageChecks']),'image checks')


if __name__=='__main__':main()
