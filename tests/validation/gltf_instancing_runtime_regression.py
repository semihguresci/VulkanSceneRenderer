"""Native EXT_mesh_gpu_instancing comparisons against ordinary repeated nodes.

Independent skin references clone joint subtrees under each repeated transform.
This preserves node*instance*skin order without using the tested extension.
Use --gpu-picking to read back instance IDs before and after live visibility
changes on both raster techniques.
"""
from __future__ import annotations
import argparse, json, math
from pathlib import Path
from gltf_accessor_runtime_regression import Asset,NativeRun,TECHNIQUES,mesh,compare,require,load_asset
from gltf_material_variants_runtime_regression import run as capture_sequence
from gltf_texture_formats_runtime_regression import verify_manifest
from gltf_vertex_alpha_runtime_regression import add_receiver

CASES=('static','skin','morph')
POSITIONS=[[-.65,-.6,0],[.65,-.6,0],[0,.7,0]]
PARENT_SCALE=[.9,1.2,1]
OWNER_OFFSET=[.05,.02,0]
OWNER_NODE=1
TRANSLATIONS=[[-.8,-.45,0],[.8,-.45,0],[-.8,.55,0],[.8,.55,0]]
SCALES=[[.45,.45,1],[-.45,.45,1],[.45,.55,1],[-.45,.55,1]]
ANGLES=[-.25,.35,.4,-.3]
ROTATIONS=[[0,0,math.sin(a/2),math.cos(a/2)] for a in ANGLES]

