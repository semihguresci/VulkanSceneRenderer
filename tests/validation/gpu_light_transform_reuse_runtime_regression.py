"""Require light intensity/position animation to retain the GPU-built ray generation."""
import argparse
import hashlib
import json
from pathlib import Path

from gltf_accessor_runtime_regression import Asset, mesh
from gltf_material_variants_runtime_regression import run, pixels


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    renderer,output=args.renderer.resolve(),args.output.resolve()
    output.mkdir(parents=True,exist_ok=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    asset=Asset()
    mesh(asset,asset.accessor('VEC3',[[-.8,-.7,0],[.8,-.7,0],[0,.8,0]]),
         asset.accessor('VEC3',[[0,0,1]]*3),asset.accessor('VEC2',[[0,0],[1,0],[.5,1]]),
         indices=asset.accessor('SCALAR',[0,1,2],5123,indices=True))
    asset.model['materials']=[dict(pbrMetallicRoughness=dict(baseColorFactor=[.7,.7,.7,1],metallicFactor=0,roughnessFactor=.7))]
    asset.model['nodes']=[dict(mesh=0,name='Stationary receiver'),dict(name='Animated lamp',translation=[0,0,2],
        extensions={'KHR_lights_punctual':dict(light=0)})]
    asset.model['scenes'][0]['nodes']=[0,1]
    asset.model['extensions']={'KHR_lights_punctual':dict(lights=[dict(type='point',intensity=2,range=8)])}
    asset.model['extensionsUsed']=['KHR_lights_punctual','KHR_animation_pointer']
    times=asset.accessor('SCALAR',[0.,1.]);values=asset.accessor('SCALAR',[2.,80.])
    asset.model['animations']=[dict(name='Light intensity only',samplers=[dict(input=times,output=values)],
        channels=[dict(sampler=0,target=dict(path='pointer',extensions={'KHR_animation_pointer':dict(
            pointer='/extensions/KHR_lights_punctual/lights/0/intensity')}))])]
    positions=asset.accessor('VEC3',[[0.,0.,2.],[.7,.2,2.]])
    asset.model['animations'].append(dict(name='Light position only',samplers=[dict(input=times,output=positions)],
        channels=[dict(sampler=0,target=dict(node=1,path='translation'))]))
    model=output/'light-only.gltf';asset.write(model)
    sequence=output/'light-only.json'
    sequence.write_text(json.dumps(dict(schemaVersion=1,frames=32,sampleFrames=[4,8,12,16,20,21,28,32],events=[
        dict(frame=1,animationPlaying=False,animationClip=0,animationTime=0),dict(frame=5,animationTime=1),
        dict(frame=9,animationTime=0),dict(frame=13,animationTime=1),
        dict(frame=17,animationClip=1,animationTime=0),dict(frame=21,animationTime=1),
        dict(frame=25,animationTime=0),dict(frame=29,animationTime=1)])))
    report=dict(passed=False,executableSha256=hashlib.sha256(renderer.read_bytes()).hexdigest(),cases=[])
    try:
        for technique in ('deferred-raster','forward-raster'):
            samples=run(renderer,output,model,technique,technique,'lit',[],sequence=sequence,extra=(
                '--gpu-transforms','--directional-intensity','0','--ray-shadows','hard','--ddgi','--ddgi-async',
                '--ddgi-counts','3','3','3','--ddgi-rays','4','--ddgi-probes-per-frame','27',
                '--ddgi-ray-budget','108','--ddgi-intensity','0','--screenshot-viewport','--no-ray-shadow-denoise'))
            if len(samples)!=8: raise RuntimeError('Missing light-only capture')
            generations={s['state']['rayShadows']['generation'] for s in samples}
            if len(generations)!=1 or not next(iter(generations)):
                raise RuntimeError('Light-only edits rebuilt the ray generation')
            for index,sample in enumerate(samples):
                state=sample['state']
                if state['rayShadows']['build']['gpuInstances']!=1 or not state['rayShadows']['active']:
                    raise RuntimeError('Light fixture did not use GPU TLAS transforms')
                if not state['ddgi']['asyncActive'] or not state['ddgi']['asyncPublishedUpdates']:
                    raise RuntimeError('Light fixture did not publish asynchronous DDGI')
                if index<4 and state['hierarchyTransforms']['dispatchedLevels']:
                    raise RuntimeError('Light intensity edits dispatched hierarchy levels')
            if not samples[5]['state']['hierarchyTransforms']['dispatchedLevels']:
                raise RuntimeError('Light translation did not update its graph transform')
            change=pixels(samples[0]['image'],samples[1]['image'])
            if change['meanAbsoluteError']<1: raise RuntimeError('Authored light animation was not visible')
            if pixels(samples[0]['image'],samples[2]['image'])['maximumError'] or pixels(samples[1]['image'],samples[3]['image'])['maximumError']:
                raise RuntimeError('Light animation did not restore the original intensities')
            position_change=pixels(samples[4]['image'],samples[5]['image'])
            if position_change['meanAbsoluteError']<.01: raise RuntimeError('Light translation was not visible')
            if pixels(samples[4]['image'],samples[6]['image'])['maximumError'] or pixels(samples[5]['image'],samples[7]['image'])['maximumError']:
                raise RuntimeError('Light animation did not restore the original positions')
            report['cases'].append(dict(technique=technique,generation=next(iter(generations)),change=change,
                positionChange=position_change,
                samples=[{k:s[k] for k in ('image','telemetry','log')} for s in samples]))
            print(technique+': light-only GPU generation reuse passed',flush=True)
        if hashlib.sha256(renderer.read_bytes()).hexdigest()!=report['executableSha256']:
            raise RuntimeError('Executable changed during validation')
        report['passed']=True
    finally:
        (output/'results.json').write_text(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
