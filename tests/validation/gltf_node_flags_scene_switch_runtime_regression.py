"""Bounded node-flag retention across scene and raster-technique changes.

Independent ordinary references prune inactive/hidden nodes and remove flag
extensions. One authoring graph visits both scenes repeatedly, changes raster
technique, then exports both portable formats. Routing remains the explicit
nearest-hit ancestor approximation; no trace-through/GPU picking is claimed.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from gltf_accessor_runtime_regression import finite_json, require
from gltf_material_variants_runtime_regression import pixels, run
from gltf_node_flags_runtime_regression import (FLAGS, HIDDEN_CAMERA, HIDDEN_LIGHT, HIDDEN_MESH,
    TARGET_A, TARGET_B, TARGET_C, fixture, inspect_export, inspect_state)
from gltf_texture_formats_runtime_regression import verify_manifest

# frame -> scene, edited flags, shown branch, raster technique, undo count
SAMPLES = {1: (0,False,False,'deferred-raster',0), 5: (0,True,True,'deferred-raster',2),
           9: (1,True,True,'deferred-raster',0), 13: (0,True,True,'deferred-raster',0),
           17: (1,True,True,'forward-raster',0), 21: (0,True,True,'forward-raster',0)}


def active_source_nodes(model, selected):
    seen, work = set(), list(model['scenes'][selected]['nodes'])
    while work:
        node = work.pop()
        require(node not in seen, 'Generated scene roots overlap or form a cycle')
        seen.add(node); work.extend(model['nodes'][node].get('children', []))
    return sorted(seen)


def ordinary_reference(model, selected, shown, path):
    retained = [node for node in active_source_nodes(model,selected)
                if shown or node not in (HIDDEN_MESH,HIDDEN_LIGHT)]
    remap = {old:new for new,old in enumerate(retained)}
    reference = copy.deepcopy(model)
    reference['nodes'] = [copy.deepcopy(model['nodes'][node]) for node in retained]
    for node in reference['nodes']:
        if 'children' in node: node['children'] = [remap[child] for child in node['children'] if child in remap]
        for extension in FLAGS.values(): node.get('extensions',{}).pop(extension,None)
        if not node.get('extensions'): node.pop('extensions',None)
    reference['extensionsUsed'] = ['KHR_lights_punctual']
    reference['scenes'] = [dict(nodes=[remap[root] for root in model['scenes'][selected]['nodes']])]
    reference['scene'] = 0
    require(reference['meshes']==model['meshes'] and reference['accessors']==model['accessors'] and
            reference['buffers']==model['buffers'], 'Independent reference rewrote shared geometry')
    path.write_text(json.dumps(reference,indent=2),encoding='utf-8')


def capture_mixed(renderer, output, source, sequence, destination, captures):
    prefix='scene-technique-switches'
    for pattern in (prefix+'*.png',prefix+'*.telemetry.json'):
        for old in output.glob(pattern): old.unlink()
    image,log=output/(prefix+'.png'),output/(prefix+'.log')
    command=[str(renderer),'--model',str(source),'--hidden','--no-ui','--validation',
        '--width','640','--height','360','--msaa','1','--no-taa','--no-bloom',
        '--environment-intensity','0','--directional-intensity','3','--directional-direction','0','1','-1',
        '--exposure','1','--camera-position','0','0','4','--camera-target','0','0','0','--camera-fov','45',
        '--fixed-dt','.016666667','--render-technique','deferred-raster','--display-mode','lit',
        '--capture-sequence',str(sequence),'--screenshot',str(image),'--save-scene',str(destination)]
    with log.open('w',encoding='utf-8') as stream:
        result=subprocess.run(command,cwd=renderer.parent,stdout=stream,stderr=subprocess.STDOUT,timeout=240,
            env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(output)),creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    text=log.read_text(encoding='utf-8',errors='replace')
    require(result.returncode==0,f'Mixed scene/technique native process failed: {log}')
    require(not re.search(r'VUID-|SYNC-HAZARD|Validation Error',text,re.I),f'Vulkan validation failed: {log}')
    records=[]
    for telemetry in sorted(output.glob(prefix+'*.telemetry.json')):
        frame_image=telemetry.with_suffix('').with_suffix('.png')
        state=json.loads(telemetry.read_text(encoding='utf-8'))
        require(frame_image.is_file() and finite_json(state),f'Invalid mixed capture: {telemetry}')
        record=dict(image=str(frame_image),telemetry=str(telemetry),log=str(log),command=command)
        captures.append(record); records.append(dict(**record,state=state))
    require(len(records)==len(SAMPLES),'Missing scene/technique preservation samples')
    return records


def inspect_alternate(state, model):
    flags=state['gltfNodeFlags']; rows={row['graphNode']:row for row in flags['nodes']}
    source={row['name']:row for row in flags['nodes'] if row['sourceIndex']>=0}
    node=lambda index:source[model['nodes'][index]['name']]
    require(not flags['truncated'],'Flag source graph telemetry truncated')
    active=active_source_nodes(model,1)
    require(sorted(state['gltfScenes']['activeSourceNodes'])==active,'Alternate view retained inactive source groups')
    for index in range(len(model['nodes'])):
        row=node(index)
        require(row['active']==(index in active),'Wrong active/inactive source identity')
        expected={key:model['nodes'][index].get('extensions',{}).get(extension,{}).get(key,True)
                  for key,extension in FLAGS.items()}
        if index==2: expected.update(selectable=True,hoverable=False)
        if index==7: expected['visible']=True
        for key,value in expected.items():
            require(row['local'+key.capitalize()]==value,'A scene switch lost a local flag, including inactive A edits')
    require(node(HIDDEN_CAMERA)['sourceIndex'] in flags['cameraSourceNodes'],'Shared hidden-parent camera became unavailable')
    require(state['gltfScenes']['pointLightCount']==1 and len(state['gltfAnimation']['pointLights'])==1,
            'Shared restored light disappeared during a scene/mode switch')
    require(state['gltfScenes']['editHistoryDepth']==0 and state['gltfMaterialVariants'].get('undoCount',0)==0 and
            state['gltfMaterialVariants'].get('redoCount',0)==0,'Scene switch retained flag edit/redo history')
    routed=[]
    for index in (TARGET_B,HIDDEN_MESH):
        target=node(index)['graphNode']; matching=[]
        for probe in flags['probes']:
            current,visited=probe['graphNode'],set()
            while current in rows and current not in visited:
                visited.add(current)
                if current==target: matching.append(probe); break
                current=rows[current]['parentGraphNode']
        require(len(matching)==1,'Active ordinary primitive probe missing or duplicated')
        probe=matching[0]
        require(probe['projected'] and probe['hit'] and probe['nearestHitGraphNode']==probe['graphNode'],
                'Alternate view raw hit did not reach the isolated primitive')
        require(probe['resolvedSelection']==(-1 if index==TARGET_B else probe['nearestHitGraphNode']) and
                probe['resolvedHover']==probe['nearestHitGraphNode'],'Active nearest-hit flag routing changed')
        routed.append(dict(target=model['nodes'][index]['name'],selection=probe['resolvedSelection'],hover=probe['resolvedHover']))
    require(len(flags['probes'])==3 and state['gltfScenes']['activeRenderableCount']==3 and
            state['scene']['primaryObjectCount']==3,'Alternate view retained inactive geometry/probes')
    require(not node(TARGET_A)['active'] and not node(TARGET_C)['active'],'Inactive geometry leaked into the alternate scene')
    return dict(inactiveAFlagsRetained=True,historyCleared=True,activeRoutes=routed,availableSharedCamera=True)


def geometry_signature(state):
    view,gpu,geometry=state['gltfScenes'],state['gltfDeformation'],state['gltfInstancing']
    return dict(graphIdentity=view['graphIdentity'],authoringNodes=view['authoringNodeCount'],
        sourceVertexCount=geometry['sourceVertexCount'],sourceIndexCount=geometry['sourceIndexCount'],
        gpuBindVertexCount=geometry['gpuBindVertexCount'],cpuGeometryRevision=gpu['cpuGeometryRevision'],
        primaryVertexCount=state['scene']['primaryVertexCount'],primaryIndexCount=state['scene']['primaryIndexCount'],
        geometryArenaCapacityBytes=state['scene']['geometryArenaCapacityBytes'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer',required=True,type=Path);parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--manifest',type=Path);parser.add_argument('--generate-only',action='store_true')
    args=parser.parse_args();renderer=args.renderer.resolve();output=args.output.resolve()
    output.mkdir(parents=True,exist_ok=True);require(renderer.is_file(),f'Missing renderer: {renderer}')
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n',encoding='utf-8')
    report=dict(schemaVersion=1,passed=False,nativeValidationPerformed=False,generatedOnly=args.generate_only,
        rendererSha256=hashlib.sha256(renderer.read_bytes()).hexdigest(),captures=[],checks=[],expectedCaptureCount=13,expectedCheckCount=14,
        scope='Nearest-hit ancestor routing only; no trace-through or GPU picking readback')
    def save():
        report.update(captureCount=len(report['captures']),checkCount=len(report['checks']))
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    if args.manifest:report['nativeManifestBefore']=verify_manifest(args.manifest)
    try:
        source=output/'flags-shared-scenes.gltf';model=fixture(source);source_bytes=source.read_bytes()
        sequence=output/'scene-mode-switches.json'
        sequence.write_text(json.dumps(dict(schemaVersion=1,frames=21,sampleFrames=list(SAMPLES),
            camera=[dict(frame=1,position=[0,0,4],target=[0,0,0])],events=[
                dict(frame=2,nodeFlags=dict(sourceNode=2,selectable=True,hoverable=False)),
                dict(frame=3,nodeFlags=dict(sourceNode=7,visible=True)),dict(frame=6,gltfScene=1),
                dict(frame=10,gltfScene=0),dict(frame=14,gltfScene=1,technique='forward-raster'),dict(frame=18,gltfScene=0)]),indent=2),encoding='utf-8')
        paths={}
        for selected,shown,technique in ((0,False,'deferred-raster'),(0,True,'deferred-raster'),(1,True,'deferred-raster'),
                                         (0,True,'forward-raster'),(1,True,'forward-raster')):
            path=output/f'ordinary-{selected}-{shown}-{technique}.gltf'
            ordinary_reference(model,selected,shown,path);paths[selected,shown,technique]=path
        report['fixtures']=dict(source=str(source),sequence=str(sequence),references=[str(path) for path in paths.values()])
        if not args.generate_only:
            references={key:run(renderer,output,path,'oracle-'+path.stem,key[2],'lit',report['captures'])[0] for key,path in paths.items()}
            contrasts=[]
            for first,second in (((0,False,'deferred-raster'),(0,True,'deferred-raster')),
                                  ((0,True,'deferred-raster'),(1,True,'deferred-raster')),
                                  ((0,True,'forward-raster'),(1,True,'forward-raster'))):
                difference=pixels(references[first]['image'],references[second]['image'])
                require(difference['meanAbsoluteError']>.25 and difference['changedPixelFraction']>.003,'Independent pruned reference contrast is vacuous')
                contrasts.append(dict(first=str(first),second=str(second),**difference))
            report['independentNegativeControls']=contrasts
            destination=output/'all-scenes-flags.glb'
            records=capture_mixed(renderer,output,source,sequence,destination,report['captures'])
            baseline=None;source_references=None
            for record,(frame,(selected,edited,shown,technique,depth)) in zip(records,sorted(SAMPLES.items())):
                state=record['state']
                require(state['technique']==technique and state['gltfScenes']['selected']==selected,'Requested scene/technique was not applied')
                semantics=inspect_alternate(state,model) if selected==1 else inspect_state(state,model,edited,shown,
                    history=depth,stored_history=depth)
                difference=pixels(record['image'],references[selected,shown,technique]['image'])
                require(difference['maximumError']==0,f'Independent scene/mode image differs at frame{frame}: {difference}')
                report['checks'].append(dict(check='retained-flags-and-active-view',frame=frame,scene=selected,technique=technique,**semantics,**difference))
                signature=geometry_signature(state)
                require(signature['sourceVertexCount']==4 and signature['sourceIndexCount']==6,'Source quad geometry changed')
                baseline=signature if baseline is None else baseline
                require(signature==baseline,'Scene/mode switch replaced graph or source geometry/reference storage')
                assignments=state['gltfMaterialVariants']['assignments']
                if source_references is None:
                    source_references={entry['node']:(entry['mesh'],entry['primitive'],entry['material']) for entry in assignments}
                require(all(source_references.get(entry['node'])==(entry['mesh'],entry['primitive'],entry['material']) for entry in assignments),
                        'Shared primitive/material reference identity changed')
                report['checks'].append(dict(check='stable-geometry-references',frame=frame,scene=selected,
                    geometryReferenceSignature=signature,activePrimitiveReferences=len(assignments)))
            for path in (destination,destination.with_suffix('.gltf')):
                metadata=inspect_export(path,model)
                record=run(renderer,output,path,'portable-'+path.suffix[1:],'forward-raster','lit',report['captures'],
                    save=destination.with_suffix('.gltf') if path.suffix=='.glb' else None)[0]
                semantics=inspect_state(record['state'],model,True,True)
                difference=pixels(record['image'],references[0,True,'forward-raster']['image'])
                require(difference['maximumError']==0,f'Portable all-scene flag state changed: {difference}')
                report['checks'].append(dict(check='portable-all-scene-flag-retention',**metadata,**semantics,**difference))
            report['nativeValidationPerformed']=True
        require(source.read_bytes()==source_bytes,'Generated source file was changed during switching/export')
        require(hashlib.sha256(renderer.read_bytes()).hexdigest()==report['rendererSha256'],'Frozen renderer changed')
        if args.manifest:report['nativeManifestAfter']=verify_manifest(args.manifest)
        report['passed']=None if args.generate_only else True
    except Exception as error:
        report['failure']=str(error);raise
    finally:save()
    print('GENERATED' if args.generate_only else 'PASS',report['captureCount'],'captures',report['checkCount'],'checks',flush=True)


if __name__=='__main__':main()
