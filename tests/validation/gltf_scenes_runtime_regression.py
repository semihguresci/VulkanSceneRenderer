"""Multi-scene CLI/default/switch/shared-resource and portable save witnesses."""
from pathlib import Path
import argparse
import copy
import hashlib
import json

from gltf_accessor_runtime_regression import Asset, mesh, load_asset, require
from gltf_material_variants_runtime_regression import run, pixels
from gltf_texture_formats_runtime_regression import verify_manifest

SCENES={0:[0,1,3,5],1:[0,2,4,6],2:[0,1,2,3,4,5,6],3:[]}
SAMPLES={1:1,3:0,5:2,7:3,9:1,11:0,13:2}

def fixture(path):
    a=Asset()
    mesh(a,a.accessor('VEC3',[[-.35,-.5,0],[.35,-.5,0],[.35,.5,0],[-.35,.5,0]]),
         a.accessor('VEC3',[[0,0,1]]*4),a.accessor('VEC2',[[0,0],[1,0],[1,1],[0,1]]))
    a.model['meshes'].append(copy.deepcopy(a.model['meshes'][0]))
    a.model['meshes'][1]['primitives'][0]['material']=1
    a.model['materials']=[dict(pbrMetallicRoughness=dict(baseColorFactor=color),
        extensions=dict(KHR_materials_unlit={})) for color in ([1,.02,.02,1],[.02,.05,1,1])]
    a.model['nodes']=[dict(name='Selection anchor',mesh=0,translation=[20,0,0]),
        dict(name='Left red',mesh=0,translation=[-.55,0,0]),
        dict(name='Right blue',mesh=1,translation=[.55,0,0]),
        dict(name='Red light',translation=[-1,1,2],extensions=dict(KHR_lights_punctual=dict(light=0))),
        dict(name='Blue light',translation=[1,1,2],extensions=dict(KHR_lights_punctual=dict(light=1))),
        dict(name='Left camera',camera=0,translation=[0,0,4]),
        dict(name='Right camera',camera=1,translation=[0,0,5])]
    a.model['cameras']=[dict(type='perspective',perspective=dict(yfov=.785398163, znear=.1)) for _ in range(2)]
    a.model['extensionsUsed']=['KHR_materials_unlit','KHR_lights_punctual']
    a.model['extensions']=dict(KHR_lights_punctual=dict(lights=[
        dict(type='point',color=[1,0,0],intensity=2),dict(type='point',color=[0,0,1],intensity=4)]))
    a.model['scenes']=[dict(name=name,nodes=SCENES[i],extras=dict(sceneIdentity=i))
        for i,name in enumerate(['Left','Right','Both','Empty'])]
    a.model['scene']=1
    a.write(path)
    return a.model

