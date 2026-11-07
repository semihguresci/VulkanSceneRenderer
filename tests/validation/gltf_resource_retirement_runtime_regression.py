"""Native resource Delete/Undo, renderer preservation and portable export.

The checker requires a frozen renderer/input manifest. CPU identities and phase
telemetry are distinct from rendered image evidence. Actual final VkImage lease
release is covered by the backend Vulkan tests, not inferred from these images.
"""
from __future__ import annotations

import argparse
import base64
import copy
import io
import json
from pathlib import Path
import sys
import tempfile

import numpy as np
from PIL import Image

import gltf_material_binding_runtime_regression as binding
import gltf_material_retirement_runtime_regression as material
from gltf_accessor_runtime_regression import Asset, load_asset, require
from gltf_extended_materials_runtime_regression import sha
from gltf_merge_runtime_regression import document, immutable, portable_copy, image_pixels
from gltf_texture_formats_runtime_regression import verify_manifest
from scene_node_authoring_runtime_regression import history_cursor

TECHNIQUES=binding.TECHNIQUES
WIDTH,HEIGHT=binding.WIDTH,binding.HEIGHT


def png(color):
    stream=io.BytesIO();Image.new('RGBA',(4,4),color).save(stream,format='PNG');return stream.getvalue()


def fixture(path,color):
    asset=Asset();record=binding.material('Textured surface',[1.,1.,1.,1.])
    record['pbrMetallicRoughness']['baseColorTexture']=dict(index=0)
    asset.model['materials']=[record]
    asset.model['images']=[dict(uri='data:image/png;base64,'+base64.b64encode(png(color)).decode())]
    asset.model['samplers']=[dict(minFilter=9728,magFilter=9728,wrapS=10497,wrapT=10497)]
    asset.model['textures']=[dict(source=0,sampler=0)]
    attrs=dict(POSITION=asset.accessor('VEC3',binding.POSITIONS),NORMAL=asset.accessor('VEC3',[[0.,0.,1.]]*4),
        TANGENT=asset.accessor('VEC4',[[1.,0.,0.,1.]]*4),TEXCOORD_0=asset.accessor('VEC2',[[0.,0.],[1.,0.],[1.,1.],[0.,1.]]))
    primitive=dict(attributes=attrs,indices=asset.accessor('SCALAR',binding.INDICES,5123,indices=True),material=0,mode=4)
    asset.model['meshes']=[dict(primitives=[primitive])];asset.model['nodes']=[dict(name='Textured surface',mesh=0)]
    asset.model['scenes']=[dict(nodes=[0])];asset.model['extensionsUsed']=[binding.UNLIT]
    path.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='resource-retirement-fixture-') as directory:
        generated=Path(directory)/'fixture.gltf';asset.write(generated);immutable(path,generated.read_bytes())


def retire(frame,row=0,*,failure=False,stale=False):
    request=dict(domain='archive',kind='texture',row=row)
    if stale:request['expectedResourceRevision']=9007199254740991
    result=dict(frame=frame,resourceRetirement=request)
    if failure:result['expectResourceRetirementFailure']=True
    return result