def fixture(path,case,kind,ray=False,animated=False,hidden_owner=False):
    a=Asset()
    primitive=mesh(a,a.accessor('VEC3',POSITIONS),a.accessor('VEC3',[[0,0,1]]*3),
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
    a.model['nodes']=[dict(name='Nested parent',scale=PARENT_SCALE,children=[OWNER_NODE]),
                      dict(name='Instanced owner',translation=[*OWNER_OFFSET[:2], .7 if ray else OWNER_OFFSET[2]],children=[])]
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


def pick_samples(anchor):
    # Project the authored triangle's interior independently of the renderer's
    # instance matrices and CPU pick telemetry. The sequence launcher fixes the
    # extent at 640x360, a 45-degree vertical FOV, and camera at (0, 0, 4).
    centroid_y = sum(position[1] for position in POSITIONS) / len(POSITIONS)
    focal_length = 180 / math.tan(math.radians(45) / 2)
    pixels = []
    for translation, scale, angle in zip(TRANSLATIONS, SCALES, ANGLES):
        local_y = centroid_y * scale[1]
        world_x = PARENT_SCALE[0] * (OWNER_OFFSET[0] + translation[0] - math.sin(angle) * local_y)
        world_y = PARENT_SCALE[1] * (OWNER_OFFSET[1] + translation[1] + math.cos(angle) * local_y)
        pixels.append([math.floor(320 + focal_length * world_x / 4),
                       math.floor(180 - focal_length * world_y / 4)])
    require(len({tuple(pixel) for pixel in pixels}) == 4 and
            all(0 <= x < 640 and 0 <= y < 360 for x, y in pixels), 'GPU pick fixture probes must be distinct and on screen')
    visibility_edits = [(7, OWNER_NODE, False), (13, anchor, False), (16, anchor, True), (17, OWNER_NODE, True)]
    events = [dict(frame=frame, nodeFlags=dict(sourceNode=node, visible=visible))
              for frame, node, visible in visibility_edits]
    samples = []
    for phase, first_frame, probes in [('visible', 3, pixels), ('hidden', 9, pixels),
                                        ('all-hidden', 15, pixels[:1]), ('restored', 19, pixels)]:
        for instance, pixel in enumerate(probes):
            frame = first_frame + instance
            samples.append((frame, phase, instance, pixel))
            events.append(dict(frame=frame, depthProbe=pixel))
    return samples, sorted(events, key=lambda event: event['frame'])


def inspect_gpu_picks(records, samples, technique, checks):
    require(len(records) == len(samples), f'Incomplete GPU pick capture sequence: {technique}')
    instance_nodes = None
    raw_ids = {'visible': set(), 'restored': set()}
    expected_draws = {'visible': 6, 'hidden': 1, 'all-hidden': 0, 'restored': 6}
    for record, (frame, phase, instance, pixel) in zip(records, samples):
        state = record['state']
        location = record['telemetry']
        instancing = state['gltfInstancing']
        instances = instancing['instances']
        visible = phase in raw_ids
        require(len(instances) == 4 and all(entry['visible'] == visible for entry in instances),
                f'Wrong instance count or inherited visibility: {location}')
        require(instancing['drawCount'] == expected_draws[phase], f'Wrong draw count for {phase} pick probe: {location}')
        nodes = {entry['sourceInstance']: entry['graphNode'] for entry in instances}
        require(set(nodes) == {0, 1, 2, 3} and len(set(nodes.values())) == 4,
                f'Instance graph identities were lost: {location}')
        require(instance_nodes is None or nodes == instance_nodes, f'Visibility edit replaced instance identities: {location}')
        instance_nodes = nodes

        probe = state['depthProbe']
        require(probe['pixel'] == pixel, f'Incorrect GPU pick pixel: {location}')
        routes = probe['sceneInteraction']
        raw_id = routes['rawGpuId']
        # Deferred still clears the PickId attachment in GBuffer without
        # draws. Forward has no ID writer when the shared pick pass skips.
        gpu_available = phase != 'all-hidden' or technique == 'deferred-raster'
        if gpu_available:
            require(type(raw_id) is int and routes['gpu'] is not None, f'GPU ID buffer readback unavailable: {location}')
        else:
            require(raw_id is None and routes['gpu'] is None, f'GPU ID buffer read back without a current writer: {location}')
        expected = (nodes[instance], OWNER_NODE, 0, 0, instance) if visible else None
        for route in ('cpu', 'gpu') if gpu_available else ('cpu',):
            for action in ('hit', 'selection', 'hover'):
                hit = routes[route][action]
                identity = None if hit is None else tuple(hit[key] for key in
                    ('graphNode', 'sourceNode', 'sourceMesh', 'sourcePrimitive', 'sourceInstance'))
                require(identity == expected, f'{route} {action} returned {identity}, expected {expected}: {location}')
        if visible:
            require(raw_id > 0 and raw_id not in raw_ids[phase], f'Reused or clear GPU ID for visible instance: {location}')
            raw_ids[phase].add(raw_id)
        elif gpu_available:
            require(raw_id == 0 and probe['opaque'] == 0, f'Hidden instance still contributed pick ID or opaque depth: {location}')
        checks.append(dict(case='static', technique=technique, check='gpu-instance-picking', frame=frame,
                           phase=phase, sourceInstance=instance, pixel=pixel, rawGpuId=raw_id, gpuReadbackAvailable=gpu_available,
                           graphNode=nodes[instance], hit=visible))


def gpu_picking(run, args):
    source = fixture(run.output/'gpu-picking-source.gltf', 'static', 'source')
    model = json.loads(source.read_text(encoding='utf-8'))
    owner = model['nodes'][OWNER_NODE]
    # Legal integral decimal tokens must survive source validation and reach
    # the native instance draws without a TinyGLTF integer fallback.
    owner['mesh'] = float(owner['mesh'])
    attributes = owner['extensions']['EXT_mesh_gpu_instancing']['attributes']
    for semantic, index in attributes.items():
        attributes[semantic] = float(index)
        accessor = model['accessors'][index]
        for field in ('count', 'componentType', 'bufferView', 'byteOffset'):
            if field in accessor:
                accessor[field] = float(accessor[field])
    anchor = len(model['nodes'])
    # Keep a current ID writer active while all instances are hidden. A
    # separate sample then hides this offscreen draw as well.
    model['nodes'].append(dict(name='Offscreen pick-pass anchor', mesh=0, translation=[20, 0, 0]))
    model['scenes'][0]['nodes'].append(anchor)
    source.write_text(json.dumps(model, indent=2), encoding='utf-8')
    samples, events = pick_samples(anchor)
    sequence = run.output/'gpu-picking-sequence.json'
    frames = [sample[0] for sample in samples]
    sequence.write_text(json.dumps(dict(schemaVersion=1, frames=frames[-1], sampleFrames=frames, events=events),
                                   indent=2), encoding='utf-8')
    run.report['fixtures'].append(dict(case='gpu-picking', files=dict(source=str(source), sequence=str(sequence))))
    if args.generate_only:
        return
    for technique in args.technique or TECHNIQUES:
        records = capture_sequence(run.renderer, run.output, source, technique+'-gpu-picking', technique, 'lit',
                                   run.report['captures'], sequence=sequence, timeout=run.timeout,
                                   extra=('--no-ray-query',), require_validation=True)
        inspect_gpu_picks(records, samples, technique, run.report['checks'])
        run.save()
        print('PASS instancing GPU picks', technique, flush=True)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--renderer',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--case',action='append',choices=CASES);p.add_argument('--technique',action='append',choices=TECHNIQUES)
    p.add_argument('--manifest',type=Path);p.add_argument('--roundtrip',action='store_true')
    p.add_argument('--ray',action='store_true');p.add_argument('--generate-only',action='store_true')
    p.add_argument('--animated',action='store_true');p.add_argument('--hidden-owner',action='store_true')
    p.add_argument('--gpu-picking', action='store_true', help='Run the dedicated static GPU pick and visibility sequence')
    args=p.parse_args()
    if args.gpu_picking and (args.case or args.roundtrip or args.ray or args.animated or args.hidden_owner):
        p.error('--gpu-picking uses a dedicated static fixture and sequence')
    run=NativeRun(args.renderer,args.output,180)
    if args.manifest: run.report['nativeManifestBefore']=verify_manifest(args.manifest)
    run.report['unverified']=['Singular instance transforms require geometry baking and are rejected before scene retirement']
    if not args.gpu_picking or args.generate_only:
        run.report['unverified'].append('Native GPU selection readback requires --gpu-picking without --generate-only')
    try:
        if args.gpu_picking:
            gpu_picking(run, args)
        cases = () if args.gpu_picking else (args.case or CASES)
        for case in cases:
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
