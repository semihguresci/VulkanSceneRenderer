"""Native arbitrary unused-material retirement, history and export acceptance.

Only --self-check/--generate-only are pure. Native execution requires a frozen
renderer and transitive helper/runtime manifest. CPU catalogs are distinct from
actual rendered pixels and serialized file inspection. No ImGui, BIM or GPU
allocation reclamation acceptance is inferred from this harness.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

import gltf_material_binding_runtime_regression as binding
from gltf_accessor_runtime_regression import Asset, decode_accessor, load_asset, require
from gltf_extended_materials_runtime_regression import sha,validation_log_error
from gltf_material_authoring_runtime_regression import deep_contains, serialized_material_record
from gltf_merge_runtime_regression import document, immutable, portable_copy
from gltf_tangent_uv_runtime_regression import finite_json
from gltf_texture_formats_runtime_regression import verify_manifest
from scene_node_authoring_runtime_regression import history_cursor

TECHNIQUES=binding.TECHNIQUES
COHORTS=('core','history','blocked','portable','runtime','append-history')
UNLIT,POINTER,VARIANTS=binding.UNLIT,binding.POINTER,'KHR_materials_variants'
WIDTH,HEIGHT=binding.WIDTH,binding.HEIGHT
# Export preserves the current SceneNode GLM binary32 pose, promoted to JSON
# doubles. It does not restore the original authored binary64 translation.
CANONICAL_PLACEMENTS=np.asarray([-1.1,1.1],dtype=np.float32).astype(float).tolist()
DEFAULT_TOKEN='__default_test_scene__'
DEFAULT_ASSETS=('models/glTF-Sample-Models/2.0/Triangle/glTF/Triangle.gltf',
    'models/glTF-Sample-Models/2.0/Cube/glTF/Cube.gltf')
COLORS=[binding.RED,[.85,.65,.08,1.],[.5,.08,.75,1.],binding.GREEN]
MATERIAL_NAMES=('Left','Unused middle','Unused later','Right')
END=[.1,.25,.85,1.]
UNVERIFIED=[
 'Pure fixture/self-check results are not native acceptance.',
 'Catalogs, exact phase/incarnation/record proofs and packed hashes are CPU metadata, not GPU readback.',
 'Literal unlit interior pixels establish visible raster preservation/color, not hidden resource reclamation or transport.',
 'Actual material retirement is exercised; image/texture deletion, native ImGui, BIM/provider, ray/DDGI and codec ownership require separate acceptance.',
 'Expected-failure requests use the ordinary backend; success is fatal and actual BEFORE/AFTER catalogs, history and images are compared.']


def materials():
    return [binding.material(name,color) for name,color in zip(MATERIAL_NAMES,COLORS)]


def right_color(time):
    return (np.asarray(COLORS[3])*(1-time)+np.asarray(END)*time).tolist()


def inert():
    return dict(name='',alphaMode='OPAQUE',alphaCutoff=.5,doubleSided=False,emissiveFactor=[0.,0.,0.],
        pbrMetallicRoughness=dict(baseColorFactor=[1.,1.,1.,1.],metallicFactor=1.,roughnessFactor=1.),extensions={})


def spec(label,frame,*,retired=(),rough=.7,later=.7,left=0,history=(0,0),time=None,appended=False,append_source=2,appended_color=None,failure_as=None):
    authored=materials();authored[1]['pbrMetallicRoughness']['roughnessFactor']=rough
    authored[2]['pbrMetallicRoughness']['roughnessFactor']=later
    if appended:
        value=copy.deepcopy(authored[append_source]);value['name']='Append after retirement'
        if appended_color is not None:value['pbrMetallicRoughness']['baseColorFactor']=list(appended_color)
        authored.append(value)
    evaluated=copy.deepcopy(authored)
    if time is not None:evaluated[3]['pbrMetallicRoughness']['baseColorFactor']=right_color(time)
    for row in retired:
        if row>=2 and row-2<len(authored):authored[row-2]=inert();evaluated[row-2]=inert()
    return dict(label=label,frame=frame,retired=list(retired),authored=authored,evaluated=evaluated,left=left,
        history=list(history),time=time,resolution=[WIDTH,HEIGHT],msaa=1,
        failureAs=failure_as)


def retire(frame,runtime=3,source=1,*,expected=None,failure=False):
    request=dict(runtimeMaterial=runtime,sourceMaterial=source)
    if expected is not None:request['expectedAuthoredRevision']=expected
    event=dict(frame=frame,materialRetirement=request)
    if failure:event['expectMaterialRetirementFailure']=True
    return event


def assign(frame,source,runtime,*,expected=2,graph=1,unique=False):
    request=dict(operation='makeUnique' if unique else 'assign',graphNode=graph,expectedRuntimeMaterial=expected,
        sourceMaterial=source,runtimeMaterial=runtime)
    if unique:request['name']='Append after retirement'
    return dict(frame=frame,materialBinding=request)


def roughness(frame,source,value):
    return dict(frame=frame,materialEdit=dict(sourceMaterial=source,values=[dict(property='pbrMetallicRoughness/roughnessFactor',value=value)]))


def profiles(cohort):
    if cohort=='append-history':
        # Four original journal commands. In particular the final redo must
        # reuse the original Delete receipt after the original birth is replayed.
        cyan=[.05,.65,.85,1.]
        born=dict(appended=True,append_source=0,left=4)
        edited=dict(**born,appended_color=cyan)
        unused=dict(appended=True,append_source=0,appended_color=cyan)
        stages=[('before',{},None),
            ('appended',dict(**born,history=(1,0)),assign(14,0,2,unique=True)),
            ('edited',dict(**edited,history=(2,0)),dict(frame=27,materialEdit=dict(sourceMaterial=4,
                values=[dict(property='pbrMetallicRoughness/baseColorFactor',value=cyan)]))),
            ('used-rejected',dict(**edited,history=(2,0),failure_as='edited'),retire(40,6,4,failure=True)),
            ('unbound',dict(**unused,history=(3,0)),assign(53,0,2,expected=6)),
            ('deleted',dict(**unused,retired=(6,),history=(4,0)),retire(66,6,4)),
            ('seek',dict(**unused,retired=(6,),history=(4,0)),dict(frame=79,animationTime=.75)),
            ('undo-delete',dict(**unused,history=(3,1)),dict(frame=92,sceneHistory='undo')),
            ('undo-assign',dict(**edited,history=(2,2)),dict(frame=105,sceneHistory='undo')),
            ('undo-factor',dict(**born,history=(1,3)),dict(frame=118,sceneHistory='undo')),
            ('undo-birth',dict(history=(0,4)),dict(frame=131,sceneHistory='undo')),
            ('redo-birth',dict(**born,history=(1,3)),dict(frame=144,sceneHistory='redo')),
            ('redo-factor',dict(**edited,history=(2,2)),dict(frame=157,sceneHistory='redo')),
            ('redo-assign',dict(**unused,history=(3,1)),dict(frame=170,sceneHistory='redo')),
            ('redo-delete',dict(**unused,retired=(6,),history=(4,0)),dict(frame=183,sceneHistory='redo')),
            ('undo-delete-again',dict(**unused,history=(3,1)),dict(frame=196,sceneHistory='undo')),
            ('redo-delete-again',dict(**unused,retired=(6,),history=(4,0)),dict(frame=209,sceneHistory='redo'))]
        steps=[spec(label,13+13*i,time=.25 if i<6 else .75,**values)
            for i,(label,values,_) in enumerate(stages)]
        events=[event for _,_,event in stages if event]
        return [dict(name='original-birth-delete-'+suffix,source='animated',animated=True,
            steps=copy.deepcopy(steps),events=copy.deepcopy(events),save=suffix,expectedFailures=1)
            for suffix in ('gltf','glb')]
    if cohort=='core':
        return [dict(name='middle-and-append',source='source',steps=[spec('before',13),
            spec('deleted',26,retired=(3,),history=(1,0)),spec('undo',39,history=(0,1)),
            spec('redo',52,retired=(3,),history=(1,0)),spec('assigned',65,retired=(3,),left=2,history=(2,0)),
            spec('appended',78,retired=(3,),left=4,appended=True,history=(3,0)),
            spec('undo-append',91,retired=(3,),left=2,history=(2,1)),
            spec('redo-append',104,retired=(3,),left=4,appended=True,history=(3,0))],
            events=[retire(14),dict(frame=27,sceneHistory='undo'),dict(frame=40,sceneHistory='redo'),
                assign(53,2,4),assign(66,2,4,expected=4,unique=True),dict(frame=79,sceneHistory='undo'),dict(frame=92,sceneHistory='redo')])]
    if cohort=='history':
        stages=[('before',{},None),('factor',dict(rough=.2,history=(1,0)),roughness(14,1,.2)),
            ('bound',dict(rough=.2,left=1,history=(2,0)),assign(27,1,3)),
            ('unbound',dict(rough=.2,history=(3,0)),assign(40,0,2,expected=3)),
            ('deleted',dict(rough=.2,retired=(3,),history=(4,0)),retire(53)),
            ('seek',dict(rough=.2,retired=(3,),history=(4,0)),dict(frame=66,animationTime=.75)),
            ('undo-delete',dict(rough=.2,history=(3,1)),dict(frame=79,sceneHistory='undo')),
            ('undo-binding',dict(rough=.2,left=1,history=(2,2)),dict(frame=92,sceneHistory='undo')),
            ('redo-binding',dict(rough=.2,history=(3,1)),dict(frame=105,sceneHistory='redo')),
            ('redo-delete',dict(rough=.2,retired=(3,),history=(4,0)),dict(frame=118,sceneHistory='redo')),
            ('undo-delete-again',dict(rough=.2,history=(3,1)),dict(frame=131,sceneHistory='undo')),
            ('undo-second-binding',dict(rough=.2,left=1,history=(2,2)),dict(frame=144,sceneHistory='undo')),
            ('undo-first-binding',dict(rough=.2,history=(1,3)),dict(frame=157,sceneHistory='undo')),
            ('undo-factor',dict(history=(0,4)),dict(frame=170,sceneHistory='undo')),
            ('redo-factor',dict(rough=.2,history=(1,3)),dict(frame=183,sceneHistory='redo')),
            ('redo-first-binding',dict(rough=.2,left=1,history=(2,2)),dict(frame=196,sceneHistory='redo')),
            ('redo-second-binding',dict(rough=.2,history=(3,1)),dict(frame=209,sceneHistory='redo')),
            ('redo-delete-final',dict(rough=.2,retired=(3,),history=(4,0)),dict(frame=222,sceneHistory='redo'))]
        steps=[];events=[]
        for index,(label,values,event) in enumerate(stages):
            steps.append(spec(label,13+13*index,time=.25 if index<5 else .75,**values))
            if event:events.append(event)
        return [dict(name='current-clock-older-history',source='animated',animated=True,steps=steps,events=events)]
    if cohort=='blocked':
        steps=[spec('before',13),spec('factor',26,rough=.2,history=(1,0)),spec('undone',39,history=(0,1)),
            spec('used-rejected',52,history=(0,1),failure_as='undone'),spec('stale-rejected',65,history=(0,1),failure_as='undone'),
            spec('pair-rejected',78,history=(0,1),failure_as='undone'),spec('default-rejected',91,history=(0,1),failure_as='undone'),
            spec('redo-preserved',104,rough=.2,history=(1,0))]
        runs=[dict(name='failures-preserve-redo',source='source',steps=steps,
            events=[roughness(14,1,.2),dict(frame=27,sceneHistory='undo'),retire(40,2,0,failure=True),
                retire(53,expected=9007199254740991,failure=True),retire(66,3,2,failure=True),retire(79,0,-1,failure=True),
                dict(frame=92,sceneHistory='redo')],expectedFailures=4)]
        for source in ('variant-blocker','pointer-blocker'):
            runs.append(dict(name=source,source=source,steps=[spec('before',13),spec('rejected',26,failure_as='before')],
                events=[retire(14,failure=True)],expectedFailures=1))
        return runs
    if cohort=='portable':
        return [dict(name='dense-'+suffix,source='animated',animated=True,save=suffix,
            steps=[spec('before',13,time=.25),spec('deleted',26,time=.25,retired=(3,),history=(1,0)),
                spec('unrelated',39,time=.25,retired=(3,),later=.4,history=(2,0))],
            events=[retire(14),roughness(27,2,.4)]) for suffix in ('gltf','glb')]
    if cohort=='runtime':
        stages=[('before',13,False,False,(0,0)),('reassigned',26,True,False,(1,0)),
            ('deleted',39,True,True,(2,0)),('undo',52,True,False,(1,1)),('redo',65,True,True,(2,0))]
        return [dict(name='builtin-unused-runtime',source='builtin',runtimeOnly=True,
            steps=[dict(label=label,frame=frame,bound=bound,retired=retired,history=list(history),resolution=[WIDTH,HEIGHT],msaa=1)
                for label,frame,bound,retired,history in stages],
            events=[assign(14,-1,3,expected=2,graph=3),retire(27,2,-1),dict(frame=40,sceneHistory='undo'),dict(frame=53,sceneHistory='redo')])]
    raise ValueError('Unknown retirement cohort')


def write_fixture(path,*,reference=None,animated=False,blocker=None):
    asset=Asset();asset.model['materials']=copy.deepcopy(reference if reference is not None else materials())
    attrs=dict(POSITION=asset.accessor('VEC3',binding.POSITIONS),NORMAL=asset.accessor('VEC3',[[0.,0.,1.]]*4),
        TANGENT=asset.accessor('VEC4',[[1.,0.,0.,1.]]*4),TEXCOORD_0=asset.accessor('VEC2',[[0.,0.],[1.,0.],[1.,1.],[0.,1.]]))
    primitive=dict(attributes=attrs,indices=asset.accessor('SCALAR',binding.INDICES,5123,indices=True),material=0,mode=4,
        extras=dict(retirementGeometry='exact unchanged topology'))
    right=copy.deepcopy(primitive);right['material']=1 if reference is not None else 3
    asset.model['meshes']=[dict(primitives=[primitive]),dict(primitives=[right])]
    asset.model['nodes']=[dict(name='Left owner',mesh=0,translation=[-1.1,0.,0.]),dict(name='Right owner',mesh=1,translation=[1.1,0.,0.])]
    asset.model['scenes']=[dict(name='Retirement acceptance',nodes=[0,1])]
    asset.model['extensionsUsed']=[UNLIT]
    asset.model['extras']=dict(retirementOpaque=dict(material=1,materials=[1,3],pointer='/materials/1/not-authority'))
    if reference is None:
        asset.model['extensionsUsed'].append(VARIANTS)
        asset.model['extensions']={VARIANTS:dict(variants=[dict(name='Surviving right')])}
        primitive['extensions']={VARIANTS:dict(mappings=[dict(material=1 if blocker=='variant' else 3,variants=[0],
            extras=dict(material=1,opaque=True))])}
    if animated or blocker=='pointer':
        times=asset.accessor('SCALAR',[0.,1.]);values=asset.accessor('VEC4',[COLORS[3],END])
        pointer='/materials/1/extensions/FUTURE_property/unsupported~1suffix' if blocker=='pointer' else '/materials/3/pbrMetallicRoughness/baseColorFactor'
        asset.model['animations']=[dict(name='Retained right curve',extras=dict(retirementCurve='unchanged'),
            samplers=[dict(input=times,output=values,interpolation='LINEAR')],
            channels=[dict(sampler=0,target=dict(path='pointer',extensions={POINTER:dict(pointer=pointer)}))])]
        asset.model['extensionsUsed'].append(POINTER)
    path.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='material-retirement-fixture-') as folder:
        generated=Path(folder)/'scene.gltf';asset.write(generated);immutable(path,generated.read_bytes())


def visual_pair(step):return [step['evaluated'][step['left']],step['evaluated'][3]]


def write_default_asset(path,name,color):
    asset=Asset();asset.model['materials']=[binding.material(name,color)]
    attrs=dict(POSITION=asset.accessor('VEC3',binding.POSITIONS),NORMAL=asset.accessor('VEC3',[[0.,0.,1.]]*4),
        TANGENT=asset.accessor('VEC4',[[1.,0.,0.,1.]]*4),TEXCOORD_0=asset.accessor('VEC2',[[0.,0.],[1.,0.],[1.,1.],[0.,1.]]))
    asset.model['meshes']=[dict(primitives=[dict(attributes=attrs,indices=asset.accessor('SCALAR',binding.INDICES,5123,indices=True),material=0,mode=4)])]
    asset.model['nodes']=[dict(mesh=0)];asset.model['scenes']=[dict(nodes=[0])];asset.model['extensionsUsed']=[UNLIT]
    path.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='retirement-default-asset-') as folder:
        generated=Path(folder)/'scene.gltf';asset.write(generated);immutable(path,generated.read_bytes())


def generate(output,cohort):
    output.mkdir(parents=True,exist_ok=True)
    sources={name:output/'sources'/(name+'.gltf') for name in ('source','animated','variant-blocker','pointer-blocker')}
    for name,path in sources.items():write_fixture(path,animated=name=='animated',blocker={'variant-blocker':'variant','pointer-blocker':'pointer'}.get(name))
    controls=[];known={};inputs=list(sources.values());runs=profiles(cohort)
    default_assets=[]
    if cohort=='runtime':
        for index,relative in enumerate(DEFAULT_ASSETS):
            path=output/'runtime-cwd'/relative;write_default_asset(path,'Builtin '+str(index),COLORS[index*3])
            default_assets.append(dict(relativePath=relative,path=str(path),sha256=sha(path)));inputs.append(path)
    for run in runs:
        for step in run['steps']:
            if cohort=='runtime':continue
            pair=visual_pair(step);key=json.dumps(pair,sort_keys=True)
            if key not in known:
                name='literal-'+str(len(controls));path=output/'sources'/(name+'.gltf');write_fixture(path,reference=pair)
                controls.append(dict(name=name,source=str(path),pair=pair,resolution=[WIDTH,HEIGHT],msaa=1));inputs.append(path);known[key]=name
            step['control']=known[key]
        script=dict(schemaVersion=1,frames=run['steps'][-1]['frame'],sampleFrames=[s['frame'] for s in run['steps']],
            camera=[dict(frame=1,position=[0.,0.,6.],target=[0.,0.,0.])],events=run['events'])
        path=output/'scripts'/(run['name']+'.json');document(path,script);run['script']=str(path);inputs.append(path)
    preflight=int(cohort in ('runtime','append-history'))
    captures=len(controls)+sum(len(r['steps'])+int('save' in r) for r in runs)+preflight
    processes=len(controls)+len(runs)+sum('save' in r for r in runs)+preflight
    additional=sum(sum(bool(s.get('failureAs')) for s in r['steps']) for r in runs)+int(cohort in ('core','history'))
    if cohort=='append-history':additional+=1
    plan=dict(schemaVersion=1,cohort=cohort,sources={k:str(v) for k,v in sources.items()},controls=controls,runs=runs,
        expectedCaptureCount=captures,expectedProcessCount=processes,expectedStateCheckCount=captures,
        expectedImageCheckCount=captures+additional,expectedExportCount=sum('save' in r for r in runs),
        runtimeCwd=str(output/'runtime-cwd'),defaultAssets=default_assets,
        immutableInputs=[dict(path=str(p),byteLength=p.stat().st_size,sha256=sha(p)) for p in inputs])
    document(output/'launch-plan.json',plan);return plan


def catalog(state):
    require(finite_json(state),'Nonfinite retirement telemetry')
    merge=state['gltfMerge'];value=state['gltfMaterialBindings']
    require(not merge['authoringNodesTruncated'] and len(merge['authoringNodes'])==merge['graphNodeCount'],'Incomplete graph catalog')
    require(not value['materialsTruncated'] and not value['runtimeMaterialStatesTruncated'],'Truncated material lifecycle catalog')
    mapping=value['sourceToRuntime'];rows=value['materials'];runtime=value['runtimeMaterialStates']
    require(len(rows)==len(mapping)==value['sourceMaterialCount'],'Incomplete source material table')
    require(all(type(i) is int and 0<=i<len(runtime) for i in mapping) and len(set(mapping))==len(mapping),
        'Source/runtime mapping is not a unique exact integer inventory')
    require(len(runtime)==value['runtimeMaterialCount'] and [r['runtimeMaterial'] for r in runtime]==list(range(len(runtime))) and
        all(type(r['runtimeMaterial']) is int and type(r['sourceMaterial']) is int for r in runtime),'Incomplete runtime lifecycle table')
    require(all(type(r['active']) is bool and type(r['incarnation']) is int and r['incarnation']>0 for r in runtime),'Missing exact lifecycle phase/incarnation')
    require(all(type(r['uploadedMaterialHash']) is int and r['uploadedMaterialHash']>0 and isinstance(r['materialProperties'],dict)
        for r in runtime),'Missing runtime packed-row hash/properties')
    require(len({r['incarnation'] for r in runtime})==len(runtime),'Lifecycle incarnations alias across runtime rows')
    require(value['retiredRuntimeMaterials']==[r['runtimeMaterial'] for r in runtime if not r['active']] and
        value['activeRuntimeMaterialCount']==sum(r['active'] for r in runtime),'Material lifecycle counts disagree')
    require([r['sourceMaterial'] for r in rows]==list(range(len(rows))) and [r['runtimeMaterial'] for r in rows]==mapping,
        'Source/runtime material identities are not explicit')
    for source,row in enumerate(rows):
        require(all(type(row[key]) is int for key in ('sourceMaterial','runtimeMaterial','incarnation','uploadedMaterialHash')) and
            type(row['active']) is bool,'Source row identities/phase/hash are not exact typed metadata')
        require(0<=mapping[source]<len(runtime) and row['active']==runtime[mapping[source]]['active'] and
            row['incarnation']==runtime[mapping[source]]['incarnation'] and runtime[mapping[source]]['sourceMaterial']==source,
            'Source/runtime lifecycle mapping disagrees')
        require(row['uploadedMaterialHash']==runtime[mapping[source]]['uploadedMaterialHash'] and
            row['materialProperties']==runtime[mapping[source]]['materialProperties'],'Source/runtime actual packed-row metadata disagrees')
    return merge,value


def inspect_record(actual,expected):
    deep_contains(actual,expected,'retirement material')
    require(set(actual.get('extensions',{}))==set(expected.get('extensions',{})),'Unexpected normative material payload')
    if expected==inert():
        require(actual==expected,'Retired row retained material/PBR metadata or outgoing payload')


def geometry_signature(state):
    merge=state['gltfMerge']
    return dict(graph=merge['graphIdentity'],nodes=[{k:v for k,v in r.items() if k!='materialIndex'} for r in merge['authoringNodes']],
        roots=merge['activeRoots'],tables={k:v for k,v in merge['sourceTables'].items() if k!='materials'},
        vertices=state['scene']['primaryVertexCount'],indices=state['scene']['primaryIndexCount'])


def history_state(state):
    # Creating the empty journal owner is not an edit. Only the exact
    # uncreated-empty representation may normalize to the emitted 0/0 cursor.
    history=state['gltfMaterialVariants'];depth=state['gltfScenes']['editHistoryDepth']
    require(type(depth) is int and depth>=0,'History depth is not an exact nonnegative integer')
    present=tuple(key in history for key in ('undoCount','redoCount'))
    if present==(False,False):
        history_cursor(state,0,0,allow_uncreated=True)
        return dict(undoCount=0,redoCount=0)
    require(present==(True,True) and all(type(history[key]) is int and history[key]>=0 for key in ('undoCount','redoCount')),
        'History cursor is partial, null, negative or not an exact integer pair')
    undo,redo=history['undoCount'],history['redoCount']
    require(depth==undo+redo,'History depth and exact cursor pair disagree')
    history_cursor(state,undo,redo)
    return dict(undoCount=undo,redoCount=redo)


def failure_signature(state):
    return dict(catalog=state['gltfMaterialBindings'],authoring=state['gltfMaterialAuthoring'],
        resources=state.get('gltfMaterialTextures'),geometry=geometry_signature(state),
        history=history_state(state),historyDepth=state['gltfScenes']['editHistoryDepth'])


def catalog_history_check(states):
    """Compare actual captures only within the same process/asset namespace.

    A matching authored/evaluated/phase record must reproduce the earlier packed
    row even after unrelated edits or clock changes. The initial capture alone
    cannot establish an edited unused row's later restoration hash.
    """
    incarnations={};packed={};defaults={};graph=None
    for state in states:
        merge,value=catalog(state)
        if graph is None:graph=merge['graphIdentity']
        require(merge['graphIdentity']==graph,'Material history silently replaced the scene namespace')
        source_rows={r['runtimeMaterial']:r for r in value['materials']}
        for row in value['runtimeMaterialStates']:
            runtime=row['runtimeMaterial'];incarnation=row['incarnation']
            if runtime in incarnations:require(incarnations[runtime]==incarnation,'A retained/reappearing runtime slot changed incarnation')
            else:incarnations[runtime]=incarnation
            if runtime in source_rows:
                source=source_rows[runtime]
                definition=dict(active=row['active'],authored=source['authored'],evaluated=source['evaluated'],properties=row['materialProperties'])
            else:
                definition=dict(active=row['active'],properties=row['materialProperties'])
                if runtime in (0,1):
                    if runtime in defaults:require(row==defaults[runtime],'Default/diagnostic packed material or identity changed')
                    else:defaults[runtime]=copy.deepcopy(row)
            key=(runtime,incarnation,json.dumps(definition,sort_keys=True,allow_nan=False))
            if key in packed:require(packed[key]==row['uploadedMaterialHash'],'Matching historical material definition did not restore its exact packed row')
            else:packed[key]=row['uploadedMaterialHash']
    return dict(passed=True,evidence='Actual chronological same-process incarnations, default rows and matching historical packed-row metadata')


def append_history_clock_check(states,steps):
    require(len(states)==len(steps),'Incomplete birth-history clock sequence')
    previous=None
    for state,step in zip(states,steps):
        value=state['gltfMaterialAuthoring']
        clocks=tuple(value[k] for k in ('generationEpoch','materialRevision','publications'))
        require(all(type(v) is int and v>=0 for v in clocks),'Invalid birth-history publication clocks')
        if previous is not None:
            label=step['label']
            # Assignments publish instance bindings, not a new material buffer;
            # animation seek publishes its buffer without recording an edit.
            delta=(0,0,0) if label=='used-rejected' else (0,1,0) if label=='seek' else (
                (1,0,1) if label in ('unbound','undo-assign','redo-assign') else (1,1,1))
            require(tuple(a-b for a,b in zip(clocks,previous))==delta,'Wrong exact birth-history publication clock transition')
        previous=clocks


def state_check(state,step,baseline=None):
    merge,value=catalog(state);rows=value['materials'];runtime=value['runtimeMaterialStates']
    require(state['resolution']==step['resolution'] and state['msaaSamples']==step['msaa'],'Stale frame configuration')
    expected_count=len(step['authored']);runtime_count=expected_count+2
    require(value['sourceMaterialCount']==expected_count and value['runtimeMaterialCount']==runtime_count and
        merge['sourceTables']['materials']==expected_count,'Retirement compacted live slots or reused an append ID')
    require(value['sourceToRuntime']==list(range(2,expected_count+2)),'Stable material source/runtime IDs changed')
    require(value['retiredRuntimeMaterials']==step['retired'],'Wrong arbitrary retired material set')
    for source,(authored,evaluated,row) in enumerate(zip(step['authored'],step['evaluated'],rows)):
        require(row['active']==(source+2 not in step['retired']),'Wrong source retirement phase')
        inspect_record(row['authored'],authored);inspect_record(row['evaluated'],evaluated)
        flags=binding.properties(evaluated) if UNLIT in evaluated.get('extensions',{}) else dict(
            transparent=False,alphaMasked=False,doubleSided=False,unlit=False,specularGlossiness=False,heightScale=0.)
        require(row['materialProperties']==flags and type(row['uploadedMaterialHash']) is int and row['uploadedMaterialHash']>0,
            'Uploaded material metadata does not follow the actual row')
    renderables=[r for r in merge['authoringNodes'] if r['renderable'] and not r['retired']]
    require(len(renderables)==2 and state['scene']['primaryObjectCount']==2,'Lost source instance')
    pair={r['graphNode']:r for r in renderables};require(1 in pair and 3 in pair,'Wrong retained primitive graph identities')
    require(pair[1]['materialIndex']==step['left']+2 and pair[3]['materialIndex']==5,'Retirement changed a surviving material binding')
    require(not value['drawsTruncated'],'Truncated actual controller draw inventory')
    draws={d['graphNode']:d for d in value['draws']};require(len(draws)==len(value['draws'])==len(renderables),'Missing/duplicated actual controller draws')
    for graph,source in ((1,step['left']),(3,3)):
        require(draws[graph]['runtimeMaterial']==source+2 and draws[graph]['indexCount']==6,'Actual controller draw changed material/geometry')
    history_cursor(state,*step['history'],allow_uncreated=step['history']==[0,0])
    require(not state['gltfMaterialAuthoring']['pendingGesture'],'Retirement left a material gesture pending')
    if step['time'] is not None:
        player=state['gltfAnimation'];require(player['selectedClip']==0 and not player['playing'],'Retirement changed playback controls')
        binding.close(player['time'],step['time'],'current retirement animation clock')
    if baseline is not None:
        _,old=catalog(baseline)
        common=min(len(runtime),len(old['runtimeMaterialStates']))
        require([r['incarnation'] for r in runtime[:common]]==[r['incarnation'] for r in old['runtimeMaterialStates'][:common]],
            'Delete/undo/redo changed a surviving slot incarnation')
        require(runtime[:2]==old['runtimeMaterialStates'][:2],'Source material edit changed default/diagnostic runtime rows')
        for row,previous in zip(rows,old['materials']):
            if row['active']==previous['active'] and row['authored']==previous['authored'] and row['evaluated']==previous['evaluated']:
                require(row['uploadedMaterialHash']==previous['uploadedMaterialHash'],'Unchanged actual row lost its CPU packed-entry hash')
            elif not row['active'] and previous['active']:
                require(row['uploadedMaterialHash']!=previous['uploadedMaterialHash'],'Retired row retained the previous CPU packed entry')
        require(geometry_signature(state)==geometry_signature(baseline),'Retirement changed retained geometry or graph structure')
    return dict(passed=True,retired=step['retired'],sourceMaterials=expected_count,runtimeMaterials=runtime_count,
        history=step['history'],evidence='CPU exact source/runtime phase, incarnation, records, hashes and actual controller draws')


def reference_state(state,pair):
    merge,value=catalog(state)
    rows=sorted([r for r in merge['authoringNodes'] if r['renderable'] and not r['retired']],key=lambda r:r['worldColumns'][3][0])
    require(len(rows)==2 and not value['retiredRuntimeMaterials'],'Control/reload has wrong primitive or retired set')
    for graph,expected in zip(rows,pair):
        entries=[r for r in value['materials'] if r['runtimeMaterial']==graph['materialIndex']]
        require(len(entries)==1,'Control/reload bound row lacks exact source owner');inspect_record(entries[0]['evaluated'],expected)
    history_cursor(state,0,0,allow_uncreated=True)
    return dict(passed=True,evidence='CPU actual bound control/reloaded material records')


def inspect_export(path,step,*,animated):
    model,_=load_asset(path);materials=model['materials'];expected_count=len(step['authored'])+2-len(step['retired'])
    require(len(materials)==expected_count,'Serialized material table retained tombstones or dropped live rows')
    require(model.get('extras')==dict(retirementOpaque=dict(material=1,materials=[1,3],pointer='/materials/1/not-authority')),'Opaque extras changed')
    names=[m.get('name','') for m in materials]
    for source,(authored,evaluated) in enumerate(zip(step['authored'],step['evaluated'])):
        if source+2 in step['retired']:continue
        require(names.count(authored['name'])==1,'Surviving exported material missing/duplicated')
        inspect_record(serialized_material_record(materials[names.index(authored['name'])]),evaluated)
    original_names=(*MATERIAL_NAMES,'Append after retirement')
    for runtime in step['retired']:
        require(original_names[runtime-2] not in names,'Deleted material payload survived serialization')
    right_material=names.index(step['authored'][3]['name'])
    # The original raw defaults still exist in retained source meshes even if
    # the selected live instance later receives another material assignment.
    bound_materials={names.index(step['authored'][source]['name']) for source in {0,3,step['left']}}
    meshes=model['meshes'];nodes=model['nodes'];scenes=model['scenes']
    def check_index(value,count,label):
        require(type(value) is int and 0<=value<count,'Invalid complete export '+label+' reference')
        return value
    # Detached export can retain unselected/orphan source rows. Every material
    # reference in every mesh still needs a valid, correctly remapped target.
    all_variants=[]
    for mesh in meshes:
        require(isinstance(mesh.get('primitives'),list) and mesh['primitives'],'Empty/invalid exported mesh')
        for primitive in mesh['primitives']:
            material=check_index(primitive.get('material'),len(materials),'primitive material')
            require(material in bound_materials and primitive.get('mode',4)==4,'Unexpected/stale complete mesh material or topology')
            binding.close(decode_accessor(path,primitive['attributes']['POSITION']),np.asarray(binding.POSITIONS,dtype=np.float32).astype(float),'all export positions',0.)
            binding.close(np.asarray(decode_accessor(path,primitive['indices'])).reshape(-1),binding.INDICES,'all export indices',0.)
            binding.close(decode_accessor(path,primitive['attributes']['NORMAL']),[[0.,0.,1.]]*4,'all export normals',0.)
            extensions=primitive.get('extensions',{})
            require(set(extensions)<={VARIANTS},'Unexpected complete primitive normative schema')
            if VARIANTS in extensions:
                body=extensions[VARIANTS]
                require(set(body)=={'mappings'} and isinstance(body['mappings'],list) and len(body['mappings'])==1,'Changed complete variant mapping shape')
                for mapping in body['mappings']:
                    check_index(mapping.get('material'),len(materials),'variant material')
                    require(mapping==dict(material=right_material,variants=[0],extras=dict(material=1,opaque=True)),
                        'Complete raw variant reference was not remapped exactly')
                    all_variants.append(mapping)
    require(all_variants,'All source variant mappings disappeared')
    for node in nodes:
        if 'mesh' in node:check_index(node['mesh'],len(meshes),'node mesh')
        children=node.get('children',[]);require(isinstance(children,list) and len(set(children))==len(children),'Invalid complete child inventory')
        for child in children:check_index(child,len(nodes),'child node')
    for scene in scenes:
        roots=scene.get('nodes',[]);require(isinstance(roots,list) and len(set(roots))==len(roots),'Invalid complete scene-root inventory')
        for root in roots:check_index(root,len(nodes),'scene root')
    check_index(model.get('scene',0),len(scenes),'selected scene')
    visited=set()
    def visit(node,ancestors):
        require(node not in ancestors,'Cycle in unselected/orphan export graph')
        if node in visited:return
        for child in nodes[node].get('children',[]):visit(child,ancestors|{node})
        visited.add(node)
    for node in range(len(nodes)):visit(node,set())
    require(all('MSFT_lod' not in material.get('extensions',{}) for material in materials),'Export invented an unproved material LOD reference')
    pending=[(i,0.) for i in scenes[model.get('scene',0)]['nodes']];found=[];seen=set();variants=[]
    while pending:
        index,parent_x=pending.pop();require(index not in seen and 0<=index<len(nodes),'Invalid export forest');seen.add(index);node=nodes[index]
        x=parent_x+(node['matrix'][12] if 'matrix' in node else node.get('translation',[0.,0.,0.])[0])
        pending.extend((child,x) for child in node.get('children',[]))
        if 'mesh' in node:
            primitives=model['meshes'][node['mesh']]['primitives'];require(len(primitives)==1,'Export changed primitive count')
            primitive=primitives[0];found.append((x,primitive['material']))
            binding.close(decode_accessor(path,primitive['attributes']['POSITION']),np.asarray(binding.POSITIONS,dtype=np.float32).astype(float),'retirement export positions',0.)
            binding.close(np.asarray(decode_accessor(path,primitive['indices'])).reshape(-1),binding.INDICES,'retirement export indices',0.)
            if VARIANTS in primitive.get('extensions',{}):variants.extend(primitive['extensions'][VARIANTS]['mappings'])
    found.sort();require(len(found)==2,'Export lost source instance')
    binding.close([x for x,_ in found],CANONICAL_PLACEMENTS,'retirement export placements',0.)
    for (_,index),expected in zip(found,visual_pair(step)):inspect_record(serialized_material_record(materials[index]),expected)
    require(variants and all(row['material']==found[1][1] and row['variants']==[0] and row['extras']==dict(material=1,opaque=True) for row in variants),
        'Surviving raw variant material was not remapped exactly')
    require(model['extensions'][VARIANTS]['variants']==[dict(name='Surviving right')],'Variant definitions changed')
    if animated:
        animations=model.get('animations',[]);require(len(animations)==1 and len(animations[0]['channels'])==1,'Original curve lost or duplicated')
        clip=animations[0];channel=clip['channels'][0];target=channel['target']
        require(target.get('path')=='pointer' and 'node' not in target and set(target.get('extensions',{}))=={POINTER},
            'Material animation target is not an exact node-less pointer target')
        require(target['extensions'][POINTER]['pointer']==f'/materials/{found[1][1]}/pbrMetallicRoughness/baseColorFactor' and
            clip['extras']==dict(retirementCurve='unchanged'),'Original material pointer prefix/metadata not remapped exactly')
        require(len(clip['samplers'])==1,'Original material curve sampler inventory changed')
        check_index(channel['sampler'],len(clip['samplers']),'animation sampler')
        sampler=clip['samplers'][channel['sampler']];require(sampler.get('interpolation','LINEAR')=='LINEAR','Curve interpolation changed')
        binding.close(np.asarray(decode_accessor(path,sampler['input'])).reshape(-1),[0.,1.],'retirement curve times',0.)
        binding.close(decode_accessor(path,sampler['output']),np.asarray([COLORS[3],END],dtype=np.float32).astype(float),'retirement curve values',0.)
    else:require(not model.get('animations'),'Export invented an animation')
    return dict(passed=True,path=str(path),sha256=sha(path),materialCount=len(materials),bindingMaterials=[i for _,i in found],
        denseRetiredRowsAbsent=True,survivingReferencesRemapped=True,allMeshMaterialReferencesVerified=True,
        allSceneNodeReferencesVerified=True,exactPointerTargetVerified=animated,geometryAndOpaqueMetadataPreserved=True)


def dependencies():return binding.dependencies()


def runtime_state_check(state,step,baseline=None):
    merge,value=catalog(state);rows=value['runtimeMaterialStates']
    require(value['sourceMaterialCount']==0 and value['runtimeMaterialCount']==5 and not value['materials'] and
        not value['sourceToRuntime'] and merge['sourceTables']['materials']==0,'Builtin fixture did not produce genuine source-less material slots')
    require(all(row['sourceMaterial']==-1 for row in rows),'Builtin runtime material gained a fabricated source identity')
    require(value['retiredRuntimeMaterials']==([2] if step['retired'] else []) and
        value['activeRuntimeMaterialCount']==(4 if step['retired'] else 5),'Wrong builtin runtime retirement phase')
    actual=sorted([r for r in merge['authoringNodes'] if r['renderable'] and not r['retired']],key=lambda r:r['primitiveIndex'])
    require(len(actual)==3 and state['scene']['primaryObjectCount']==3 and
        [r['graphNode'] for r in actual]==[1,2,3] and [r['primitiveIndex'] for r in actual]==[0,1,2],
        'Builtin preflight no longer proves explicit sphere graph/primitive identities')
    expected=[3,4,3 if step['bound'] else 2]
    require([r['materialIndex'] for r in actual]==expected,'Builtin reassignment/retirement changed wrong instance')
    require(not value['drawsTruncated'],'Builtin actual controller draw inventory is truncated')
    draws=sorted(value['draws'],key=lambda r:r['primitiveIndex'])
    require(len(draws)==3 and [r['runtimeMaterial'] for r in draws]==expected and all(r['indexCount']>0 for r in draws),
        'Actual builtin controller draws do not follow source-less material binding')
    for row in rows:require(type(row['uploadedMaterialHash']) is int and row['uploadedMaterialHash']>0,'Missing builtin actual CPU packed-entry hash')
    require(rows[3]['materialProperties']['unlit'] and rows[4]['materialProperties']['unlit'],'Generated literal builtin materials were not imported')
    if step['retired']:
        require(rows[2]['materialProperties']==dict(transparent=False,alphaMasked=False,doubleSided=False,unlit=False,
            specularGlossiness=False,heightScale=0.),'Retired source-less row is not inert')
    history_cursor(state,*step['history'],allow_uncreated=step['history']==[0,0])
    require(not state['gltfMaterialAuthoring']['pendingGesture'],'Builtin retirement left pending gesture')
    if baseline is not None:
        _,old=catalog(baseline);before=old['runtimeMaterialStates']
        require(geometry_signature(state)==geometry_signature(baseline),'Builtin retirement changed geometry/graph/source resources')
        require([r['incarnation'] for r in rows]==[r['incarnation'] for r in before],'Source-less retirement/history changed incarnation')
        for index in (0,1,3,4):require(rows[index]['uploadedMaterialHash']==before[index]['uploadedMaterialHash'],'Builtin retirement changed another packed row')
        require((rows[2]['uploadedMaterialHash']!=before[2]['uploadedMaterialHash']) if step['retired'] else
            (rows[2]['uploadedMaterialHash']==before[2]['uploadedMaterialHash']),'Builtin selected payload was not retired/restored exactly')
    return dict(passed=True,sourceLessRuntimeMaterial=2,graphNode=3,primitive=2,retired=step['retired'],history=step['history'],
        evidence='CPU exact runtime-only identity/phase/packed-row hash/properties and actual controller draws')


def runtime_pixels(record):
    with Image.open(record['image']) as image:
        require(image.size==(WIDTH,HEIGHT),'Wrong builtin image extent');return np.asarray(image.convert('RGB'),dtype=np.int16)


def runtime_visible(record):
    image=runtime_pixels(record);sphere=image[165:195,485:505].astype(float);background=image[20:35,20:35].astype(float)
    contrast=float(np.abs(sphere.mean(axis=(0,1))-background.mean(axis=(0,1))).mean())
    require(contrast>8.,'Builtin sphere image witness is blank/offscreen')
    return dict(passed=True,sphereRegion=[485,165,505,195],backgroundContrast=contrast,evidence='Actual Lit-mode sphere pixels under fixed front lighting')


def runtime_lighting_check(lighting):
    def exact(value,expected):
        return type(value) in (int,float) and math.isfinite(value) and value==expected
    require(exact(lighting['environmentIntensity'],0.) and exact(lighting['directionalIntensity'],1.),
        'Wrong builtin fixed lighting intensity')
    for key,expected in (('directionalDirection',[0.,0.,-1.]),('directionalColor',[1.,1.,1.])):
        values=lighting[key]
        require(isinstance(values,list) and len(values)==3 and all(exact(a,b) for a,b in zip(values,expected)),
            'Wrong builtin fixed '+key)
    require(type(lighting['pointLightCount']) is int and lighting['pointLightCount']==0 and
        type(lighting['areaLightCount']) is int and lighting['areaLightCount']==0,'Unexpected builtin local lights')


def runtime_unchanged(actual,reference):
    error=int(np.abs(runtime_pixels(actual)-runtime_pixels(reference)).max())
    require(error==0,'Unused source-less retirement/history changed rendered image')
    return dict(passed=True,fullImage=True,maximumByteError=error,evidence='Actual full rendered image equality')


def runtime_positive(before,after):
    a,b=runtime_pixels(before),runtime_pixels(after)
    delta=float(np.abs(a[165:195,485:505]-b[165:195,485:505]).mean())
    unchanged=[int(np.abs(a[y0:y1,x0:x1]-b[y0:y1,x0:x1]).max()) for x0,y0,x1,y1 in ((135,165,155,195),(310,165,330,195))]
    require(delta>2. and max(unchanged)==0,'Builtin prior reassignment was vacuous or changed another object')
    literal_delta=int(np.abs(b[165:195,485:505]-a[165:195,135:155]).max())
    require(literal_delta==0,'Runtime-only assigned sphere differs from the independently authored literal red control')
    return dict(passed=True,sphereMeanDelta=delta,otherRegionMaximumDelta=unchanged,literalMaximumDelta=literal_delta,
        evidence='Actual positive isolated runtime-only reassignment and independent literal red instance')


def launch_runtime(args,output,report,persist,verify,technique,name,steps,*,script=None):
    """Ordinary builtin scene producer; isolated CWD supplies its sample assets."""
    verify();target=output/'captures'/(technique+'-'+name);target.parent.mkdir(parents=True,exist_ok=True)
    require(not binding.artifacts(target),'Refusing to overwrite builtin retirement evidence')
    image,log=target.with_suffix('.png'),target.with_suffix('.log')
    command=[str(args.renderer),'--hidden','--no-ui','--validation','--no-bloom','--no-taa','--width',str(WIDTH),'--height',str(HEIGHT),
        '--msaa','1','--warmup-frames','12','--capture-frame','13','--fixed-dt','.016666667','--render-technique',technique,
        '--display-mode','lit','--exposure','.35','--camera-position','0','0','6','--camera-target','0','0','0','--camera-fov','45',
        '--environment-intensity','0','--directional-intensity','1','--directional-direction','0','0','-1','--directional-color','1','1','1',
        '--import-scale','1','--model',DEFAULT_TOKEN,'--screenshot',str(image),
        '--no-ray-query','--ray-shadows','raster']
    if script:command+=['--capture-sequence',str(script)]
    row=dict(name=name,technique=technique,command=command,cwd=str(output/'runtime-cwd'),log=str(log),completed=False)
    report['processes'].append(row);report['rendererExecuted']=True;persist()
    try:
        with log.open('x',encoding='utf-8') as stream:
            process=subprocess.run(command,cwd=output/'runtime-cwd',stdout=stream,stderr=subprocess.STDOUT,timeout=args.timeout,
                env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(output)),creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        text=log.read_text(encoding='utf-8',errors='replace');row.update(returnCode=process.returncode,completed=True,logSha256=sha(log));persist()
        require(not validation_log_error(text),'Strict Vulkan validation error: '+str(log));require(process.returncode==0,'Builtin retirement process failed: '+str(log))
        final=json.loads(script.read_text())['frames'] if script else 13;records={}
        for step in steps:
            frame=step['frame'];sampled=image if frame==final else image.with_name(image.stem+f'.frame-{frame:04d}.png')
            telemetry=sampled.with_suffix('.telemetry.json');require(sampled.is_file() and telemetry.is_file(),'Missing builtin capture image/telemetry')
            state=json.loads(telemetry.read_text(encoding='utf-8-sig'))
            require(finite_json(state) and state['technique']==technique and state['resolution']==[WIDTH,HEIGHT] and state['msaaSamples']==1 and
                not state['taa']['enabled'] and
                any(p['active'] and p['gpuTimed'] for p in state['passes']),'Wrong builtin native configuration/no timed GPU frame')
            runtime_lighting_check(state['lighting'])
            item=dict(name=step['label'],process=name,technique=technique,frame=frame,resolution=[WIDTH,HEIGHT],msaa=1,
                image=str(sampled),telemetry=str(telemetry),log=str(log),imageSha256=sha(sampled),telemetrySha256=sha(telemetry),validationClean=True)
            report['captures'].append(item);records[step['label']]={**item,'state':state}
        require({p.resolve() for p in binding.artifacts(target) if p.suffix=='.png'}=={Path(r['image']).resolve() for r in records.values()},'Unexpected builtin capture inventory')
        persist();return records
    finally:verify();persist()


def run_native(args,output,plan,report):
    require(args.renderer.is_file() and args.manifest.is_file(),'Missing renderer/frozen input manifest')
    require(not any(output.glob('captures/*')) and not any(output.glob('exports/**/*')) and not any(output.glob('portable/**/*')),'Evidence directories must be fresh')
    if (output/'results.json').exists():
        previous=json.loads((output/'results.json').read_text());require(previous.get('generatedOnly') and not previous.get('rendererExecuted'),'Refusing to overwrite native retirement evidence')
    manifest_hash=sha(args.manifest)
    frozen_paths={Path(p).resolve() for p in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
    def persist():
        for key in ('captures','processes','stateChecks','imageChecks','exports'):report[key+'Count']=len(report[key])
        (output/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    def runtime_files():return sorted([*args.renderer.parent.glob('*.dll'),*(args.renderer.parent/'spv_shaders').glob('*.spv')])
    def verify():
        require(sha(args.manifest)==manifest_hash,'Frozen retirement manifest changed');result=verify_manifest(args.manifest)
        require((output/'vk_layer_settings.txt').read_bytes()==b'khronos_validation.validate_sync = true\n','Synchronization-validation settings changed')
        require(dependencies()==report['sourceDependencies'] and sha(output/'launch-plan.json')==report['launchPlanSha256'],'Retirement checker/dependencies/plan changed')
        for row in plan['immutableInputs']:
            p=Path(row['path']);require(p.stat().st_size==row['byteLength'] and sha(p)==row['sha256'],'Retirement fixture/script changed')
        if args.cohort=='runtime':
            resolved=[]
            for row in plan['defaultAssets']:
                preferred=args.renderer.parent/row['relativePath'];path=preferred if preferred.exists() else Path(row['path'])
                require(path.is_file() and sha(path)==row['sha256'],'Builtin asset priority differs from exact isolated fixture')
                resolved.append(dict(path=str(path.resolve()),sha256=row['sha256'],exePriority=preferred.exists()))
            material=args.renderer.parent/'materials/base.mtlx'
            if not material.exists():material=Path(plan['runtimeCwd'])/'materials/base.mtlx'
            if material.exists():
                require(material.resolve() in frozen_paths,'Native manifest omits builtin default MaterialX source')
                resolved.append(dict(path=str(material.resolve()),sha256=sha(material),materialXDefault=True))
            if 'defaultAssetResolution' in report:require(resolved==report['defaultAssetResolution'],'Builtin source resolution changed')
            else:report['defaultAssetResolution']=resolved
        if 'runtimeInputs' in report:require([str(p.resolve()) for p in runtime_files()]==report['runtimeInputs'],'Resident DLL/shader inventory changed')
        for copied in report['portableCopies']:require(all(sha(Path(f['path']))==f['sha256'] for f in copied['files']),'Portable retirement export changed')
        return result
    def check(kind,technique,name,result):report[kind].append(dict(technique=technique,check=name,**result));persist()
    try:
        report['nativeManifestBefore']=verify_manifest(args.manifest)
        frozen={Path(p).resolve() for p in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
        require({args.renderer,*map(Path,report['sourceDependencies'])}<=frozen,'Manifest omits renderer/local Python helpers')
        runtime=runtime_files();require(any(p.suffix=='.spv' for p in runtime) and {p.resolve() for p in runtime}<=frozen,'Manifest omits resident DLLs/SPIR-V')
        report.update(rendererSha256=sha(args.renderer),runtimeInputs=[str(p.resolve()) for p in runtime])
        immutable(output/'vk_layer_settings.txt',b'khronos_validation.validate_sync = true\n');persist();verify()
        for technique in args.technique:
            if args.cohort=='runtime':
                run=plan['runs'][0];before=run['steps'][0]
                preflight=launch_runtime(args,output,report,persist,verify,technique,'builtin-identity-preflight',[before])['before']
                check('stateChecks',technique,'builtin-identity-preflight',runtime_state_check(preflight['state'],before))
                check('imageChecks',technique,'builtin-identity-preflight-visible',runtime_visible(preflight))
                # No numeric action is launched until this independent native
                # process proves the actual source-less slot/binding identities.
                records=launch_runtime(args,output,report,persist,verify,technique,run['name'],run['steps'],script=Path(run['script']))
                catalog_history_check([records[step['label']]['state'] for step in run['steps']])
                for step in run['steps']:
                    record=records[step['label']]
                    check('stateChecks',technique,run['name']+'/'+step['label'],runtime_state_check(record['state'],step,preflight['state']))
                    if step['label']=='reassigned':result=runtime_positive(records['before'],record)
                    else:result=runtime_unchanged(record,preflight if not step['bound'] else records['reassigned'])
                    check('imageChecks',technique,run['name']+'/'+step['label'],result)
                continue
            controls={}
            for control in plan['controls']:
                step=dict(label='control',frame=13,resolution=control['resolution'],msaa=control['msaa'])
                record=binding.launch(args,output,report,persist,verify,technique,control['name'],Path(control['source']),[step],initial=control)['control']
                check('stateChecks',technique,control['name'],reference_state(record['state'],control['pair']))
                check('imageChecks',technique,control['name']+'-visible',binding.visible_reference(record));controls[control['name']]=record
            if args.cohort=='append-history':
                before=plan['runs'][0]['steps'][0]
                preflight=binding.launch(args,output,report,persist,verify,technique,'birth-identity-preflight',
                    Path(plan['sources']['animated']),[before],animation_time=.25)['before']
                check('stateChecks',technique,'birth-identity-preflight',state_check(preflight['state'],before))
                check('imageChecks',technique,'birth-identity-preflight',binding.compare_images(preflight,controls[before['control']]))
            first=None
            for run in plan['runs']:
                save=output/'exports'/technique/run['name']/('scene.'+run['save']) if 'save' in run else None
                records=binding.launch(args,output,report,persist,verify,technique,run['name'],Path(plan['sources'][run['source']]),run['steps'],
                    script=Path(run['script']),save=save,animation_time=.25 if run.get('animated') else None)
                if first is None:first=records
                baseline=records['before']['state']
                catalog_history_check([records[step['label']]['state'] for step in run['steps']])
                if args.cohort=='append-history':
                    append_history_clock_check([records[s['label']]['state'] for s in run['steps']],run['steps'])
                if run.get('expectedFailures'):
                    log=Path(next(iter(records.values()))['log']).read_text(encoding='utf-8',errors='replace')
                    require(log.count('Expected capture material retirement failure: ')==run['expectedFailures'],'Missing explicit expected-retirement rejection diagnostics')
                for step in run['steps']:
                    record=records[step['label']];check('stateChecks',technique,run['name']+'/'+step['label'],state_check(record['state'],step,baseline))
                    check('imageChecks',technique,run['name']+'/'+step['label'],binding.compare_images(record,controls[step['control']]))
                    if step['failureAs']:
                        earlier=records[step['failureAs']]
                        require(failure_signature(record['state'])==failure_signature(earlier['state']),'Rejected retirement changed source/resources/routing/generation/history')
                        check('imageChecks',technique,run['name']+'/'+step['label']+'-unchanged-after',binding.compare_images(record,earlier))
                    incarnation_as='appended' if run['name']=='middle-and-append' and step['label']=='redo-append' else None
                    if incarnation_as:
                        before_states=records[incarnation_as]['state']['gltfMaterialBindings']['runtimeMaterialStates']
                        after_states=record['state']['gltfMaterialBindings']['runtimeMaterialStates']
                        require([r['incarnation'] for r in before_states]==[r['incarnation'] for r in after_states],
                            'History replay changed the retained appended/runtime-only material incarnation')
                if save:
                    last=run['steps'][-1];report['exports'].append(dict(technique=technique,**inspect_export(save,last,animated=run.get('animated',False))));persist()
                    copied,files=portable_copy(save,output/'portable'/technique/run['name']);report['portableCopies'].append(dict(files=files));persist()
                    step=dict(label='reload',frame=13,resolution=last['resolution'],msaa=last['msaa'])
                    reload=binding.launch(args,output,report,persist,verify,technique,run['name']+'-reload',copied,[step],initial=step,
                        animation_time=last['time'] if run.get('animated') else None)['reload']
                    check('stateChecks',technique,run['name']+'-reload',reference_state(reload['state'],visual_pair(last)))
                    check('imageChecks',technique,run['name']+'-reload',binding.compare_images(reload,records[last['label']]))
            if args.cohort in ('core','history'):
                a,b=('redo','assigned') if args.cohort=='core' else ('factor','bound')
                check('imageChecks',technique,'isolated-positive-binding-control',binding.isolate_change(first[a],first[b],0))
            if args.cohort=='append-history':
                check('imageChecks',technique,'birth-visible-factor-control',binding.isolate_change(first['appended'],first['edited'],0))
        for key in ('Capture','Process','StateCheck','ImageCheck','Export'):
            plural={'Capture':'captures','Process':'processes','StateCheck':'stateChecks','ImageCheck':'imageChecks','Export':'exports'}[key]
            require(len(report[plural])==plan['expected'+key+'Count']*len(args.technique),'Incomplete retirement '+plural+' cohort')
        report.update(passed=True,nativeValidationPerformed=True);verify()
    except Exception as error:report['failure']=str(error);raise
    finally:
        if report.get('nativeManifestBefore'):
            try:report['nativeManifestAfter']=verify()
            except Exception as error:report.update(passed=False,afterManifestFailure=str(error));report.setdefault('failure',str(error))
        persist()
    require(report['passed'],'Native retirement provenance failed after execution')


def synthetic_state(step):
    """Independent telemetry-shaped input for checker calibration only."""
    runtime_count=len(step['authored'])+2
    runtime=[dict(runtimeMaterial=i,sourceMaterial=i-2 if 2<=i<len(step['authored'])+2 else -1,
        active=i not in step['retired'],incarnation=100+i,uploadedMaterialHash=1000+i,
        materialProperties=dict(transparent=False,alphaMasked=False,doubleSided=False,unlit=False,specularGlossiness=False,heightScale=0.)) for i in range(runtime_count)]
    rows=[]
    for source,(a,e) in enumerate(zip(step['authored'],step['evaluated'])):
        flags=binding.properties(e) if UNLIT in e.get('extensions',{}) else dict(
            transparent=False,alphaMasked=False,doubleSided=False,unlit=False,specularGlossiness=False,heightScale=0.)
        # A deterministic calibration identifier, not a fake implementation hash.
        packed=int(sha_bytes(json.dumps(e,sort_keys=True).encode())[:12],16)+1
        rows.append(dict(sourceMaterial=source,runtimeMaterial=source+2,active=runtime[source+2]['active'],
            incarnation=runtime[source+2]['incarnation'],authored=copy.deepcopy(a),evaluated=copy.deepcopy(e),
            materialProperties=flags,uploadedMaterialHash=packed))
        runtime[source+2]['materialProperties']=copy.deepcopy(flags);runtime[source+2]['uploadedMaterialHash']=packed
    nodes=[]
    for owner,(x,source) in enumerate(((-1.1,step['left']),(1.1,3))):
        world=[[1.,0.,0.,0.],[0.,1.,0.,0.],[0.,0.,1.,0.],[x,0.,0.,1.]]
        nodes.extend([dict(graphNode=owner*2,renderable=False,retired=False,materialIndex=0,worldColumns=world),
            dict(graphNode=owner*2+1,renderable=True,retired=False,materialIndex=source+2,worldColumns=world)])
    value=dict(sourceMaterialCount=len(rows),runtimeMaterialCount=runtime_count,sourceToRuntime=list(range(2,len(rows)+2)),
        materials=rows,materialsTruncated=False,runtimeMaterialStates=runtime,runtimeMaterialStatesTruncated=False,
        activeRuntimeMaterialCount=sum(r['active'] for r in runtime),retiredRuntimeMaterials=list(step['retired']),drawsTruncated=False,
        draws=[dict(graphNode=n['graphNode'],runtimeMaterial=n['materialIndex'],indexCount=6) for n in nodes if n['renderable']])
    return dict(resolution=step['resolution'],msaaSamples=step['msaa'],
        gltfMerge=dict(graphIdentity=7,authoringNodes=nodes,authoringNodesTruncated=False,graphNodeCount=len(nodes),
            activeRoots=[0,2],sourceTables=dict(materials=len(rows),nodes=2,meshes=2)),
        gltfMaterialBindings=value,scene=dict(primaryObjectCount=2,primaryVertexCount=8,primaryIndexCount=12),
        gltfMaterialVariants=dict(undoCount=step['history'][0],redoCount=step['history'][1]),
        gltfScenes=dict(editHistoryDepth=sum(step['history'])),
        gltfMaterialAuthoring=dict(pendingGesture=False,generationEpoch=1,materialRevision=1,publications=0),
        gltfAnimation=dict(selectedClip=0,playing=False,time=step['time']),gltfMaterialTextures=dict(resourceRevision=1))


def sha_bytes(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


def synthetic_runtime_state(step):
    flags=dict(transparent=False,alphaMasked=False,doubleSided=False,unlit=False,specularGlossiness=False,heightScale=0.)
    runtime=[dict(runtimeMaterial=i,sourceMaterial=-1,active=not(i==2 and step['retired']),incarnation=100+i,
        uploadedMaterialHash=(9999 if i==2 and step['retired'] else 1000+i),materialProperties=dict(flags,unlit=i in (3,4))) for i in range(5)]
    world=[[1.,0.,0.,0.],[0.,1.,0.,0.],[0.,0.,1.,0.],[0.,0.,0.,1.]]
    nodes=[dict(graphNode=0,renderable=False,retired=False,materialIndex=0,primitiveIndex=2**32-1,worldColumns=world)]
    for index,material in enumerate((3,4,3 if step['bound'] else 2)):
        nodes.append(dict(graphNode=index+1,renderable=True,retired=False,materialIndex=material,primitiveIndex=index,worldColumns=world))
    nodes.append(dict(graphNode=4,renderable=False,retired=False,materialIndex=0,primitiveIndex=2**32-1,worldColumns=world))
    return dict(resolution=[WIDTH,HEIGHT],msaaSamples=1,scene=dict(primaryObjectCount=3,primaryVertexCount=100,primaryIndexCount=500),
        gltfMerge=dict(graphIdentity=7,authoringNodes=nodes,authoringNodesTruncated=False,graphNodeCount=len(nodes),
            activeRoots=[0],sourceTables=dict(materials=0,nodes=0,meshes=0)),
        gltfMaterialBindings=dict(sourceMaterialCount=0,runtimeMaterialCount=5,sourceToRuntime=[],materials=[],materialsTruncated=False,
            runtimeMaterialStates=runtime,runtimeMaterialStatesTruncated=False,activeRuntimeMaterialCount=4 if step['retired'] else 5,
            retiredRuntimeMaterials=[2] if step['retired'] else [],drawsTruncated=False,
            draws=[dict(graphNode=n['graphNode'],primitiveIndex=n['primitiveIndex'],runtimeMaterial=n['materialIndex'],indexCount=6)
                for n in nodes if n['renderable']]),
        gltfMaterialVariants=dict(undoCount=step['history'][0],redoCount=step['history'][1]),
        gltfScenes=dict(editHistoryDepth=sum(step['history'])),
        gltfMaterialAuthoring=dict(pendingGesture=False,generationEpoch=1,materialRevision=1,publications=0))


def self_check():
    checks=[];counts={}
    def reject(label,operation):
        try:operation()
        except (AssertionError,RuntimeError,ValueError,KeyError,IndexError):checks.append('reject-'+label)
        else:raise AssertionError('Retirement checker accepted '+label)
    with tempfile.TemporaryDirectory(prefix='material-retirement-check-') as folder:
        for cohort in COHORTS:
            path=Path(folder)/cohort;plan=generate(path,cohort)
            require(generate(path,cohort)==plan,'Retirement plan is not byte-repeatable')
            for row in plan['immutableInputs']:require(sha(Path(row['path']))==row['sha256'],'Wrong immutable fixture hash')
            for run in plan['runs']:
                script=json.loads(Path(run['script']).read_text());require(script['sampleFrames']==[s['frame'] for s in run['steps']] and
                    script['frames']==run['steps'][-1]['frame'],'Incomplete script sampling plan')
                for step in run['steps']:
                    if cohort!='runtime':require(all(step['left']+2!=r and 5!=r for r in step['retired']),'Planned retirement hides a used material')
            model,_=load_asset(Path(plan['sources']['source']));require(len(model['materials'])==4 and
                [p['material'] for m in model['meshes'] for p in m['primitives']]==[0,3],'Fixture lost arbitrary unused middle row')
            blocker,_=load_asset(Path(plan['sources']['variant-blocker']));require(blocker['meshes'][0]['primitives'][0]['extensions'][VARIANTS]['mappings'][0]['material']==1,'Variant blocker is vacuous')
            blocker,_=load_asset(Path(plan['sources']['pointer-blocker']));require(blocker['animations'][0]['channels'][0]['target']['extensions'][POINTER]['pointer'].startswith('/materials/1/'),'Raw pointer blocker is vacuous')
            counts[cohort]={k:v for k,v in plan.items() if k.startswith('expected')};checks.append(cohort+'-immutable-semantic-plan')
            for run in plan['runs']:
                if cohort=='runtime':
                    baseline=synthetic_runtime_state(run['steps'][0])
                    for step in run['steps']:runtime_state_check(synthetic_runtime_state(step),step,baseline)
                    catalog_history_check([synthetic_runtime_state(step) for step in run['steps']])
                    require(run['events'][0]['materialBinding']==dict(operation='assign',graphNode=3,expectedRuntimeMaterial=2,sourceMaterial=-1,runtimeMaterial=3),
                        'Runtime cohort stopped using the real builtin source-less producer')
                    for row in plan['defaultAssets']:
                        model,_=load_asset(Path(row['path']));require(len(model['materials'])==len(model['meshes'])==len(model['nodes'])==1,
                            'Builtin isolated fixture adds unexpected materials/objects')
                else:
                    baseline=synthetic_state(run['steps'][0])
                    for step in run['steps']:state_check(synthetic_state(step),step,baseline)
                    catalog_history_check([synthetic_state(step) for step in run['steps']])
            checks.append(cohort+'-semantic-state-checker')
        clock_steps=[dict(label=label) for label in ('before','appended','unbound','seek','used-rejected','redo-delete')]
        clock_values=[(0,2,0),(1,3,1),(2,3,2),(2,4,2),(2,4,2),(3,5,3)]
        clock_states=[dict(gltfMaterialAuthoring=dict(zip(('generationEpoch','materialRevision','publications'),v))) for v in clock_values]
        append_history_clock_check(clock_states,clock_steps);checks.append('birth-history-exact-clock-transitions')
        for index,key in ((2,'materialRevision'),(3,'publications'),(4,'generationEpoch'),(5,'materialRevision')):
            damaged=copy.deepcopy(clock_states);damaged[index]['gltfMaterialAuthoring'][key]+=1
            reject('birth-clock-'+str(index)+'-'+key,lambda:append_history_clock_check(damaged,clock_steps))
        step=spec('deleted',26,retired=(3,),history=(1,0));baseline=synthetic_state(spec('before',13));valid=synthetic_state(step)
        mutations=[
            ('retired-set',lambda s:s['gltfMaterialBindings'].update(retiredRuntimeMaterials=[])),
            ('active-count',lambda s:s['gltfMaterialBindings'].update(activeRuntimeMaterialCount=6)),
            ('runtime-incarnation',lambda s:s['gltfMaterialBindings']['runtimeMaterialStates'][3].update(incarnation=999)),
            ('active-source',lambda s:s['gltfMaterialBindings']['materials'][1].update(active=True)),
            ('retained-payload',lambda s:s['gltfMaterialBindings']['materials'][1].update(authored=materials()[1])),
            ('stale-gpu-entry',lambda s:s['gltfMaterialBindings']['materials'][1].update(uploadedMaterialHash=baseline['gltfMaterialBindings']['materials'][1]['uploadedMaterialHash'])),
            ('source-reindexed',lambda s:s['gltfMaterialBindings']['sourceToRuntime'].__setitem__(2,3)),
            ('wrong-draw',lambda s:s['gltfMaterialBindings']['draws'][1].update(runtimeMaterial=4)),
            ('wrong-history',lambda s:s['gltfMaterialVariants'].update(undoCount=0)),
            ('geometry-changed',lambda s:s['scene'].update(primaryVertexCount=9)),
            ('truncated-runtime',lambda s:s['gltfMaterialBindings'].update(runtimeMaterialStatesTruncated=True))]
        for label,mutation in mutations:
            damaged=copy.deepcopy(valid);mutation(damaged)
            try:state_check(damaged,step,baseline)
            except (AssertionError,RuntimeError,ValueError,KeyError):checks.append('reject-'+label)
            else:raise AssertionError('Retirement checker accepted '+label)
        for runtime in (0,1):
            damaged=copy.deepcopy(valid);damaged['gltfMaterialBindings']['runtimeMaterialStates'][runtime]['uploadedMaterialHash']+=1
            reject('source-cohort-default-hash-'+str(runtime),lambda:state_check(damaged,step,baseline))
        empty=synthetic_state(spec('empty',13));uncreated=copy.deepcopy(empty)
        uncreated['gltfMaterialVariants'].pop('undoCount');uncreated['gltfMaterialVariants'].pop('redoCount')
        require(failure_signature(uncreated)==failure_signature(empty),'Exact uncreated/initialized empty journals differ semantically')
        checks.append('empty-journal-owner-initialization-normalizes-only-zero')
        for label,mutation in (
                ('history-one-counter-missing',lambda s:s['gltfMaterialVariants'].pop('undoCount')),
                ('history-other-counter-missing',lambda s:s['gltfMaterialVariants'].pop('redoCount')),
                ('history-null-counter',lambda s:s['gltfMaterialVariants'].update(undoCount=None)),
                ('history-null-pair',lambda s:s['gltfMaterialVariants'].update(undoCount=None,redoCount=None)),
                ('history-bool-counter',lambda s:s['gltfMaterialVariants'].update(undoCount=False)),
                ('history-float-counter',lambda s:s['gltfMaterialVariants'].update(redoCount=0.)),
                ('history-negative-counter',lambda s:s['gltfMaterialVariants'].update(undoCount=-1,redoCount=1)),
                ('history-inconsistent-depth',lambda s:s['gltfScenes'].update(editHistoryDepth=1)),
                ('history-bool-depth',lambda s:s['gltfScenes'].update(editHistoryDepth=False)),
                ('history-float-depth',lambda s:s['gltfScenes'].update(editHistoryDepth=0.)),
                ('history-null-depth',lambda s:s['gltfScenes'].update(editHistoryDepth=None))):
            damaged=copy.deepcopy(empty);mutation(damaged)
            reject(label,lambda:failure_signature(damaged))
        damaged=copy.deepcopy(uncreated);damaged['gltfScenes']['editHistoryDepth']=1
        reject('history-uncreated-nonzero-depth',lambda:failure_signature(damaged))
        journal=synthetic_state(spec('journal',13,history=(1,1)))
        changed=copy.deepcopy(journal);changed['gltfMaterialVariants'].update(undoCount=0,redoCount=2)
        reject('failure-real-cursor-change',lambda:require(failure_signature(changed)==failure_signature(journal),'Actual cursor changed'))
        changed=copy.deepcopy(journal);changed['gltfMaterialVariants'].update(redoCount=0);changed['gltfScenes']['editHistoryDepth']=1
        reject('failure-real-depth-change',lambda:require(failure_signature(changed)==failure_signature(journal),'Actual depth changed'))
        for label,where in (('inert-pbr-extension','pbrMetallicRoughness'),('inert-pbr-extra','pbrMetallicRoughness'),('inert-top-field',None)):
            damaged=copy.deepcopy(valid);record=damaged['gltfMaterialBindings']['materials'][1]['authored']
            if where:
                record[where]['extensions' if label.endswith('extension') else 'extras']={'FUTURE':{'index':7}}
            else:record['futurePayload']={'index':7}
            reject(label,lambda:state_check(damaged,step,baseline))
        history_run=profiles('history')[0];history_states=[synthetic_state(s) for s in history_run['steps']]
        damaged=copy.deepcopy(history_states);bad=damaged[6]['gltfMaterialBindings'];wrong=bad['materials'][1]['uploadedMaterialHash']+1
        bad['materials'][1]['uploadedMaterialHash']=wrong;bad['runtimeMaterialStates'][3]['uploadedMaterialHash']=wrong
        # The current A/E definition is correct and differs from initial rest;
        # only the earlier matching edited capture proves its packed restoration.
        state_check(damaged[6],history_run['steps'][6],history_states[0])
        reject('historical-edited-row-restoration-hash',lambda:catalog_history_check(damaged))
        core_run=profiles('core')[0];core_states=[synthetic_state(s) for s in core_run['steps']]
        damaged=copy.deepcopy(core_states);bad=damaged[-1]['gltfMaterialBindings']
        bad['materials'][-1]['incarnation']=999;bad['runtimeMaterialStates'][-1]['incarnation']=999
        state_check(damaged[-1],core_run['steps'][-1],core_states[0])
        reject('reappearing-append-incarnation',lambda:catalog_history_check(damaged))
        output=Path(folder)/'export-check';output.mkdir();original=output/'source.gltf';write_fixture(original,animated=True)
        model=json.loads(original.read_text());saved_step=spec('saved',39,time=.25,retired=(3,),later=.4,history=(2,0))
        model['materials']=[dict(inert(),name='Renderer default'),dict(inert(),name='Renderer diagnostic'),
            *copy.deepcopy([saved_step['evaluated'][i] for i in (0,2,3)])]
        for mesh in model['meshes']:
            for primitive in mesh['primitives']:
                primitive['material']=2 if primitive['material']==0 else 4
                for mapping in primitive.get('extensions',{}).get(VARIANTS,{}).get('mappings',[]):mapping['material']=4
        model['animations'][0]['channels'][0]['target']['extensions'][POINTER]['pointer']='/materials/4/pbrMetallicRoughness/baseColorFactor'
        for node,x in zip(model['nodes'],CANONICAL_PLACEMENTS):node['translation'][0]=x
        good=output/'dense.gltf';document(good,model);inspect_export(good,saved_step,animated=True);checks.append('dense-export-checker')
        matrix_pose=copy.deepcopy(model)
        for node,x in zip(matrix_pose['nodes'],CANONICAL_PLACEMENTS):
            node.pop('translation');node['matrix']=[1.,0.,0.,0.,0.,1.,0.,0.,0.,0.,1.,0.,x,0.,0.,1.]
        require(CANONICAL_PLACEMENTS!=[-1.1,1.1] and
            np.asarray(CANONICAL_PLACEMENTS,dtype=np.float32).view(np.uint32).tolist()==[0xbf8ccccd,0x3f8ccccd],
            'Canonical pose is not the exact expected binary32 representation')
        matrix_path=output/'dense-binary32-matrix.gltf';document(matrix_path,matrix_pose)
        inspect_export(matrix_path,saved_step,animated=True);checks.append('dense-export-exact-binary32-matrix-pose')
        for side,label in enumerate(('left','right')):
            damaged=copy.deepcopy(matrix_pose);original_x=damaged['nodes'][side]['matrix'][12]
            next_x=float(np.nextafter(np.float32(original_x),np.float32(np.inf)))
            require(abs(next_x-original_x)==abs(float(np.spacing(np.float32(original_x)))),'Negative pose mutation is not exactly one binary32 ULP')
            damaged['nodes'][side]['matrix'][12]=next_x
            path=output/(label+'-one-binary32-ulp.gltf');document(path,damaged)
            reject('export-'+label+'-one-binary32-ulp',lambda:inspect_export(path,saved_step,animated=True))
        export_mutations=[('tombstone-retained',lambda m:m['materials'].append(inert())),
            ('variant-not-remapped',lambda m:m['meshes'][0]['primitives'][0]['extensions'][VARIANTS]['mappings'][0].update(material=5)),
            ('curve-not-remapped',lambda m:m['animations'][0]['channels'][0]['target']['extensions'][POINTER].update(pointer='/materials/5/pbrMetallicRoughness/baseColorFactor')),
            ('opaque-metadata',lambda m:m['extras']['retirementOpaque'].update(material=3)),
            ('positions',lambda m:m['nodes'][0]['translation'].__setitem__(0,-1.)),
            ('missing-pointer-path',lambda m:m['animations'][0]['channels'][0]['target'].pop('path')),
            ('wrong-pointer-path',lambda m:m['animations'][0]['channels'][0]['target'].update(path='translation')),
            ('pointer-node',lambda m:m['animations'][0]['channels'][0]['target'].update(node=0)),
            ('negative-pointer-node',lambda m:m['animations'][0]['channels'][0]['target'].update(node=-1))]
        for label,mutation in export_mutations:
            damaged=copy.deepcopy(model);mutation(damaged);path=output/(label+'.gltf');document(path,damaged)
            try:inspect_export(path,saved_step,animated=True)
            except (AssertionError,RuntimeError,ValueError,KeyError):checks.append('reject-export-'+label)
            else:raise AssertionError('Export checker accepted '+label)
        for label,variant,other_scene,reference in (
                ('orphan-default-out-of-range',False,False,5),('orphan-default-stale-in-range',False,False,3),
                ('orphan-variant-out-of-range',True,False,5),('orphan-variant-stale-in-range',True,False,3),
                ('other-scene-default-out-of-range',False,True,5),('other-scene-default-stale-in-range',False,True,3),
                ('other-scene-variant-out-of-range',True,True,5),('other-scene-variant-stale-in-range',True,True,3)):
            damaged=copy.deepcopy(model);mesh=copy.deepcopy(damaged['meshes'][0])
            if variant:mesh['primitives'][0]['extensions'][VARIANTS]['mappings'][0]['material']=reference
            else:mesh['primitives'][0]['material']=reference
            damaged['meshes'].append(mesh)
            if other_scene:
                damaged['nodes'].append(dict(mesh=len(damaged['meshes'])-1));damaged['scenes'].append(dict(nodes=[len(damaged['nodes'])-1]))
            path=output/(label+'.gltf');document(path,damaged)
            reject('export-'+label,lambda:inspect_export(path,saved_step,animated=True))
        for label,mutation in (
                ('other-scene-node-mesh',lambda m:(m['nodes'].append(dict(mesh=99)),m['scenes'].append(dict(nodes=[len(m['nodes'])-1])))),
                ('other-scene-root',lambda m:m['scenes'].append(dict(nodes=[99]))),
                ('orphan-node-cycle',lambda m:m['nodes'].append(dict(children=[len(m['nodes'])])))):
            damaged=copy.deepcopy(model);mutation(damaged);path=output/(label+'.gltf');document(path,damaged)
            reject('export-'+label,lambda:inspect_export(path,saved_step,animated=True))
        steps=profiles('runtime')[0]['steps'];baseline=synthetic_runtime_state(steps[0]);valid=synthetic_runtime_state(steps[2])
        runtime_mutations=[
            ('runtime-selected-hash',lambda s:s['gltfMaterialBindings']['runtimeMaterialStates'][2].update(uploadedMaterialHash=1002)),
            ('runtime-other-hash',lambda s:s['gltfMaterialBindings']['runtimeMaterialStates'][0].update(uploadedMaterialHash=2000)),
            ('runtime-fabricated-source',lambda s:s['gltfMaterialBindings']['runtimeMaterialStates'][2].update(sourceMaterial=0)),
            ('runtime-wrong-primitive',lambda s:s['gltfMerge']['authoringNodes'][3].update(primitiveIndex=1)),
            ('runtime-wrong-binding',lambda s:s['gltfMerge']['authoringNodes'][3].update(materialIndex=2)),
            ('runtime-not-inert',lambda s:s['gltfMaterialBindings']['runtimeMaterialStates'][2]['materialProperties'].update(unlit=True)),
            ('runtime-stale-incarnation',lambda s:s['gltfMaterialBindings']['runtimeMaterialStates'][2].update(incarnation=999)),
            ('runtime-pending-gesture',lambda s:s['gltfMaterialAuthoring'].update(pendingGesture=True))]
        for label,mutation in runtime_mutations:
            damaged=copy.deepcopy(valid);mutation(damaged)
            try:runtime_state_check(damaged,steps[2],baseline)
            except (AssertionError,RuntimeError,ValueError,KeyError):checks.append('reject-'+label)
            else:raise AssertionError('Runtime checker accepted '+label)
        lighting=dict(environmentIntensity=0.,directionalIntensity=1.,directionalDirection=[0.,0.,-1.],
            directionalColor=[1.,1.,1.],pointLightCount=0,areaLightCount=0)
        runtime_lighting_check(lighting);checks.append('runtime-supported-lit-lighting')
        for label,patch in (('dark',dict(directionalIntensity=0.)),('environment',dict(environmentIntensity=.1)),
                ('wrong-direction',dict(directionalDirection=[0.,0.,1.])),('colored',dict(directionalColor=[.5,1.,1.])),
                ('point-light',dict(pointLightCount=1)),('area-light',dict(areaLightCount=1)),('bool-intensity',dict(directionalIntensity=True))):
            damaged=copy.deepcopy(lighting);damaged.update(patch)
            reject('runtime-lighting-'+label,lambda:runtime_lighting_check(damaged))
        pixels=np.zeros((HEIGHT,WIDTH,3),dtype=np.uint8);pixels[165:195,135:155]=[180,25,15]
        pixels[165:195,310:330]=[20,170,35];pixels[165:195,485:505]=[210,215,220]
        before=output/'runtime-before.png';Image.fromarray(pixels).save(before)
        after_pixels=pixels.copy();after_pixels[165:195,485:505]=[180,25,15]
        after=output/'runtime-after.png';Image.fromarray(after_pixels).save(after)
        runtime_visible(dict(image=str(before)));runtime_positive(dict(image=str(before)),dict(image=str(after)))
        runtime_unchanged(dict(image=str(after)),dict(image=str(after)));checks.append('runtime-image-checkers-positive')
        damaged=after_pixels.copy();damaged[0,0]=[255,0,255];bad=output/'runtime-damaged.png';Image.fromarray(damaged).save(bad)
        try:runtime_unchanged(dict(image=str(bad)),dict(image=str(after)))
        except (AssertionError,RuntimeError):checks.append('reject-runtime-full-image-change')
        else:raise AssertionError('Runtime full-image checker accepted outside-ROI change')
    return dict(passed=True,checks=checks,cohorts=counts,nativeValidationPerformed=False,unverified=UNVERIFIED)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path);parser.add_argument('--renderer',type=Path);parser.add_argument('--manifest',type=Path)
    parser.add_argument('--technique',choices=TECHNIQUES,action='append');parser.add_argument('--cohort',choices=COHORTS,default='core')
    parser.add_argument('--timeout',type=float,default=600.);parser.add_argument('--self-check',action='store_true');parser.add_argument('--generate-only',action='store_true')
    args=parser.parse_args()
    if args.self_check:print(json.dumps(self_check(),indent=2));return
    if args.output is None:parser.error('--output is required')
    if not math.isfinite(args.timeout) or args.timeout<=0:parser.error('--timeout must be finite and positive')
    if not args.generate_only and (args.renderer is None or args.manifest is None):parser.error('Native run requires explicit --renderer and --manifest')
    args.technique=tuple(dict.fromkeys(args.technique or TECHNIQUES))
    if args.renderer:args.renderer=args.renderer.resolve()
    if args.manifest:args.manifest=args.manifest.resolve()
    output=args.output.resolve();plan=generate(output,args.cohort)
    report=dict(schemaVersion=1,cohort=args.cohort,passed=None if args.generate_only else False,generatedOnly=args.generate_only,
        rendererExecuted=False,nativeValidationPerformed=False,sourceDependencies=dependencies(),launchPlanSha256=sha(output/'launch-plan.json'),
        expectedCaptureCount=plan['expectedCaptureCount']*len(args.technique),expectedProcessCount=plan['expectedProcessCount']*len(args.technique),
        captures=[],processes=[],stateChecks=[],imageChecks=[],exports=[],portableCopies=[],unverified=UNVERIFIED)
    if args.generate_only:document(output/'results.json',report);print('GENERATED',report['expectedCaptureCount'],'planned captures; renderer not executed')
    else:
        run_native(args,output,plan,report)
        print('PASS',len(report['captures']),'captures;',len(report['stateChecks']),'CPU/publication checks;',len(report['imageChecks']),'image checks')


if __name__=='__main__':main()