def generate(output):
    output.mkdir(parents=True,exist_ok=True)
    source=output/'sources/red.gltf';control=output/'sources/green.gltf';white=output/'sources/white.gltf';green=output/'sources/replacement.png'
    fixture(source,(210,30,10,255));fixture(control,(20,210,60,255));fixture(white,(255,255,255,255));immutable(green,png((20,210,60,255)))
    descriptions=[('before','red',False,(0,0)),('used-rejected','red',False,(0,0)),('imported','green',False,(1,0)),
        ('deleted','green',True,(2,0)),('undo-delete','green',False,(1,1)),('redo-delete','green',True,(2,0)),
        ('used-new-rejected','green',True,(2,0)),('restore','green',False,(1,1)),
        ('stale-rejected','green',False,(1,1)),('undo-import','red',False,(0,2)),
        ('redo-import','green',False,(1,1)),('redo-delete-final','green',True,(2,0))]
    steps=[dict(label=name,frame=13*(index+1),color=color,retired=retired,history=list(history),resolution=[WIDTH,HEIGHT],msaa=1)
        for index,(name,color,retired,history) in enumerate(descriptions)]
    events=[retire(14,failure=True),dict(frame=27,materialTextureEdit=dict(sourceMaterial=0,slot='baseColor',operation='importFile',
        path=str(green),sampler=dict(minFilter=9728,magFilter=9728,wrapS=10497,wrapT=10497))),retire(40),
        dict(frame=53,sceneHistory='undo'),dict(frame=66,sceneHistory='redo'),retire(79,1,failure=True),
        dict(frame=92,sceneHistory='undo'),retire(105,0,failure=True,stale=True),dict(frame=118,sceneHistory='undo'),
        dict(frame=131,sceneHistory='redo'),dict(frame=144,sceneHistory='redo')]
    script=output/'scripts/delete-history.json'
    document(script,dict(schemaVersion=1,frames=steps[-1]['frame'],sampleFrames=[s['frame'] for s in steps],
        camera=[dict(frame=1,position=[0.,0.,6.],target=[0.,0.,0.])],events=events))
    tail_descriptions=[('before','red',False,(0,0)),('imported','green',False,(1,0)),('cleared','white',False,(2,0)),
        ('deleted','white',True,(3,0)),('undo-delete','white',False,(2,1)),('undo-clear','green',False,(1,2)),
        ('undo-import','red',False,(0,3)),('redo-import','green',False,(1,2)),('redo-clear','white',False,(2,1)),
        ('redo-delete','white',True,(3,0))]
    tail_steps=[dict(label=name,frame=13*(index+1),color=color,retired=retired,history=list(history),resolution=[WIDTH,HEIGHT],msaa=1,
        targetRow=1,missing=name in ('before','undo-import')) for index,(name,color,retired,history) in enumerate(tail_descriptions)]
    tail_events=[dict(events[1],frame=14),dict(frame=27,materialTextureEdit=dict(sourceMaterial=0,slot='baseColor',operation='clear')),
        retire(40,1),dict(frame=53,sceneHistory='undo'),dict(frame=66,sceneHistory='undo'),dict(frame=79,sceneHistory='undo'),
        dict(frame=92,sceneHistory='redo'),dict(frame=105,sceneHistory='redo'),dict(frame=118,sceneHistory='redo')]
    tail_script=output/'scripts/imported-tail-history.json'
    document(tail_script,dict(schemaVersion=1,frames=tail_steps[-1]['frame'],sampleFrames=[s['frame'] for s in tail_steps],
        camera=[dict(frame=1,position=[0.,0.,6.],target=[0.,0.,0.])],events=tail_events))
    inputs=[source,control,white,green,script,tail_script]
    result=dict(schemaVersion=1,source=str(source),control=str(control),white=str(white),script=str(script),steps=steps,
        importedTail=dict(script=str(tail_script),steps=tail_steps),
        immutableInputs=[dict(path=str(p),byteLength=p.stat().st_size,sha256=sha(p)) for p in inputs])
    document(output/'launch-plan.json',result);return result


def pixels(record):
    result=np.asarray(Image.open(record['image']).convert('RGB'),dtype=np.int16)
    require(result.shape==(HEIGHT,WIDTH,3),'Wrong resource capture extent');return result


def same_image(actual,expected):
    a,b=pixels(actual),pixels(expected);require(np.array_equal(a,b),'Resource Delete/Undo changed the rendered image')
    return dict(passed=True,maximumDelta=0,evidence='Complete decoded native raster image')


def control_visible(record,color):
    center=pixels(record)[175:185,315:325].mean(axis=(0,1))
    if color=='red':valid=center[0]>center[1]+20 and center[0]>center[2]+20
    elif color=='green':valid=center[1]>center[0]+20 and center[1]>center[2]+20
    else:valid=float(center.min())>50 and float(center.max()-center.min())<=1
    require(valid,'Independent texture control is dark, absent or has the wrong color: '+color)
    return dict(passed=True,centerMeanRgb=center.tolist(),evidence='Independent constant-color textured fixture interior pixels')


