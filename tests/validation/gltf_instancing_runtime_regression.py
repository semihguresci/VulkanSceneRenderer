"""Native EXT_mesh_gpu_instancing comparisons against ordinary repeated nodes.

Independent skin references clone joint subtrees under each repeated transform.
This preserves node*instance*skin order without using the tested extension.
"""
from __future__ import annotations
import argparse, math
from pathlib import Path
from gltf_accessor_runtime_regression import Asset,NativeRun,TECHNIQUES,mesh,compare,require,load_asset
from gltf_texture_formats_runtime_regression import verify_manifest
from gltf_vertex_alpha_runtime_regression import add_receiver

CASES=('static','skin','morph')
TRANSLATIONS=[[-.8,-.45,0],[.8,-.45,0],[-.8,.55,0],[.8,.55,0]]
SCALES=[[.45,.45,1],[-.45,.45,1],[.45,.55,1],[-.45,.55,1]]
ANGLES=[-.25,.35,.4,-.3]
ROTATIONS=[[0,0,math.sin(a/2),math.cos(a/2)] for a in ANGLES]

def fixture(path,case,kind,ray=False,animated=False,hidden_owner=False):
    a=Asset()
    p=[[-.65,-.6,0],[.65,-.6,0],[0,.7,0]]
    primitive=mesh(a,a.accessor('VEC3',p),a.accessor('VEC3',[[0,0,1]]*3),
        a.accessor('VEC2',[[0,1],[1,1],[.5,0]]),
        tangent=a.accessor('VEC4',[[1,0,0,1]]*3),
        indices=a.accessor('SCALAR',[0,1,2],5123,indices=True))
    a.model['materials']=[dict(pbrMetallicRoughness=dict(baseColorFactor=[.15,.7,.35,1],metallicFactor=0,roughnessFactor=.7))]
    if not ray:
        a.model['materials'][0]['extensions']=dict(KHR_materials_unlit={})
        a.model['extensionsUsed']=['KHR_materials_unlit']
    if case=='morph':
        primitive['targets']=[dict(POSITION=a.accessor('VEC3',[[.2,.08,0],[.25,.08,0],[.12,.15,0]]))]
        a.model['meshes'][0]['weights']=[.4]
    if case=='skin':
        primitive['attributes']['JOINTS_0']=a.accessor('VEC4',[[0,0,0,0]]*3,5121)
        primitive['attributes']['WEIGHTS_0']=a.accessor('VEC4',[[1,0,0,0]]*3)
    a.model['nodes']=[dict(name='Nested parent',scale=[.9,1.2,1],children=[1]),
                      dict(name='Instanced owner',translation=[.05,.02,.7 if ray else 0],children=[])]
    a.model['scenes'][0]['nodes']=[0]
    owner=a.model['nodes'][1]
    child=dict(name='Authored ordinary child',mesh=0,translation=[0,-1,0],scale=[.2,.2,1])
    if kind=='reference':
        for i,(t,r,s) in enumerate(zip(TRANSLATIONS,ROTATIONS,SCALES)):
            group=len(a.model['nodes'])
            a.model['nodes'].append(dict(name=f'Repeated {i}',translation=t,rotation=r,scale=s,children=[group+1]))
            owner['children'].append(group)
            node=dict(name=f'Ordinary mesh {i}',mesh=0)
            a.model['nodes'].append(node)
            if case=='skin':
                joint=len(a.model['nodes'])
                a.model['nodes'][group]['children'].append(joint)
                a.model['nodes'].append(dict(name=f'Repeated joint {i}',translation=[.12,.04,0]))
                skins=a.model.setdefault('skins',[])
                node['skin']=len(skins);skins.append(dict(joints=[joint]))
    else:
        owner['mesh']=0
        if case=='skin':
            joint=len(a.model['nodes'])
            owner['children'].append(joint)
            a.model['nodes'].append(dict(name='Shared joint',translation=[.12,.04,0]))
            owner['skin']=0;a.model['skins']=[dict(joints=[joint])]
        if kind=='source':
            attrs=dict(TRANSLATION=a.accessor('VEC3',TRANSLATIONS,stride=16,prefix=4),
                       ROTATION=a.accessor('VEC4',ROTATIONS),SCALE=a.accessor('VEC3',SCALES),
                       _ID=a.accessor('SCALAR',[16777217,16777219,16777221,16777223],5125))
            owner['extensions']=dict(EXT_mesh_gpu_instancing=dict(attributes=attrs,extras=dict(retain='instance-source')))
            a.model.setdefault('extensionsUsed',[]).append('EXT_mesh_gpu_instancing')
            a.model['extensionsRequired']=['EXT_mesh_gpu_instancing']
    owner['children'].append(len(a.model['nodes']));a.model['nodes'].append(child)
    if ray: add_receiver(a,shadow=True)
    if animated:
        time=a.accessor('SCALAR',[0.,1.])
        translate=a.accessor('VEC3',[[.05,.02,.7 if ray else 0],[.12,.08,.7 if ray else 0]])
        scale=a.accessor('VEC3',[[.9,1.2,1],[1.05,.9,1]])
        a.model['animations']=[dict(name='Parent and owner motion',samplers=[dict(input=time,output=translate),dict(input=time,output=scale)],
            channels=[dict(sampler=0,target=dict(node=1,path='translation')),dict(sampler=1,target=dict(node=0,path='scale'))])]
    if hidden_owner:
        add_receiver(a,shadow=False)
        receiver=len(a.model['nodes'])-1
        # Keep the initially selected primitive offscreen in every variant.
        # Otherwise the receiver-only oracle acquires a yellow selection
        # outline that is absent when the selected instance owner is hidden.
        dummy=len(a.model['nodes'])
        a.model['nodes'].append(dict(name='Offscreen selection anchor',mesh=0,translation=[20,0,0]))
        a.model['scenes'][0]['nodes'].insert(0,dummy)
        if kind=='source':
            owner.setdefault('extensions',{})['KHR_node_visibility']=dict(visible=False)
            a.model.setdefault('extensionsUsed',[]).append('KHR_node_visibility')
        elif kind=='reference':
            # An independent visibility oracle only instantiates the receiver.
            a.model['scenes'][0]['nodes']=[dummy,receiver]
    a.write(path)
    return path

