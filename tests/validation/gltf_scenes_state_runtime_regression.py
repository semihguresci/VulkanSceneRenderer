"""Scene changes retain authoring pose/material edits and retire view state."""
from pathlib import Path
import argparse
import json
import hashlib

from gltf_accessor_runtime_regression import Asset, require, load_asset
from gltf_scenes_runtime_regression import fixture
from gltf_material_variants_runtime_regression import run,pixels
from gltf_texture_formats_runtime_regression import verify_manifest

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

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--renderer',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--manifest',type=Path);args=p.parse_args()
    renderer=args.renderer.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    sha=lambda path:hashlib.sha256(Path(path).read_bytes()).hexdigest()
    report=dict(passed=False,rendererSha256=sha(renderer),captures=[],checks=[])
    if args.manifest:report['nativeManifestBefore']=verify_manifest(args.manifest)
    path=output/'state.gltf';source(path)
    sequence=output/'state-sequence.json'
    sequence.write_text(json.dumps(dict(schemaVersion=1,frames=7,sampleFrames=[1,3,5,7],
        camera=[dict(frame=1,position=[0,0,4],target=[0,0,0])],
        objectName='Right blue',objectTranslation=[dict(frame=1,translation=[.1,0,0])],
        events=[dict(frame=1,materialVariant=0),dict(frame=2,gltfScene=0),
                dict(frame=4,gltfScene=1),dict(frame=6,gltfScene=2)]),indent=2))
    try:
        for technique in ['deferred-raster','forward-raster']:
            saved=output/(technique+'-saved.glb')
            records=run(renderer,output,path,technique+'-state',technique,'lit',report['captures'],sequence=sequence,save=saved,
                extra=['--gltf-camera-node','6','--animation-clip','0','--animation-time','.5','--animation-speed','0'])
            require(len(records)==4,'Missing state sequence samples')
            first_pose=None
            for index,(record,selected) in enumerate(zip(records,[1,0,1,2])):
                state=record['state'];view=state['gltfScenes'];animation=state['gltfAnimation']
                require(view['selected']==selected,'Wrong scene state')
                require(animation['selectedClip']==0 and abs(animation['time']-.5)<1e-6 and animation['playing']==(index==0),
                    'Scene change lost clip/pose or failed to pause playback')
                require(animation['selectedCameraNode']==(6 if index==0 else -1),'Scene change retained authored camera selection')
                require(view['editHistoryDepth']==(1 if index==0 else 0),'Scene change retained history or skipped initial edit')
                require(state['gltfMaterialVariants']['selected']==0,'Scene switch lost global material selection')
                pose=next(n['localMatrix'] for n in animation['nodes'] if n['sourceIndex']==1)
                if first_pose is None:first_pose=pose
                require(pose==first_pose,'Switching scenes changed the retained authoring correction or pose')
                require(abs(pose[12]+.55)<1e-5 and abs(pose[13]-.1)<1e-5,'Clip pose was not exercised')
                report['checks'].append(dict(technique=technique,scene=selected,clipTime=animation['time'],playing=animation['playing'],
                    selectedCamera=animation['selectedCameraNode'],historyDepth=view['editHistoryDepth'],retainedLocalMatrix=pose))
            model,_=load_asset(saved);require(len(model['scenes'])==4,'State save discarded scenes')
            oracle_model,_=load_asset(path)
            oracle_model.pop('animations')
            oracle_model['scenes']=[oracle_model['scenes'][2]];oracle_model['scene']=0
            oracle_model['nodes'][1]['translation']=[-.55,.1,0]
            oracle_model['nodes'][2]['translation']=[.65,0,0]
            for material in oracle_model['materials']:
                material['pbrMetallicRoughness']['baseColorFactor']=[.02,1,.02,1]
            oracle_path=output/(technique+'-baked-oracle.gltf');oracle_path.write_text(json.dumps(oracle_model,indent=2))
            oracle=run(renderer,output,oracle_path,technique+'-edited-pose-oracle',technique,'lit',report['captures'])[0]
            reference_error=pixels(records[-1]['image'],oracle['image'])
            require(reference_error['maximumError']==0,f'Retained object edit/animated pose differs from independent baked scene {reference_error}')
            report['checks'].append(dict(technique=technique,check='independent-edited-pose',**reference_error))
            reload=run(renderer,output,saved,technique+'-state-reload',technique,'lit',report['captures'],gltf_scene=2,
                extra=['--animation-clip','0','--animation-time','.5','--animation-paused'])[0]
            error=pixels(records[-1]['image'],reload['image']);require(error['maximumError']==0,f'Edited/animated multi-scene export mismatch {error}')
            report['checks'].append(dict(technique=technique,check='edited-animated-all-scenes-export',**error))
            print('PASS scene state',technique,flush=True)
        require(sha(renderer)==report['rendererSha256'],'Renderer changed')
        if args.manifest:report['nativeManifestAfter']=verify_manifest(args.manifest)
        report['passed']=True
    except Exception as error:report['failure']=str(error);raise
    finally:
        report.update(captureCount=len(report['captures']),checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS',len(report['captures']),'captures',len(report['checks']),'checks',flush=True)

if __name__=='__main__':main()