def resource_catalog(state):
    value=state['gltfResourceCatalog']
    require(not value['truncated'],'Truncated resource lifecycle catalog')
    rows=value['rows'];require(len(rows)<=65536,'Resource catalog exceeds its telemetry limit')
    keys=[(r['domain'],r['kind'],r['row']) for r in rows]
    require(len(set(keys))==len(keys),'Ambiguous resource catalog rows')
    require(all(r['domain'] in ('archive','evaluated','runtime') and r['kind'] in ('image','texture','sampler','runtimeTexture') and
        ((r['domain']=='runtime')==(r['kind']=='runtimeTexture')) and type(r['row']) is int and 0<=r['row']<1000000 and
        type(r['active']) is bool and type(r['incarnation']) is int and r['incarnation']>0
        and type(r['domainIdentity']) is int and r['domainIdentity']>0 for r in rows),'Missing exact resource identity/phase')
    require(value['logicalGenerationValid'] is True and type(value['resourceRevision']) is int and value['resourceRevision']>0,
        'Invalid resource generation/revision')
    runtime=[r for r in rows if r['domain']=='runtime']
    require(value['activeRows']==sum(r['active'] for r in rows) and value['retiredRows']==sum(not r['active'] for r in rows) and
        value['runtimeSlotCount']==len(runtime) and value['activeRuntimeCount']==sum(r['active'] for r in runtime),
        'Resource catalog phase counts disagree')
    require(all(r['capabilityValid'] is True for r in value['actualImages']) and
        value['coveredActualImageCount']==len(value['actualImages']),'An active image capability is invalid or missing')
    return value,{key:row for key,row in zip(keys,rows)}


def check_state(state,step,baseline=None):
    catalog,rows=resource_catalog(state);key=('archive','texture',step.get('targetRow',0))
    require(key not in rows if step.get('missing') else key in rows and rows[key]['active']!=step['retired'],'Selected texture phase is wrong')
    history_cursor(state,*step['history'],allow_uncreated=True)
    require(state['gltfScenes']['editHistoryDepth']==sum(step['history']),'Resource history depth disagrees with its cursor')
    if baseline is not None:
        old,oldrows=resource_catalog(baseline)
        require(catalog['assetIdentity']==old['assetIdentity'],'Resource Delete replaced the asset')
        for key,row in oldrows.items():
            require(key in rows and rows[key]['incarnation']==row['incarnation'] and rows[key]['domainIdentity']==row['domainIdentity'],
                'Resource Delete/Undo replaced an existing row identity')
        def geometry(value):
            merge=value['gltfMerge']
            return dict(graph=merge['graphIdentity'],nodes=merge['authoringNodes'],roots=merge['activeRoots'],
                vertices=value['scene']['primaryVertexCount'],indices=value['scene']['primaryIndexCount'])
        require(geometry(state)==geometry(baseline),'Resource Delete changed scene geometry or graph')
    return dict(passed=True,retired=step['retired'],history=step['history'],resourceRevision=catalog['resourceRevision'],
        evidence='Exact CPU catalog/phase/incarnation/history metadata')


def inspect_export(path):
    model,buffers=load_asset(path)
    require(len(model.get('textures',[]))==1 and len(model.get('images',[]))==1,'Export did not remove retired source texture/image holes')
    require(model['textures'][0]['source']==0,'Export texture image index was not projected densely')
    require(len(model.get('samplers',[]))==1 and model['textures'][0]['sampler']==0,'Export sampler table/index is not dense')
    sampler=model['samplers'][0]
    require(all(sampler.get(key,default)==expected for key,default,expected in [
        ('minFilter',-1,9728),('magFilter',-1,9728),('wrapS',10497,10497),('wrapT',10497,10497)]),'Export sampler policy changed')
    require(np.array_equal(image_pixels(path,model,buffers,0),np.asarray(Image.open(io.BytesIO(png((20,210,60,255)))))),
        'Export lost the surviving imported image payload')
    primitive=model['meshes'][0]['primitives'][0];record=model['materials'][primitive['material']]
    require(record['pbrMetallicRoughness']['baseColorTexture']['index']==0,'Export material texture index was not projected densely')
    return dict(path=str(path),sha256=sha(path),passed=True,evidence='Serialized portable dense glTF/GLB resource references')