def inspect(actual,reference,case,hidden_owner=False):
    state=actual['state'];inst=state['gltfInstancing']
    require(len(inst['instances'])==4,'Missing source instance identities or phantom fallback draw')
    require(sorted(x['sourceInstance'] for x in inst['instances'])==[0,1,2,3],'Instance identity lost')
    require(len({x['primitiveIndex'] for x in inst['instances']})==1,'Instancing duplicated source primitive')
    require(inst['sourceVertexCount']==reference['state']['gltfInstancing']['sourceVertexCount'],'Source geometry was copied per instance')
    require(inst['sourceIndexCount']==reference['state']['gltfInstancing']['sourceIndexCount'],'Source indices were copied per instance')
    bounds,other_bounds=inst['modelBounds'],reference['state']['gltfInstancing']['modelBounds']
    require(bounds['valid'] and other_bounds['valid'],'Missing source framing bounds')
    error=max(abs(a-b) for key in ('min','max','center') for a,b in zip(bounds[key],other_bounds[key]))
    require(error<=1e-5 and abs(bounds['radius']-other_bounds['radius'])<=1e-5,
            f'Framing bounds disagree with repeated nodes: {bounds} vs {other_bounds}')
    for sample in inst['instances']:
        pick=sample['pick']
        if hidden_owner:
            require(not sample['visible'] and (not pick['hit'] or pick.get('hitNode')!=pick['expectedNode']),f'Hidden instance remained selectable: {sample}')
        else:
            require(pick['projected'] and pick['hit'] and pick['hitNode']==pick['expectedNode'],f'Production CPU picking missed instance: {sample}')
    if case!='static':
        deformed=state['gltfDeformation'];other=reference['state']['gltfDeformation']
        require(deformed['gpuReady'] and deformed['currentBoundToGpu'],'Instance deformation did not execute on GPU')
        require(deformed['instanceCount']<other['instanceCount'],'Owner deformation streams were not shared')
        require(deformed['expandedVertexCount']<other['expandedVertexCount'],'Deformed output expanded per instance')
    return dict(sourceVertexCount=inst['sourceVertexCount'],sourceIndexCount=inst['sourceIndexCount'],
                sourceInstances=4,productionCpuPicks=0 if hidden_owner else 4,hiddenInstancePickRejections=4 if hidden_owner else 0,
                framingBoundsMaxError=error,
                deformedStreams=state['gltfDeformation']['instanceCount'],
                repeatedDeformedStreams=reference['state']['gltfDeformation']['instanceCount'])

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--renderer',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--case',action='append',choices=CASES);p.add_argument('--technique',action='append',choices=TECHNIQUES)
    p.add_argument('--manifest',type=Path);p.add_argument('--roundtrip',action='store_true')
    p.add_argument('--ray',action='store_true');p.add_argument('--generate-only',action='store_true')
    p.add_argument('--animated',action='store_true');p.add_argument('--hidden-owner',action='store_true')
    args=p.parse_args();run=NativeRun(args.renderer,args.output,180)
    if args.manifest: run.report['nativeManifestBefore']=verify_manifest(args.manifest)
    run.report['unverified']=['GPU selection readback has no native capture action','Singular instance transforms require geometry baking and are rejected before scene retirement']
    try:
        for case in args.case or CASES:
            files={kind:fixture(run.output/f'{case}-{kind}.gltf',case,kind,args.ray,args.animated,args.hidden_owner) for kind in ('source','reference','dropped')}
            run.report['fixtures'].append(dict(case=case,files={k:str(v) for k,v in files.items()}))
            if args.generate_only: continue
            for technique in args.technique or TECHNIQUES:
                stem=technique+'-'+case
                destination=run.output/(stem+'-saved.glb') if args.roundtrip else None
                extra=['--ray-shadows','hard','--no-ray-shadow-denoise','--ddgi','--ddgi-intensity','0',
                       '--ddgi-fit-scene','--ddgi-fit-probes','64','--ddgi-rays','32','--ddgi-probes-per-frame','64',
                       '--ddgi-ray-budget','2048','--ddgi-hysteresis','0'] if args.ray else []
                if args.animated: extra+=['--animation-clip','0','--animation-time','1','--animation-paused']
                actual=run.capture(files['source'],stem+'-source',technique,destination=destination,extra=extra,lit=args.ray)
                reference=run.capture(files['reference'],stem+'-reference',technique,extra=extra,lit=args.ray)
                dropped=run.capture(files['dropped'],stem+'-dropped',technique,extra=extra,lit=args.ray)
                if args.animated:
                    for record in (actual,reference,dropped):
                        state=record['state']['gltfAnimation']
                        require(state['selectedClip']==0 and abs(state['time']-1)<1e-6 and not state['playing'] and state['diagnostics']==0,
                                'Parent/owner animation did not execute at the requested time')
                run.report['checks'].append(dict(case=case,technique=technique,check='ordinary-repeated-nodes',**compare(actual,reference,tolerance=.05)))
                run.report['checks'].append(dict(case=case,technique=technique,check='missing-instancing-negative-control',**compare(actual,dropped,exact=False)))
                run.report['checks'].append(dict(case=case,technique=technique,check='shared-geometry-deformation-and-instance-picks',**inspect(actual,reference,case,args.hidden_owner)))
                if args.ray:
                    state=actual['state'];ray=state['rayShadows'];gi=state['ddgi']
                    require(ray['active'] and ray['build']['instances']==6,'TLAS lost per-instance placement or authored child/receiver')
                    expected_blas=2 if case=='static' else 3
                    require(ray['build']['blasBuilt']+ray['build']['blasReused']==expected_blas,'Shared owner pose/source mesh/receiver did not reuse BLAS')
                    require(gi['active'] and gi['tracedRays']>0,'DDGI did not traverse instanced scene')
                    run.report['checks'].append(dict(case=case,technique=technique,check='shared-blas-and-ddgi',build=ray['build'],tracedRays=gi['tracedRays']))
                if destination:
                    ascii_path=destination.with_suffix('.gltf')
                    glb=run.capture(destination,stem+'-glb-reload',technique,destination=ascii_path,extra=extra,lit=args.ray)
                    text=run.capture(ascii_path,stem+'-ascii-reload',technique,extra=extra,lit=args.ray)
                    for kind,record in [('glb',glb),('gltf',text)]:
                        run.report['checks'].append(dict(case=case,technique=technique,check=kind+'-reload',**compare(actual,record,tolerance=.05)))
                        model,_=load_asset(destination if kind=='glb' else ascii_path)
                        require(all('EXT_mesh_gpu_instancing' not in n.get('extensions',{}) or 'mesh' in n for n in model['nodes']),
                                'Export left instance extension on meshless source group')
                run.save();print('PASS instancing',case,technique,flush=True)
        if args.manifest: run.report['nativeManifestAfter']=verify_manifest(args.manifest)
        if args.generate_only: run.report['passed']=True
        else: run.finish()
    except Exception as e:
        run.report['failure']=str(e);raise
    finally:
        run.report.update(captureCount=len(run.report['captures']),checkCount=len(run.report['checks']));run.save()
    print('PASS',len(run.report['captures']),'captures',len(run.report['checks']),'checks',flush=True)
if __name__=='__main__':main()
