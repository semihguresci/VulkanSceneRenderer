"""Scene changes retain authoring pose/material edits and retire view state."""
from pathlib import Path
import argparse
import json
import hashlib

from gltf_accessor_runtime_regression import Asset, require, load_asset
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_scenes_runtime_regression import NAMES, SCENES, assert_pick, capture, fixture
from gltf_material_variants_runtime_regression import pixels

CAMERA=[dict(frame=1,position=[0,0,4],target=[0,0,0])]
RIGHT_PICK_PIXEL=[391,180]
INVALID_NODE=2**32-1
# frame -> active scene, selected source camera, shared undo/redo counts
SAMPLES={1:(1,6,1,0),3:(1,6,2,0),5:(1,6,2,1),7:(0,-1,0,0),
         8:(0,5,0,0),9:(0,5,0,0),11:(1,-1,0,0),13:(2,-1,0,0)}

def source(path):
    model=fixture(path)
    a=Asset();a.model=model
    # Reuse the existing embedded payload, then append bounded animation data.
    import base64
    a.data=bytearray(base64.b64decode(model['buffers'][0]['uri'].split(',',1)[1]))
    times=a.accessor('SCALAR',[0,1])
    translated=a.accessor('VEC3',[[-.55,0,0],[-.55,.2,0]])
    camera=a.accessor('VEC3',[[0,0,4],[0,0,4]])
    model['animations']=[dict(name='Persistent pose',samplers=[
        dict(input=times,output=translated),dict(input=times,output=camera)],channels=[
        dict(sampler=0,target=dict(node=1,path='translation')),
        dict(sampler=1,target=dict(node=5,path='translation'))])]
    model['materials'].append(dict(pbrMetallicRoughness=dict(baseColorFactor=[.02,1,.02,1]),
        extensions=dict(KHR_materials_unlit={})))
    model['extensionsUsed'].append('KHR_materials_variants')
    model['extensions']['KHR_materials_variants']=dict(variants=[dict(name='Green')])
    for mesh in model['meshes']:
        mesh['primitives'][0].setdefault('extensions',{})['KHR_materials_variants']=dict(mappings=[dict(material=2,variants=[0])])
    model['scene']=1
    a.write(path)


def translation_matrix(x):
    return [1,0,0,0, 0,1,0,0, 0,0,1,0, x,0,0,1]


def transform(frame,x):
    return dict(frame=frame,nodeTransform=dict(node=dict(sourceNode=2),matrix=translation_matrix(x)))


