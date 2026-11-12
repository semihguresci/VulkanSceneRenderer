"""Multi-scene CLI/default/switch/shared-resource and portable save witnesses."""
from functools import partial
from pathlib import Path
import argparse
import copy
import hashlib
import json

from gltf_accessor_runtime_regression import Asset, mesh, load_asset, require
from gltf_inactive_scene_capacity_runtime_regression import verify_runtime_manifest
from gltf_material_variants_runtime_regression import run, pixels

SCENES={0:[0,1,3,5],1:[0,2,4,6],2:[0,1,2,3,4,5,6],3:[]}
NAMES=['Left','Right','Both','Empty']
SAMPLES={1:1,3:0,5:2,7:3,9:1,11:0,13:2}
# A 45-degree camera four units from the quads projects their centers to
# x=260 and x=380. Probe the same pixels both with and without their owner.
PICK_PIXELS={1:[260,180],2:[380,180]}
PROBES={1:1,3:1,5:2,7:2,9:2,11:2,13:1}
capture=partial(run,require_validation=True)

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
        for i,name in enumerate(NAMES)]
    a.model['scene']=1
    a.write(path)
    return a.model

def assert_state(state,selected):
    view=state['gltfScenes'];expected=SCENES[selected]
    require(view['selected']==selected and view['names']==NAMES,'Wrong scene/default/name selection')
    require(sorted(view['activeSourceNodes'])==expected,'Stale source groups after scene switch')
    draws=sum(i in expected for i in (0,1,2))
    require(view['activeRenderableCount']==draws and state['scene']['primaryObjectCount']==draws,'Stale renderable view')
    require(state['gltfInstancing']['drawCount']==draws,'Stale provider draw count')
    require(view['pointLightCount']==sum(i in expected for i in (3,4)),'Stale authored lights')
    lights=state['gltfAnimation']['pointLights']
    require(len(lights)==sum(i in expected for i in (3,4)),'Stale uploaded point-light table')
    for light,index in zip(lights,[i for i in (3,4) if i in expected]):
        require(light['position']==([-1,1,2] if index==3 else [1,1,2]) and
                light['color']==([1,0,0] if index==3 else [0,0,1]) and light['intensity']==(2 if index==3 else 4),
                'Authored light ordinal identity changed during publication')
    require(state['gltfNodeFlags']['cameraSourceNodes']==[i for i in (5,6) if i in expected],
            'Stale active camera list')
    require(not state['gltfAnimation']['playing'] and state['gltfAnimation']['selectedCameraNode']==-1,'Playback/camera state not reset')
    bounds=state['gltfInstancing']['modelBounds']
    require(bounds['valid']==bool(draws),'Scene did not publish the expected bounds validity')
    if draws:
        expected_bounds=[-.9 if 1 in expected else .2,-.5,0,20.35,.5,0]
        require(all(abs(a-b)<1e-5 for a,b in zip(bounds['min']+bounds['max'],expected_bounds)),
                f'Stale scene bounds: {bounds}')


def assert_pick(state,selected,owner,pixel):
    probe=state['depthProbe']
    require(probe['pixel']==pixel,f'Wrong scene pick sample pixel: {probe}')
    interaction=probe['sceneInteraction']
    gpu_available=selected!=3 or state['technique']=='deferred-raster'
    raw_id=interaction['rawGpuId']
    if gpu_available:
        require(type(raw_id) is int and interaction['gpu'] is not None,f'GPU pick unavailable: {interaction}')
    else:
        # The forward pick pass skips empty views. Deferred GBuffer clears PickId.
        require(raw_id is None and interaction['gpu'] is None,f'Forward pick sampled a stale ID: {interaction}')
    expected=None
    if owner in SCENES[selected]:
        nodes=state['gltfMerge']['authoringNodes']
        owner_node=next(node['graphNode'] for node in nodes if node['sourceNode']==owner)
        primitive=next(node for node in nodes if node['meshOwnerGraphNode']==owner_node and node['renderable'])
        expected=(primitive['graphNode'],owner,owner-1,0,-1)
        require(raw_id>0 and probe['opaque']>0,f'Active scene did not write its ID and depth: {probe}')
    elif gpu_available:
        require(raw_id==0 and probe['opaque']==0,f'Inactive scene wrote an ID or depth: {probe}')
    for route in ('cpu','gpu') if gpu_available else ('cpu',):
        for action in ('hit','selection','hover'):
            hit=interaction[route][action]
            identity=None if hit is None else tuple(hit[key] for key in
                ('graphNode','sourceNode','sourceMesh','sourcePrimitive','sourceInstance'))
            require(identity==expected,f'Scene {selected} {route} {action}: expected {expected}, got {identity}')
    return interaction


def resident_geometry(state):
    instancing=state['gltfInstancing']
    return (instancing['sourceVertexCount'],instancing['sourceIndexCount'],instancing['gpuBindVertexCount'],
            state['gltfDeformation']['cpuGeometryRevision'],state['gltfMerge']['descriptorGeneration'])