def dependencies():
    directory=Path(__file__).resolve().parent
    return {str(Path(m.__file__).resolve()):sha(Path(m.__file__).resolve()) for m in tuple(sys.modules.values())
        if getattr(m,'__file__',None) and Path(m.__file__).resolve().parent==directory}


def run(args,output,plan):
    require(args.renderer.is_file() and args.manifest.is_file(),'A frozen renderer and manifest are required')
    report=dict(passed=False,nativeValidationPerformed=False,rendererExecuted=False,rendererSha256=sha(args.renderer),
        sourceDependencies=dependencies(),launchPlanSha256=sha(output/'launch-plan.json'),processes=[],captures=[],stateChecks=[],imageChecks=[],exports=[],portableCopies=[],
        limitations=['No native ImGui, ray/DDGI or BIM acceptance is inferred. Final image lease release is covered separately by backend Vulkan tests.'])
    manifest_sha=sha(args.manifest)
    def persist():
        (output/'results.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    def verify():
        require(sha(args.manifest)==manifest_sha,'Resource manifest changed');result=verify_manifest(args.manifest)
        require(dependencies()==report['sourceDependencies'],'Resource checker helper changed')
        require(sha(output/'launch-plan.json')==report['launchPlanSha256'],'Resource launch plan changed')
        require((output/'vk_layer_settings.txt').read_bytes()==b'khronos_validation.validate_sync = true\n','Validation settings changed')
        for row in plan['immutableInputs']:require(sha(Path(row['path']))==row['sha256'],'Resource fixture/script changed')
        for group in report['portableCopies']:
            for row in group['files']:require(sha(Path(row['path']))==row['sha256'],'Portable resource artifact changed')
        return result
    immutable(output/'vk_layer_settings.txt',b'khronos_validation.validate_sync = true\n')
    frozen={Path(p).resolve() for p in json.loads(args.manifest.read_text(encoding='utf-8-sig'))['files']}
    require({args.renderer,*map(Path,report['sourceDependencies']),*args.renderer.parent.glob('*.dll'),
        *(args.renderer.parent/'spv_shaders').glob('*.spv')}<=frozen,'Manifest omits renderer/checker/runtime dependencies')
    try:
        report['nativeManifestBefore']=verify();persist()
        for technique in args.technique:
            simple=[dict(label='control',frame=13,resolution=[WIDTH,HEIGHT],msaa=1)]
            controls={color:binding.launch(args,output,report,persist,verify,technique,'control-'+color,Path(plan[source]),simple)['control']
                for color,source in [('red','source'),('green','control'),('white','white')]}
            for color,record in controls.items():
                report['imageChecks'].append(dict(technique=technique,label='control-'+color,**control_visible(record,color)))
            require(float(np.abs(pixels(controls['red'])-pixels(controls['green'])).mean())>.5,'Texture replacement visual control is vacuous')
            for suffix in ('gltf','glb'):
                exported=output/'exports'/technique/('retired.'+suffix)
                records=binding.launch(args,output,report,persist,verify,technique,'delete-history-'+suffix,Path(plan['source']),
                    plan['steps'],script=Path(plan['script']),save=exported)
                baseline=records['before']['state']
                known={}
                for step in plan['steps']:
                    record=records[step['label']]
                    _,rows=resource_catalog(record['state'])
                    for key,row in rows.items():
                        identity=(row['domainIdentity'],row['incarnation'])
                        require(key not in known or known[key]==identity,'Older texture history changed a stable resource incarnation')
                        known[key]=identity
                    report['stateChecks'].append(dict(technique=technique,format=suffix,label=step['label'],**check_state(record['state'],step,baseline)))
                    report['imageChecks'].append(dict(technique=technique,label=step['label'],**same_image(record,controls[step['color']])))
                text=Path(records['before']['log']).read_text(encoding='utf-8',errors='replace')
                require(text.count('Expected capture resource retirement failure:')==3,'Missing expected ordinary backend failures')
                imported,importrows=resource_catalog(records['imported']['state'])
                deleted,deleterows=resource_catalog(records['deleted']['state'])
                for key in [('archive','texture',0),('evaluated','texture',0),('archive','image',0),('evaluated','image',0),
                    ('archive','sampler',0),('evaluated','sampler',0)]:
                    require(key in deleterows and deleterows[key]['active'] is False,'Delete left a proved orphan source resource active')
                require(deleted['activeRuntimeCount']<imported['activeRuntimeCount'],'Delete did not retire the orphan sampled runtime texture')
                require(all(deleterows[k]['active']==importrows[k]['active'] for k in importrows if k[0]=='archive' and k[2]>0),
                    'Delete retired a surviving imported resource')
                for after,before in [('used-rejected','before'),('used-new-rejected','redo-delete'),('stale-rejected','restore')]:
                    a,arows=resource_catalog(records[after]['state']);b,brows=resource_catalog(records[before]['state'])
                    require(all(a[key]==b[key] for key in ('resourceRevision','generationRevision','generationEpoch','geometryRevision',
                        'materialRevision','authoredMaterialRevision','runtimeSlotCount','activeRuntimeCount','actualImages','runtimeRows')) and
                        arows==brows,'A rejected resource action changed its catalog or renderer publication clocks')
                report['exports'].append(inspect_export(exported))
                copied,files=portable_copy(exported,output/'portable'/technique/suffix)
                report['portableCopies'].append(dict(entry=str(copied),files=files));verify()
                reloaded=binding.launch(args,output,report,persist,verify,technique,'reload-'+suffix,copied,simple)['control']
                report['imageChecks'].append(dict(technique=technique,label='reload-'+suffix,**same_image(reloaded,controls['green'])))
                persist()
            tail=plan['importedTail']
            records=binding.launch(args,output,report,persist,verify,technique,'imported-tail-history',Path(plan['source']),
                tail['steps'],script=Path(tail['script']))
            baseline=records['before']['state'];known={}
            for step in tail['steps']:
                record=records[step['label']];_,rows=resource_catalog(record['state'])
                for key,row in rows.items():
                    identity=(row['domainIdentity'],row['incarnation'])
                    require(key not in known or known[key]==identity,'Imported resource history recreated a logical incarnation')
                    known[key]=identity
                report['stateChecks'].append(dict(technique=technique,profile='imported-tail',label=step['label'],**check_state(record['state'],step,baseline)))
                report['imageChecks'].append(dict(technique=technique,label='imported-tail-'+step['label'],**same_image(record,controls[step['color']])))
            for label in ('deleted','redo-delete'):
                _,rows=resource_catalog(records[label]['state'])
                for key in [('archive','texture',1),('evaluated','texture',1),('archive','image',1),('evaluated','image',1),
                    ('archive','sampler',1),('evaluated','sampler',1)]:
                    require(key in rows and rows[key]['active'] is False,'Imported-tail Delete left a proved orphan active')
                require(rows[('archive','texture',0)]['active'] is True,'Imported-tail Delete retired the original texture')
            persist()
        report.update(passed=True,nativeValidationPerformed=True)
    except Exception as error:report['failure']=str(error);raise
    finally:
        try:report['nativeManifestAfter']=verify()
        except Exception as error:report.update(passed=False,afterManifestFailure=str(error))
        persist()
    require(report['passed'],'Resource native input validation failed')
    return report


def self_check():
    checks=[]
    with tempfile.TemporaryDirectory(prefix='resource-delete-checker-') as directory:
        plan=generate(Path(directory)/'plan');require(len(plan['steps'])==12,'Wrong native plan size')
        model,_=load_asset(Path(plan['source']));require(len(model['textures'])==len(model['images'])==1,'Fixture source changed')
        checks.append('immutable-native-plan-and-textured-source')
        script=json.loads(Path(plan['script']).read_text());frames=[r['frame'] for r in script['events']]
        require(frames==sorted(set(frames)) and len(frames)==11,'Event order is not one-shot')
        tail=json.loads(Path(plan['importedTail']['script']).read_text());frames=[r['frame'] for r in tail['events']]
        require(frames==sorted(set(frames)) and len(frames)==9 and tail['events'][2]['resourceRetirement']['row']==1 and
            plan['importedTail']['steps'][-1]['retired'],'Imported-tail history does not test its original deletion redo')
        checks.append('ordered-one-shot-actions')
        row=dict(domain='archive',kind='texture',row=0,active=True,domainIdentity=7,incarnation=11)
        catalog=dict(assetIdentity=3,resourceRevision=5,truncated=False,logicalGenerationValid=True,rows=[row],
            activeRows=1,retiredRows=0,runtimeSlotCount=0,activeRuntimeCount=0,actualImages=[],coveredActualImageCount=0)
        state=dict(gltfResourceCatalog=catalog,gltfMaterialVariants=dict(undoCount=0,redoCount=0),gltfScenes=dict(editHistoryDepth=0))
        check_state(state,plan['steps'][0]);checks.append('exact-catalog-and-history-positive')
        for name,mutate in [('truncated',lambda c:c.update(truncated=True)),('missing-incarnation',lambda c:c['rows'][0].update(incarnation=0)),
            ('duplicate-row',lambda c:c['rows'].append(copy.deepcopy(c['rows'][0]))),('phase-count',lambda c:c.update(activeRows=0)),
            ('foreign-domain',lambda c:c['rows'][0].update(domain='foreign')),('runtime-kind-mismatch',lambda c:c['rows'][0].update(kind='runtimeTexture')),
            ('invalid-generation',lambda c:c.update(logicalGenerationValid=False)),('stale-image-capability',lambda c:c.update(actualImages=[dict(capabilityValid=False)],coveredActualImageCount=1))]:
            damaged=copy.deepcopy(state);mutate(damaged['gltfResourceCatalog'])
            try:resource_catalog(damaged)
            except (RuntimeError,KeyError,TypeError,ValueError):checks.append('reject-'+name)
            else:raise RuntimeError('Checker accepted damaged evidence: '+name)
        for name,mutate in [('wrong-phase',lambda s:s['gltfResourceCatalog']['rows'][0].update(active=False)),
            ('wrong-history-depth',lambda s:s['gltfScenes'].update(editHistoryDepth=1)),
            ('wrong-history-cursor',lambda s:s['gltfMaterialVariants'].update(undoCount=1))]:
            damaged=copy.deepcopy(state);mutate(damaged)
            try:check_state(damaged,plan['steps'][0])
            except (RuntimeError,KeyError,TypeError,ValueError):checks.append('reject-'+name)
            else:raise RuntimeError('Checker accepted damaged state: '+name)
    return dict(passed=True,nativeValidationPerformed=False,checks=checks)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--renderer',type=Path);parser.add_argument('--manifest',type=Path)
    parser.add_argument('--output',type=Path);parser.add_argument('--timeout',type=int,default=600)
    parser.add_argument('--technique',choices=TECHNIQUES,action='append');parser.add_argument('--generate-only',action='store_true')
    parser.add_argument('--self-check',action='store_true');args=parser.parse_args()
    if args.self_check:print(json.dumps(self_check()));return
    require(args.output is not None,'An output directory is required');args.output=args.output.resolve();args.technique=args.technique or list(TECHNIQUES)
    require(not args.output.exists(),'Refusing to overwrite resource retirement evidence');plan=generate(args.output)
    if args.generate_only:print(json.dumps(dict(generatedOnly=True,nativeValidationPerformed=False)));return
    require(args.renderer is not None and args.manifest is not None,'Native runs require renderer and manifest')
    args.renderer=args.renderer.resolve();args.manifest=args.manifest.resolve();report=run(args,args.output,plan)
    print(json.dumps(dict(passed=report['passed'],captures=len(report['captures']),processes=len(report['processes']),
        stateChecks=len(report['stateChecks']),imageChecks=len(report['imageChecks']),exports=len(report['exports']))))


if __name__=='__main__':main()