def startup_override_reload(renderer,output,technique,startup,oracle_path,oracle,report):
    sequence=output/f'{technique}-startup-override-reload.json'
    sequence.write_text(json.dumps(dict(schemaVersion=1,frames=4,sampleFrames=[1,4],camera=CAMERA,
        events=[dict(frame=2,reload=str(oracle_path))]),indent=2))
    records=capture(renderer,output,startup,technique+'-startup-override-reload',technique,'lit',report['captures'],
        sequence=sequence,gltf_scene=1)
    require(len(records)==2,'Missing startup override/reload samples')
    before,after=(record['state']['gltfScenes'] for record in records)
    require(before['selected']==1 and before['names']==NAMES and before['activeSourceNodes']==SCENES[1],
            'Startup CLI override did not select Right over the authored Left default')
    report['checks'].append(dict(technique=technique,check='startup-only-cli-scene',scene=before['selected']))
    require(after['selected']==0 and after['names']==['Both'] and after['activeRenderableCount']==3 and
            after['graphIdentity']!=before['graphIdentity'],'Model reload did not load the replacement scene default')
    error=pixels(records[1]['image'],oracle['image'])
    require(error['maximumError']==0,f'Replacement scene differs from its independent startup capture: {error}')
    require(pixels(records[0]['image'],records[1]['image'])['meanAbsoluteError']>.5,'Reload witness is visually vacuous')
    report['checks'].append(dict(technique=technique,check='replacement-authored-default',scene=after['selected'],**error))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--renderer',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--manifest',type=Path);args=p.parse_args()
    renderer=args.renderer.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    sha=lambda path:hashlib.sha256(Path(path).read_bytes()).hexdigest()
    report=dict(passed=False,rendererSha256=sha(renderer),captures=[],checks=[])
    path=output/'state.gltf';source(path)
    startup=output/'startup-left-default.gltf'
    startup_model,_=load_asset(path);startup_model['scene']=0
    startup.write_text(json.dumps(startup_model,indent=2))
    sequence=output/'state-sequence.json'
    sequence.write_text(json.dumps(dict(schemaVersion=1,frames=max(SAMPLES),sampleFrames=list(SAMPLES),camera=CAMERA,
        events=[dict(frame=1,materialVariant=0),transform(2,.65),
                dict(frame=3,viewportPointer=dict(action='select',pixel=RIGHT_PICK_PIXEL),depthProbe=RIGHT_PICK_PIXEL),
                transform(4,.75),dict(frame=5,sceneHistory='undo'),dict(frame=6,gltfScene=0),
                dict(frame=8,cameraSelection=dict(sourceNode=5)),
                dict(frame=9,cameraSelection=dict(sourceNode=6),expectCameraSelectionFailure=True),
                dict(frame=10,gltfScene=1),dict(frame=12,gltfScene=2)]),indent=2))
    try:
        if args.manifest:report['nativeManifestBefore']=verify_runtime_manifest(args.manifest,renderer)
        for technique in ['deferred-raster','forward-raster']:
            saved=output/(technique+'-saved.glb')
            records=capture(renderer,output,path,technique+'-state',technique,'lit',report['captures'],sequence=sequence,save=saved,
                extra=['--gltf-camera-node','6','--animation-clip','0','--animation-time','.5','--animation-speed','0'])
            require(len(records)==len(SAMPLES),'Missing state sequence samples')
            by_frame=dict(zip(SAMPLES,records))
            require(Path(records[0]['log']).read_text(encoding='utf-8',errors='replace').count(
                'Expected capture camera selection failure:')==1,'Inactive camera rejection was not witnessed')
            first_pose=None;selected_mesh=None
            for frame,(selected,camera,undo,redo) in SAMPLES.items():
                record=by_frame[frame]
                state=record['state'];view=state['gltfScenes'];animation=state['gltfAnimation']
                require(view['selected']==selected and view['activeSourceNodes']==SCENES[selected],'Wrong scene state')
                require(animation['selectedClip']==0 and abs(animation['time']-.5)<1e-6 and animation['playing']==(frame<7),
                    'Scene change lost clip/pose or failed to pause playback')
                require(animation['selectedCameraNode']==camera and
                    state['gltfNodeFlags']['cameraSourceNodes']==[i for i in (5,6) if i in SCENES[selected]],
                    f'Stale active camera list or authored camera selection at frame {frame}')
                require(state['sceneEditHistory']==dict(undo=undo,redo=redo) and view['editHistoryDepth']==undo+redo,
                    f'Shared history did not record edits/undo or clear on switch at frame {frame}')
                require(state['gltfMaterialVariants']['selected']==0,'Scene switch lost global material selection')
                if frame==3:
                    pick=assert_pick(state,selected,2,RIGHT_PICK_PIXEL)
                    selected_mesh=pick['gpu']['selection']['graphNode']
                    require(state['gui']['selectedMeshNodeId']==selected_mesh,'Viewport pointer did not select the GPU-picked node')
                elif frame==5:
                    require(state['gui']['selectedMeshNodeId']==selected_mesh,'Transform undo lost the viewport selection')
                elif frame>=7:
                    require(state['gui']['selectedMeshNodeId']==INVALID_NODE,'Scene change retained the viewport selection')
                right=next(node for node in state['gltfMerge']['authoringNodes'] if node['sourceNode']==2)
                expected=translation_matrix(.55 if frame==1 else .65)
                for key in ('localColumns','worldColumns'):
                    require(all(abs(a-b)<1e-5 for a,b in zip(sum(right[key],[]),expected)),
                        f'Scene switch or undo lost the authored Right transform at frame {frame}: {right[key]}')
                pose=next(n['localMatrix'] for n in animation['nodes'] if n['sourceIndex']==1)
                if first_pose is None:first_pose=pose
                require(pose==first_pose,'Switching scenes changed the retained authoring correction or pose')
                require(abs(pose[12]+.55)<1e-5 and abs(pose[13]-.1)<1e-5,'Clip pose was not exercised')
                report['checks'].append(dict(technique=technique,frame=frame,scene=selected,clipTime=animation['time'],
                    playing=animation['playing'],selectedCamera=camera,history=state['sceneEditHistory'],retainedLocalMatrix=pose))
            before_reject,after_reject=(by_frame[frame] for frame in (8,9))
            require(before_reject['state']['camera']==after_reject['state']['camera'] and
                pixels(before_reject['image'],after_reject['image'])['maximumError']==0,'Inactive camera changed the rendered view')
            model,_=load_asset(saved);require(len(model['scenes'])==4 and model['scene']==2,'State save discarded scenes or default')
            oracle_model,_=load_asset(path)
            oracle_model.pop('animations')
            oracle_model['scenes']=[oracle_model['scenes'][2]];oracle_model['scene']=0
            oracle_model['nodes'][1]['translation']=[-.55,.1,0]
            oracle_model['nodes'][2]['translation']=[.65,0,0]
            for material in oracle_model['materials']:
                material['pbrMetallicRoughness']['baseColorFactor']=[.02,1,.02,1]
            oracle_path=output/(technique+'-baked-oracle.gltf');oracle_path.write_text(json.dumps(oracle_model,indent=2))
            oracle=capture(renderer,output,oracle_path,technique+'-edited-pose-oracle',technique,'lit',report['captures'])[0]
            reference_error=pixels(records[-1]['image'],oracle['image'])
            require(reference_error['maximumError']==0,f'Retained object edit/animated pose differs from independent baked scene {reference_error}')
            report['checks'].append(dict(technique=technique,check='independent-edited-pose',**reference_error))
            reload=capture(renderer,output,saved,technique+'-state-reload',technique,'lit',report['captures'],
                extra=['--animation-clip','0','--animation-time','.5','--animation-paused'])[0]
            error=pixels(records[-1]['image'],reload['image']);require(error['maximumError']==0,f'Edited/animated multi-scene export mismatch {error}')
            report['checks'].append(dict(technique=technique,check='edited-animated-all-scenes-export',**error))
            startup_override_reload(renderer,output,technique,startup,oracle_path,oracle,report)
            print('PASS scene state',technique,flush=True)
        require(sha(renderer)==report['rendererSha256'],'Renderer changed')
        if args.manifest:
            report['nativeManifestAfter']=verify_runtime_manifest(args.manifest,renderer)
            require(report['nativeManifestBefore']==report['nativeManifestAfter'],'Native freeze manifest changed during capture')
        report['passed']=True
    except Exception as error:report['failure']=str(error);raise
    finally:
        report.update(captureCount=len(report['captures']),checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS',len(report['captures']),'captures',len(report['checks']),'checks',flush=True)

if __name__=='__main__':main()