def assert_saved_scene(state,selected):
    view=state['gltfScenes']
    require(view['selected']==selected and view['names']==NAMES,'Wrong saved scene/default/name selection')
    require(view['pointLightCount']==sum(i in SCENES[selected] for i in (3,4)),'Saved scene changed active light membership')
    require(len(state['gltfNodeFlags']['cameraSourceNodes'])==sum(i in SCENES[selected] for i in (5,6)),
            'Saved scene changed active camera membership')

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
    source=output/'multi-scene.gltf';model=fixture(source)
    sequence=output/'switches.json'
    events=[dict(frame=frame-1,gltfScene=scene) for frame,scene in SAMPLES.items() if frame>1]
    events.extend(dict(frame=frame,depthProbe=PICK_PIXELS[owner]) for frame,owner in PROBES.items())
    sequence.write_text(json.dumps(dict(schemaVersion=1,frames=13,sampleFrames=list(SAMPLES),
        camera=[dict(frame=1,position=[0,0,4],target=[0,0,0])],
        events=sorted(events,key=lambda event:event['frame'])),indent=2))
    try:
        if args.manifest:report['nativeManifestBefore']=verify_runtime_manifest(args.manifest,renderer)
        for technique in args.technique or ['deferred-raster','forward-raster']:
            oracles={}
            for selected,roots in SCENES.items():
                # Prune the node table to form a single-scene independent oracle.
                oracle=copy.deepcopy(model);oracle['nodes']=[copy.deepcopy(model['nodes'][i]) for i in roots]
                oracle['scenes']=[dict(nodes=list(range(len(roots))))];oracle['scene']=0
                path=output/f'oracle-{selected}.gltf';path.write_text(json.dumps(oracle,indent=2))
                oracles[selected]=capture(renderer,output,path,f'{technique}-oracle-{selected}',technique,'lit',report['captures'])[0]
            negative=pixels(oracles[0]['image'],oracles[1]['image'])
            require(negative['meanAbsoluteError']>.5,'Distinct-scene oracle is vacuous')
            for selected in (0,1):
                record=capture(renderer,output,source,f'{technique}-cli-{selected}',technique,'lit',report['captures'],gltf_scene=selected)[0]
                assert_state(record['state'],selected)
                error=pixels(record['image'],oracles[selected]['image'])
                require(error['maximumError']==0,f'CLI scene selection mismatch: {error}')
                report['checks'].append(dict(technique=technique,check='cli-scene',scene=selected,**error))
            destination=output/f'{technique}-saved.glb'
            captured=capture(renderer,output,source,f'{technique}-switches',technique,'lit',report['captures'],sequence=sequence,save=destination)
            require(len(captured)==len(SAMPLES),'Missing scene-switch captures')
            shared=None;previous_epoch=None
            for record,(frame,selected) in zip(captured,sorted(SAMPLES.items())):
                state=record['state'];assert_state(state,selected)
                view=state['gltfScenes']
                identity=(view['graphIdentity'],view['authoringNodeCount'],resident_geometry(state))
                if shared is None:shared=identity
                require(identity==shared,'Scene switch replaced authoring/shared geometry')
                epoch=state['taa']['epoch']
                require(previous_epoch is None or epoch>previous_epoch,'Scene switch retained temporal history')
                previous_epoch=epoch
                error=pixels(record['image'],oracles[selected]['image'])
                require(error['maximumError']==0,f'Scene switch image mismatch frame {frame}: {error}')
                pick=assert_pick(state,selected,PROBES[frame],PICK_PIXELS[PROBES[frame]])
                report['checks'].append(dict(technique=technique,frame=frame,scene=selected,pick=pick,**error))
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
                select=output/f'select-{selected}.json'
                select.write_text(json.dumps(dict(schemaVersion=1,frames=3,sampleFrames=[3],
                    camera=[dict(frame=1,position=[0,0,4],target=[0,0,0])],
                    events=[] if selected==2 else [dict(frame=1,gltfScene=selected)])))
                record=capture(renderer,output,destination,f'{technique}-glb-scene-{selected}',technique,'lit',report['captures'],sequence=select,
                    save=ascii_path if selected==2 else None)[0]
                assert_saved_scene(record['state'],selected)
                error=pixels(record['image'],oracles[selected]['image'])
                require(error['maximumError']==0,f'Saved scene {selected} changed appearance: {error}')
                report['checks'].append(dict(technique=technique,check='portable-scene-reload',scene=selected,**error))
            text,_=load_asset(ascii_path)
            require(len(text['scenes'])==4 and text['scene']==2 and
                    [s['extras']['sceneIdentity'] for s in text['scenes']]==[0,1,2,3],
                    'ASCII export lost inactive scenes, metadata or default')
            for selected in SCENES:
                record=capture(renderer,output,ascii_path,f'{technique}-ascii-scene-{selected}',technique,'lit',report['captures'],
                    sequence=output/f'select-{selected}.json')[0]
                assert_saved_scene(record['state'],selected)
                error=pixels(record['image'],oracles[selected]['image'])
                require(error['maximumError']==0,f'ASCII scene {selected} changed appearance: {error}')
                report['checks'].append(dict(technique=technique,check='ascii-scene-reload',scene=selected,**error))
            save();print('PASS scenes',technique,flush=True)
        require(sha(renderer)==report['rendererSha256'],'Renderer changed during capture')
        if args.manifest:
            report['nativeManifestAfter']=verify_runtime_manifest(args.manifest,renderer)
            require(report['nativeManifestBefore']==report['nativeManifestAfter'],'Native freeze manifest changed during capture')
        report['passed']=True
    except Exception as error:
        report['failure']=str(error);raise
    finally:save()
    print('PASS',len(report['captures']),'captures',len(report['checks']),'checks',flush=True)

if __name__=='__main__':main()