def assert_state(state,selected):
    view=state['gltfScenes'];expected=SCENES[selected]
    require(view['selected']==selected and view['names']==['Left','Right','Both','Empty'],'Wrong scene/default/name selection')
    require(sorted(view['activeSourceNodes'])==expected,'Stale source groups after scene switch')
    draws=sum(i in expected for i in (0,1,2))
    require(view['activeRenderableCount']==draws and state['scene']['primaryObjectCount']==draws,'Stale renderable view')
    require(view['pointLightCount']==sum(i in expected for i in (3,4)),'Stale authored lights')
    lights=state['gltfAnimation']['pointLights']
    require(len(lights)==sum(i in expected for i in (3,4)),'Stale uploaded point-light table')
    for light,index in zip(lights,[i for i in (3,4) if i in expected]):
        require(light['position']==([-1,1,2] if index==3 else [1,1,2]) and
                light['color']==([1,0,0] if index==3 else [0,0,1]) and light['intensity']==(2 if index==3 else 4),
                'Authored light ordinal identity changed during publication')
    require(not state['gltfAnimation']['playing'] and state['gltfAnimation']['selectedCameraNode']==-1,'Playback/camera state not reset')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--renderer',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--manifest',type=Path);p.add_argument('--technique',action='append',choices=['deferred-raster','forward-raster'])
    args=p.parse_args();renderer=args.renderer.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    sha=lambda path:hashlib.sha256(Path(path).read_bytes()).hexdigest()
    report=dict(passed=False,rendererSha256=sha(renderer),captures=[],checks=[])
    def save():
        report.update(captureCount=len(report['captures']),checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    if args.manifest:report['nativeManifestBefore']=verify_manifest(args.manifest)
    source=output/'multi-scene.gltf';model=fixture(source)
    sequence=output/'switches.json'
    sequence.write_text(json.dumps(dict(schemaVersion=1,frames=13,sampleFrames=list(SAMPLES),
        camera=[dict(frame=1,position=[0,0,4],target=[0,0,0])],
        events=[dict(frame=frame-1,gltfScene=scene) for frame,scene in SAMPLES.items() if frame>1]),indent=2))
    try:
        for technique in args.technique or ['deferred-raster','forward-raster']:
            oracles={}
            for selected,roots in SCENES.items():
                # Prune the node table to form a single-scene independent oracle.
                oracle=copy.deepcopy(model);oracle['nodes']=[copy.deepcopy(model['nodes'][i]) for i in roots]
                oracle['scenes']=[dict(nodes=list(range(len(roots))))];oracle['scene']=0
                path=output/f'oracle-{selected}.gltf';path.write_text(json.dumps(oracle,indent=2))
                oracles[selected]=run(renderer,output,path,f'{technique}-oracle-{selected}',technique,'lit',report['captures'])[0]
            negative=pixels(oracles[0]['image'],oracles[1]['image'])
            require(negative['meanAbsoluteError']>.5,'Distinct-scene oracle is vacuous')
            for selected in (0,1):
                record=run(renderer,output,source,f'{technique}-cli-{selected}',technique,'lit',report['captures'],gltf_scene=selected)[0]
                assert_state(record['state'],selected)
                error=pixels(record['image'],oracles[selected]['image'])
                require(error['maximumError']==0,f'CLI scene selection mismatch: {error}')
                report['checks'].append(dict(technique=technique,check='cli-scene',scene=selected,**error))
            destination=output/f'{technique}-saved.glb'
            captured=run(renderer,output,source,f'{technique}-switches',technique,'lit',report['captures'],sequence=sequence,save=destination)
            require(len(captured)==len(SAMPLES),'Missing scene-switch captures')
            graph=None;source_vertices=None;authoring=None;previous_epoch=None
            for record,(frame,selected) in zip(captured,sorted(SAMPLES.items())):
                state=record['state'];assert_state(state,selected)
                view=state['gltfScenes'];identity=view['graphIdentity']
                if graph is None:
                    graph=identity;source_vertices=state['gltfDeformation']['sourceVertexCount'];authoring=view['authoringNodeCount']
                require(identity==graph and view['authoringNodeCount']==authoring and
                        state['gltfDeformation']['sourceVertexCount']==source_vertices,'Scene switch replaced authoring/shared geometry')
                epoch=state['taa']['epoch']
                require(previous_epoch is None or epoch>previous_epoch,'Scene switch retained temporal history')
                previous_epoch=epoch
                error=pixels(record['image'],oracles[selected]['image'])
                require(error['maximumError']==0,f'Scene switch image mismatch frame {frame}: {error}')
                report['checks'].append(dict(technique=technique,frame=frame,scene=selected,**error))
            exported,_=load_asset(destination)
            require(len(exported['scenes'])==4 and exported['scene']==2,'Save discarded scenes or selected default')
            require([s['extras']['sceneIdentity'] for s in exported['scenes']]==[0,1,2,3],'Scene metadata lost')
            incoming=[0]*len(exported['nodes'])
            for node in exported['nodes']:
                for child in node.get('children',[]):incoming[child]+=1
            require(max(incoming,default=0)<=1 and all(incoming[root]==0 for s in exported['scenes'] for root in s.get('nodes',[])),
                    'Export violated global strict forest')
            ascii_path=destination.with_suffix('.gltf')
            for selected in SCENES:
                # Existing capture helper has no scene option; pass a constant
                # scene event in a one-sample sequence to exercise saved scenes.
                select=output/f'select-{selected}.json'
                select.write_text(json.dumps(dict(schemaVersion=1,frames=3,sampleFrames=[3],
                    camera=[dict(frame=1,position=[0,0,4],target=[0,0,0])],events=[dict(frame=1,gltfScene=selected)])))
                record=run(renderer,output,destination,f'{technique}-glb-scene-{selected}',technique,'lit',report['captures'],sequence=select,
                    save=ascii_path if selected==0 else None)[0]
                error=pixels(record['image'],oracles[selected]['image'])
                require(error['maximumError']==0,f'Saved scene {selected} changed appearance: {error}')
                report['checks'].append(dict(technique=technique,check='portable-scene-reload',scene=selected,**error))
            text,_=load_asset(ascii_path)
            require(len(text['scenes'])==4 and [s['extras']['sceneIdentity'] for s in text['scenes']]==[0,1,2,3],
                    'ASCII export lost inactive scenes or metadata')
            for selected in SCENES:
                record=run(renderer,output,ascii_path,f'{technique}-ascii-scene-{selected}',technique,'lit',report['captures'],
                    sequence=output/f'select-{selected}.json')[0]
                error=pixels(record['image'],oracles[selected]['image'])
                require(error['maximumError']==0,f'ASCII scene {selected} changed appearance: {error}')
                report['checks'].append(dict(technique=technique,check='ascii-scene-reload',scene=selected,**error))
            save();print('PASS scenes',technique,flush=True)
        require(sha(renderer)==report['rendererSha256'],'Renderer changed during capture')
        if args.manifest:report['nativeManifestAfter']=verify_manifest(args.manifest)
        report['passed']=True
    except Exception as error:
        report['failure']=str(error);raise
    finally:save()
    print('PASS',len(report['captures']),'captures',len(report['checks']),'checks',flush=True)

if __name__=='__main__':main()
