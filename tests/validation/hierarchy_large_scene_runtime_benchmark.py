"""Measure complete native CPU/GPU paths on a large, continuously changing glTF scene.

Fixed-step capture timings include the renderer's normal CPU mirror and ray
extraction. GPU frame-graph timing excludes the separately reported hierarchy
pass and is not total GPU frame latency. Payload counters are bytes copied by
the measured writers, not PCIe traffic. No speedup threshold is assumed.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess

from gltf_accessor_runtime_regression import Asset,mesh
from gltf_extended_materials_runtime_regression import validation_log_error
from gltf_material_variants_runtime_regression import pixels


def fixture(path,count):
    asset=Asset()
    mesh(asset,asset.accessor('VEC3',[[-.04,-.04,0],[.04,-.04,0],[0,.04,0]]),
         asset.accessor('VEC3',[[0,0,1]]*3),asset.accessor('VEC2',[[0,0],[1,0],[.5,1]]),
         indices=asset.accessor('SCALAR',[0,1,2],5123,indices=True))
    asset.model['materials']=[dict(pbrMetallicRoughness=dict(baseColorFactor=[.3,.65,.9,1],metallicFactor=0,roughnessFactor=.7))]
    side=math.ceil(math.sqrt(count))
    positions=[[(i%side-(side-1)/2)*.12,(i//side-(side-1)/2)*.12,0.] for i in range(count)]
    asset.model['nodes']=[dict(name='Animated grid root',children=[1]),dict(name='Shared instances',mesh=0,
        extensions={'EXT_mesh_gpu_instancing':dict(attributes=dict(TRANSLATION=asset.accessor('VEC3',positions)))})]
    asset.model['extensionsUsed']=['EXT_mesh_gpu_instancing']
    asset.model['extensionsRequired']=['EXT_mesh_gpu_instancing']
    times=asset.accessor('SCALAR',[0.,2.]);translation=asset.accessor('VEC3',[[-.35,0.,0.],[.35,.2,0.]])
    asset.model['animations']=[dict(name='Whole grid translation',samplers=[dict(input=times,output=translation)],
        channels=[dict(sampler=0,target=dict(node=0,path='translation'))])]
    asset.write(path)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--instances',type=int,default=20000)
    parser.add_argument('--technique',action='append',choices=('deferred-raster','forward-raster'))
    parser.add_argument('--ray',action='store_true',help='Include ray-scene extraction/build/trace cost')
    parser.add_argument('--no-validation',action='store_true',help='Measure without validation after a separate validated run')
    args=parser.parse_args()
    if not 1000<=args.instances<=100000:parser.error('instances must be 1000 to 100000')
    renderer,output=args.renderer.resolve(),args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    model=output/'large-grid.gltf';fixture(model,args.instances)
    frames=[8,16,24,32,40,48]
    sequence=output/'benchmark-sequence.json'
    sequence.write_text(json.dumps(dict(schemaVersion=1,frames=frames[-1],sampleFrames=frames)))
    report=dict(passed=False,executableSha256=hashlib.sha256(renderer.read_bytes()).hexdigest(),instances=args.instances,
                ray=args.ray,validationEnabled=not args.no_validation,cases=[])
    def capture(technique,gpu):
        base=output/(technique+('-gpu' if gpu else '-cpu'));log=Path(str(base)+'.log')
        command=[str(renderer),'--hidden','--no-ui','--no-validation' if args.no_validation else '--validation','--no-taa','--no-bloom',
                 '--render-technique',technique,'--width','640','--height','360','--model',str(model),
                 '--max-scene-objects',str(args.instances),'--import-scale','1','--animation-clip','0',
                 '--fixed-dt','.016666667','--camera-position','0','0','22','--camera-target','0','0','0',
                 '--environment-intensity','0','--directional-intensity','3','--directional-direction','0','0','-1',
                 '--exposure','1','--screenshot-viewport','--capture-sequence',str(sequence),
                 '--screenshot',str(base)+'.png']
        if gpu:command.append('--gpu-transforms')
        if args.ray:command+=['--ray-shadows','hard','--no-ray-shadow-denoise']
        for frame in frames:
            stem=base if frame==frames[-1] else Path(f'{base}.frame-{frame:04d}')
            Path(str(stem)+'.png').unlink(missing_ok=True);Path(str(stem)+'.telemetry.json').unlink(missing_ok=True)
        with log.open('w',encoding='utf-8') as stream:
            result=subprocess.run(command,cwd=renderer.parent,stdout=stream,stderr=subprocess.STDOUT,timeout=600,
                env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(output)),creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        if result.returncode or validation_log_error(log.read_text(errors='replace')):raise RuntimeError(str(log))
        samples=[]
        for frame in frames:
            stem=base if frame==frames[-1] else Path(f'{base}.frame-{frame:04d}')
            state=json.loads(Path(str(stem)+'.telemetry.json').read_text())
            hierarchy=state['hierarchyTransforms']
            if state['scene']['primaryObjectCount']!=args.instances or hierarchy['active']!=gpu:
                raise RuntimeError('Large-scene mode/capacity was not exercised')
            if gpu:
                if hierarchy['cpuObjectUploadBytes'] or hierarchy['objectMetadataBytes']!=112*args.instances:
                    raise RuntimeError('GPU path retained full object uploads or omitted changing bounds')
                if not hierarchy['dispatchedLevels'] or hierarchy['dirtyLocals']!=1:
                    raise RuntimeError('Large hierarchy did not use one dirty animated local')
            elif hierarchy['cpuObjectUploadBytes']!=224*args.instances:
                raise RuntimeError('CPU baseline did not upload changing complete object rows')
            if args.ray:
                build=state['rayShadows']['build']
                if build['instances']!=args.instances or build['gpuInstances']!=(args.instances if gpu else 0):
                    raise RuntimeError('Large ray-instance publication was not exercised')
                if build['blasBuilt'] or build['blasReused']!=1:
                    raise RuntimeError('Changing transforms rebuilt shared geometry')
            samples.append(dict(frame=frame,image=str(stem)+'.png',telemetry=str(stem)+'.telemetry.json',
                hierarchy=hierarchy,cpuPhasesMs=state['cpuPhasesMs'],frameGraphGpuMs=state['gpuKnownMs'],
                frameHistory=state['frameHistory'],
                rayBuild=state['rayShadows'].get('build'),passes=state['passes']))
        if pixels(samples[0]['image'],samples[-1]['image'])['meanAbsoluteError']<.01:
            raise RuntimeError('Large grid animation was not visible')
        history={row['frameIndex']:row for sample in samples for row in sample['frameHistory']}
        steady=[row for index,row in history.items() if index>=8 and index not in {frame-1 for frame in frames}
                and row['cpuPhasesMs']['Screenshot']==0]
        if len(steady)<16:raise RuntimeError('Insufficient uncaptured warmed-up frame timings')
        summary=dict(cpuPhasesMs={key:statistics.median(s['cpuPhasesMs'][key] for s in steady) for key in samples[0]['cpuPhasesMs']},
                     measuredCpuFrames=len(steady),measuredFrameIndices=sorted(s['frameIndex'] for s in steady),
                     frameGraphGpuMs=statistics.median(s['gpuKnownMs'] for s in steady),
                     cpuObjectBytes=statistics.median(s['hierarchy']['cpuObjectUploadBytes'] for s in samples))
        if gpu:
            summary.update(hierarchyPrepareMs=statistics.median(s['hierarchy']['prepareMs'] for s in samples),
                metadataPrepareMs=statistics.median(s['hierarchy']['objectMetadataMs'] for s in samples),
                hierarchyGpuMs=statistics.median(s['hierarchy']['previousGpuMs'] for s in samples),
                metadataBytes=statistics.median(s['hierarchy']['objectMetadataBytes'] for s in samples),
                localTopologyBindingBytes=statistics.median(s['hierarchy']['uploadedBytes'] for s in samples))
        return dict(summary=summary,samples=samples,log=str(log))
    try:
        for technique in args.technique or ('deferred-raster','forward-raster'):
            cpu,gpu=capture(technique,False),capture(technique,True)
            differences=[pixels(a['image'],b['image']) for a,b in zip(cpu['samples'],gpu['samples'],strict=True)]
            if max(d['maximumError'] for d in differences)>1:raise RuntimeError('Large GPU/CPU image mismatch')
            report['cases'].append(dict(technique=technique,cpu=cpu,gpu=gpu,comparisons=differences))
            print(technique+': large-scene CPU/GPU benchmark passed',flush=True)
            (output/'results.json').write_text(json.dumps(report,indent=2))
        if hashlib.sha256(renderer.read_bytes()).hexdigest()!=report['executableSha256']:raise RuntimeError('Executable changed')
        report['passed']=True
    finally:
        (output/'results.json').write_text(json.dumps(report,indent=2))


if __name__=='__main__':main()
