"""Validate closed glTF glass volumes in the Vulkan editor and raw EXR path."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess

os.environ.setdefault('OPENCV_IO_ENABLE_OPENEXR','1')
import cv2
import numpy as np

from gltf_accessor_runtime_regression import Asset
from gltf_extended_materials_runtime_regression import validation_log_error

WIDTH,HEIGHT,SAMPLES=320,240,64
LIGHT=np.array([4.,2.,1.])


def fixture(path,profile):
    asset=Asset();asset.model['meshes']=[]
    asset.model['nodes']=[dict(name='Interior emitter',mesh=0),dict(name='Orthographic camera',camera=0,translation=[0,0,2])]
    asset.model['materials']=[dict(pbrMetallicRoughness=dict(baseColorFactor=[0,0,0,1],metallicFactor=0),
        emissiveFactor=[1,.5,.25],extensions={'KHR_materials_specular':dict(specularFactor=0),
            'KHR_materials_emissive_strength':dict(emissiveStrength=4)})]
    asset.model['cameras']=[dict(type='orthographic',orthographic=dict(xmag=.125,ymag=.09375,znear=.1,zfar=100))]

    def primitive(positions,normal,tangent,material):
        return dict(attributes=dict(POSITION=asset.accessor('VEC3',positions),
            NORMAL=asset.accessor('VEC3',[normal]*4),TANGENT=asset.accessor('VEC4',[[*tangent,1]]*4),
            TEXCOORD_0=asset.accessor('VEC2',[[0,0],[1,0],[1,1],[0,1]])),
            indices=asset.accessor('SCALAR',[0,1,2,0,2,3],5123,indices=True),material=material)

    def emitter(z=-2,x=0,half=4,front=True):
        positions=[[x-half,-4,z],[x+half,-4,z],[x+half,4,z],[x-half,4,z]]
        if not front: positions=positions[::-1]
        asset.model['meshes'].append(dict(primitives=[primitive(positions,[0,0,1 if front else -1],[1,0,0],0)]))

    # Explicit cube faces, authored independently of renderer/test geometry.
    faces=[([[.5,-.5,.5],[.5,-.5,-.5],[.5,.5,-.5],[.5,.5,.5]],[1,0,0],[0,0,-1]),
        ([[-.5,-.5,-.5],[-.5,-.5,.5],[-.5,.5,.5],[-.5,.5,-.5]],[-1,0,0],[0,0,1]),
        ([[-.5,.5,.5],[.5,.5,.5],[.5,.5,-.5],[-.5,.5,-.5]],[0,1,0],[1,0,0]),
        ([[-.5,-.5,-.5],[.5,-.5,-.5],[.5,-.5,.5],[-.5,-.5,.5]],[0,-1,0],[1,0,0]),
        ([[-.5,-.5,.5],[.5,-.5,.5],[.5,.5,.5],[-.5,.5,.5]],[0,0,1],[1,0,0]),
        ([[.5,-.5,-.5],[-.5,-.5,-.5],[-.5,.5,-.5],[.5,.5,-.5]],[0,0,-1],[-1,0,0])]

    def volume(scale,color=(.5,.25,1),ior=1,specular=0,roughness=1,distance=1,translation=(0,0,0),open_mesh=False,
               metallic=0,base=(1,1,1)):
        material=len(asset.model['materials'])
        extension=dict(thicknessFactor=9,attenuationColor=list(color))
        if distance is not None: extension['attenuationDistance']=distance
        # A zero baked thickness texel must not turn a traced solid into a sheet.
        tex=asset.image(np.tile(np.array([0,0,0,255],dtype=np.uint8),(2,2,1)))
        extension['thicknessTexture']=dict(index=tex)
        asset.model['materials'].append(dict(pbrMetallicRoughness=dict(baseColorFactor=[*base,1],metallicFactor=metallic,roughnessFactor=roughness),
            extensions={'KHR_materials_specular':dict(specularFactor=specular),'KHR_materials_ior':dict(ior=ior),
                'KHR_materials_transmission':dict(transmissionFactor=1),'KHR_materials_volume':extension}))
        mesh=len(asset.model['meshes'])
        selected=faces[:-1] if open_mesh else faces
        asset.model['meshes'].append(dict(primitives=[primitive(p,n,t,material) for p,n,t in selected]))
        asset.model['nodes'].append(dict(name='Closed homogeneous glass',mesh=mesh,scale=list(scale),translation=list(translation)))
        return material

    expected=None
    if profile.startswith('snell'):
        angle=-math.asin(.6);c,s=math.cos(angle),math.sin(angle)
        matrix=np.array([[c,0,s,-1],[0,1,0,0],[-s,0,c,2],[0,0,0,1]])
        asset.model['nodes'][1]=dict(camera=0,matrix=matrix.T.reshape(-1).tolist())
        emitter(z=0,x=-.25+.4/math.sqrt(1-.4**2),half=.18)
        volume((10,10,2),color=(1,1,1),ior=1 if profile.endswith('matched') else 1.5,roughness=0,distance=None)
        if not profile.endswith('matched'): expected=LIGHT/2.25
    elif profile.startswith('tir'):
        angle=-math.asin(.8);c,s=math.cos(angle),math.sin(angle)
        matrix=np.array([[c,0,s,0],[0,1,0,0],[-s,0,c,.5],[0,0,0,1]])
        asset.model['nodes'][1]=dict(camera=0,matrix=matrix.T.reshape(-1).tolist())
        emitter(z=0,x=10/3,half=.23,front=False)
        volume((20,20,2),color=(1,1,1),ior=1 if profile.endswith('matched') else 1.5,roughness=0,distance=None,
               metallic=.25 if profile=='tir-metallic' else 0,base=(.2,.4,.8) if profile=='tir-metallic' else (1,1,1))
        expected=np.zeros(3) if profile.endswith('matched') else LIGHT
        if profile=='tir-metallic':
            color=np.array([.2,.4,.8]);expected=LIGHT*(.75+.25*(color+(1-color)*.4**5))
    elif profile=='shadow-metallic':
        emitter(z=0)
        asset.model['materials'][0]=dict(pbrMetallicRoughness=dict(baseColorFactor=[.5,.5,.5,1],metallicFactor=0,roughnessFactor=1),
            extensions={'KHR_materials_specular':dict(specularFactor=0)})
        volume((.6,.8,.6),color=(1,1,1),ior=1,specular=1,roughness=0,distance=None,
               translation=(1,0,1),metallic=.25,base=(.5,.5,.5))
        asset.model['extensions']={'KHR_lights_punctual':dict(lights=[dict(type='point',intensity=8)])}
        asset.model['nodes'].append(dict(translation=[2,0,2],extensions={'KHR_lights_punctual':dict(light=0)}))
        coordinate=(np.arange(256)+.5)/256
        x,y=np.meshgrid(coordinate*.25-.125,coordinate*.1875-.09375)
        square=(2-x)**2+y*y+4
        value=(4/math.pi)*(2/np.sqrt(square))/square*(.375**2)
        expected=np.full(3,value.mean())
    else:
        emitter(z=0 if 'inside' in profile else -2)
        if profile=='nested':
            volume((6,6,3),color=(.5,1,1));volume((4,4,1),color=(1,.5,1));expected=np.ones(3)
        elif profile=='nested-ior':
            volume((6,6,3),color=(1,1,1),ior=1.5,roughness=0,distance=None)
            volume((4,4,1),color=(1,1,1),ior=2.4,roughness=0,distance=None);expected=LIGHT
        elif profile=='camera-inside':
            volume((6,6,6),ior=1.5);expected=LIGHT*np.power([.5,.25,1],1.9)
        elif profile=='nested-camera-inside':
            volume((8,8,8),ior=1.5);volume((6,6,6),color=(.25,.5,1),ior=2.4)
            expected=LIGHT*np.power([.25,.5,1],1.9)
        elif profile=='smooth-glass':
            volume((4,4,2),color=(1,1,1),ior=1.5,specular=1,roughness=0,distance=None)
            expected=LIGHT*(12/13)
        elif profile=='rough-glass':
            volume((4,4,2),color=(1,1,1),ior=1.5,specular=1,roughness=.6,distance=None)
        elif profile=='black':
            volume((4,4,2),color=(0,0,0));expected=np.zeros(3)
        elif profile=='infinite-black':
            volume((4,4,2),color=(0,0,0),distance=None);expected=LIGHT
        elif profile=='overlap':
            volume((4,4,1),color=(.5,1,1),translation=(0,0,1));volume((4,4,1),color=(1,.5,1),translation=(0,0,.5))
        else:
            volume((-4,4,2) if profile=='mirrored' else (4,4,2),open_mesh=profile=='open-volume')
            if profile!='open-volume': expected=np.array([1,.125,1])
    asset.model['scenes']=[dict(nodes=list(range(len(asset.model['nodes']))))]
    asset.model['extensionsUsed']=sorted({e for m in asset.model['materials'] for e in m.get('extensions',{})})
    if profile=='shadow-metallic': asset.model['extensionsUsed'].append('KHR_lights_punctual')
    asset.write(path)
    return expected


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();renderer,output=args.renderer.resolve(),args.output.resolve()
    output.mkdir(parents=True,exist_ok=True);(output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    digest=hashlib.sha256(renderer.read_bytes()).hexdigest()
    report=dict(passed=False,executableSha256=digest,captures=0,runs=[])

    def capture(profile,frames=(4,),events=()):
        base=output/profile;model=base.with_suffix('.gltf');expected=fixture(model,profile)
        sequence=base.with_suffix('.sequence.json')
        sequence.write_text(json.dumps(dict(schemaVersion=1,frames=max(frames),sampleFrames=list(frames),
            events=[dict(frame=1,cameraSelection=dict(sourceNode=1)),*events])))
        command=[str(renderer),'--hidden','--no-ui','--validation','--no-taa','--no-bloom',
            '--width',str(WIDTH),'--height',str(HEIGHT),'--msaa','1','--fixed-dt','.016666667',
            '--model',str(model),'--import-scale','1','--render-technique','path-tracing','--display-mode','lit',
            '--directional-intensity','0','--environment-intensity','0','--path-spp','16','--path-samples',str(SAMPLES),
            '--path-bounces','1' if profile=='shadow-metallic' else '32','--capture-sequence',str(sequence),'--screenshot',str(base.with_suffix('.exr')),'--screenshot-viewport']
        log=base.with_suffix('.log')
        with log.open('w',encoding='utf-8') as stream:
            process=subprocess.run(command,cwd=renderer.parent,stdout=stream,stderr=subprocess.STDOUT,
                timeout=240,env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(output)),creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        assert process.returncode==0 and not validation_log_error(log.read_text(encoding='utf-8',errors='replace')),log
        samples=[]
        for frame in frames:
            stem=base if frame==max(frames) else Path(f'{base}.frame-{frame:04d}')
            state=json.loads(Path(f'{stem}.telemetry.json').read_text());path=state['referencePath']
            rgb=cv2.imdecode(np.frombuffer(Path(f'{stem}.exr').read_bytes(),dtype=np.uint8),cv2.IMREAD_UNCHANGED)[:,:,::-1]
            assert rgb.dtype==np.float32 and rgb.shape==(HEIGHT,WIDTH,3) and np.isfinite(rgb).all()
            assert state['technique']=='path-tracing' and path['attemptedSamples']==SAMPLES and path['rejectedSamples']==0,path
            if profile in ('open-volume','overlap'):
                assert path['unsupportedSamples']==WIDTH*HEIGHT*SAMPLES,path
                np.testing.assert_array_equal(rgb,0)
            else:
                assert path['unsupportedSamples']==0,path
            metadata=json.loads(Path(f'{stem}.exr.capture.json').read_text())
            assert metadata['referencePath']['attemptedSamplesPerPixel']==SAMPLES
            samples.append((rgb,state));report['captures']+=1
        report['runs'].append(dict(profile=profile,frames=list(frames),log=str(log),expected=None if expected is None else expected.tolist(),referencePaths=[s[1]['referencePath'] for s in samples]))
        return samples,expected

    try:
        for profile in ['world-distance','mirrored','nested','nested-ior','camera-inside','nested-camera-inside',
                        'smooth-glass','rough-glass','snell','snell-matched','tir','tir-matched','tir-metallic','shadow-metallic',
                        'black','infinite-black','open-volume','overlap']:
            samples,expected=capture(profile);rgb=samples[0][0]
            if profile in ('smooth-glass','nested-ior'):
                # Default application roulette starts at depth three. Its
                # compensated samples are stochastic even at normal incidence.
                np.testing.assert_allclose(rgb.mean(axis=(0,1),dtype=np.float64),expected,atol=.015,rtol=0)
            elif profile=='shadow-metallic': np.testing.assert_allclose(rgb.mean(axis=(0,1),dtype=np.float64),expected,atol=.00002,rtol=0)
            elif expected is not None: np.testing.assert_allclose(rgb,np.broadcast_to(expected,rgb.shape),atol=.00001,rtol=0)
            elif profile=='snell-matched': assert rgb[:,:,0].mean()<1
            elif profile=='rough-glass': assert rgb[:,:,0].mean()>.01
        reload,_=capture('reload',(4,8),[dict(frame=5,reloadShaders=True)])
        np.testing.assert_array_equal(reload[0][0],reload[1][0]);assert reload[1][1]['shaderReload']['generation']==1
        assert [s[1]['referencePath']['historyResets'] for s in reload]==[1,2]
        history,_=capture('history',(4,8,12,16),[
            dict(frame=5,materialEdit=dict(sourceMaterial=1,values=[dict(property='extensions/KHR_materials_volume/attenuationDistance',value=2)])),
            dict(frame=9,sceneHistory='undo'),dict(frame=13,sceneHistory='redo')])
        np.testing.assert_allclose(history[1][0],np.broadcast_to([2,.5,1],history[1][0].shape),atol=.00001,rtol=0)
        np.testing.assert_array_equal(history[0][0],history[2][0]);np.testing.assert_array_equal(history[1][0],history[3][0])
        assert [s[1]['referencePath']['historyResets'] for s in history]==[1,2,3,4]
        assert hashlib.sha256(renderer.read_bytes()).hexdigest()==digest
        report['passed']=True
    finally:
        (output/'results.json').write_text(json.dumps(report,indent=2))


if __name__=='__main__': main()
