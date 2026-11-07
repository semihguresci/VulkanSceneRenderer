"""Validate reference camera framing with independent emissive image regions."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess

os.environ.setdefault('OPENCV_IO_ENABLE_OPENEXR', '1')
import cv2
import numpy as np

from gltf_accessor_runtime_regression import Asset
from gltf_extended_materials_runtime_regression import validation_log_error

WIDTH, HEIGHT = 320, 240
COLORS = ([4,.25,.0625],[.125,2,.25],[.25,.125,3],[1.5,1,.5])
QUADS = [(-.65,.4,.45,.3),(.65,.4,.45,.3),(-.65,-.4,.45,.3),(.65,-.4,.45,.3)]


def world_pose(rotated):
    if not rotated:
        return np.eye(4)
    # Rodrigues rotation and an independent camera-local roll.
    axis = np.array([.3,.7,-.2]); axis /= np.linalg.norm(axis)
    x,y,z=axis; angle=.73; c,s=math.cos(angle),math.sin(angle)
    cross=np.array([[0,-z,y],[z,0,-x],[-y,x,0]])
    rotation=np.eye(3)*c+(1-c)*np.outer(axis,axis)+s*cross
    roll=.39; cr,sr=math.cos(roll),math.sin(roll)
    result=np.eye(4);result[:3,:3]=rotation@np.array([[cr,-sr,0],[sr,cr,0],[0,0,1]])
    result[:3,3]=[.183,-.416,3.714]
    return result


def fixture(path, profile, rotated):
    asset=Asset();primitives=[]
    for i,(x,y,hx,hy) in enumerate(QUADS):
        positions=[[x-hx,y-hy,-4],[x+hx,y-hy,-4],[x+hx,y+hy,-4],[x-hx,y+hy,-4]]
        primitives.append(dict(attributes=dict(POSITION=asset.accessor('VEC3',positions),
            NORMAL=asset.accessor('VEC3',[[0,0,1]]*4),TANGENT=asset.accessor('VEC4',[[1,0,0,1]]*4),
            TEXCOORD_0=asset.accessor('VEC2',[[0,0],[1,0],[1,1],[0,1]])),
            indices=asset.accessor('SCALAR',[0,1,2,0,2,3],5123,indices=True),material=i))
    asset.model['meshes']=[dict(primitives=primitives)]
    asset.model['materials']=[dict(pbrMetallicRoughness=dict(baseColorFactor=[0,0,0,1],metallicFactor=0),
        emissiveFactor=[c/4 for c in color],extensions={'KHR_materials_specular':dict(specularFactor=0),
            'KHR_materials_emissive_strength':dict(emissiveStrength=4)}) for color in COLORS]
    asset.model['extensionsUsed']=['KHR_materials_specular','KHR_materials_emissive_strength']
    camera=dict(type='perspective',perspective=dict(yfov=math.pi/4,aspectRatio=WIDTH/HEIGHT,znear=.1,zfar=100))
    if profile.startswith('ortho'):
        camera=dict(type='orthographic',orthographic=dict(xmag=4*math.tan(math.pi/8)*WIDTH/HEIGHT,
            ymag=4*math.tan(math.pi/8),znear=.1,zfar=100))
    values=camera[camera['type']]
    if profile=='infinite': values.pop('zfar')
    if profile.endswith('near'): values['znear']=5
    if profile.endswith('far'): values['zfar']=3
    matrix=world_pose(rotated).T.reshape(-1).tolist()
    asset.model['nodes']=[dict(name='Emissive camera-local quadrants',mesh=0,matrix=matrix),
                          dict(name='Authored camera',camera=0,matrix=matrix)]
    asset.model['cameras']=[camera];asset.model['scenes']=[dict(nodes=[0,1])]
    asset.write(path)


def verify_regions(rgb, profile, technique):
    assert rgb.shape==(HEIGHT,WIDTH,3) and rgb.dtype==np.float32 and np.isfinite(rgb).all()
    if profile.endswith(('near','far')):
        np.testing.assert_array_equal(rgb,0)
        return
    # Independent camera-local projection: all patches are four world units away.
    scale=HEIGHT/(8*math.tan(math.pi/8))
    outside=np.ones((HEIGHT,WIDTH),dtype=bool)
    for (x,y,hx,hy),color in zip(QUADS,COLORS,strict=True):
        x0,x1=WIDTH/2+(x-hx)*scale,WIDTH/2+(x+hx)*scale
        y0,y1=HEIGHT/2-(y+hy)*scale,HEIGHT/2-(y-hy)*scale
        region=rgb[math.ceil(y0)+1:math.floor(y1)-1,math.ceil(x0)+1:math.floor(x1)-1]
        assert region.size
        if technique=='path-tracing':
            np.testing.assert_allclose(region,np.broadcast_to(color,region.shape),atol=2e-6,rtol=0)
        else:
            # Raster stores RGBA16F. Bound conversion/interpolation rounding by
            # one adjacent representable value, independent of renderer output.
            value=np.asarray(color,dtype=np.float16)
            low=np.nextafter(value,np.float16(-np.inf)).astype(np.float32)
            high=np.nextafter(value,np.float16(np.inf)).astype(np.float32)
            assert np.all(region>=low-1e-6) and np.all(region<=high+1e-6)
        outside[max(0,math.floor(y0)-1):min(HEIGHT,math.ceil(y1)+1),
                max(0,math.floor(x0)-1):min(WIDTH,math.ceil(x1)+1)]=False
    np.testing.assert_array_equal(rgb[outside],0)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();renderer,output=args.renderer.resolve(),args.output.resolve()
    output.mkdir(parents=True,exist_ok=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    digest=hashlib.sha256(renderer.read_bytes()).hexdigest()
    report=dict(passed=False,executableSha256=digest,captures=0,cases=[])
    try:
        for profile in ('perspective','infinite','ortho','perspective-near','perspective-far','ortho-near','ortho-far'):
            for rotated in (False,True):
                model=output/f'{profile}-{"rotated" if rotated else "identity"}.gltf'
                fixture(model,profile,rotated)
                techniques=('path-tracing','deferred-raster','forward-raster') if profile in ('perspective','ortho') and rotated else ('path-tracing',)
                for technique in techniques:
                    base=model.with_name(model.stem+'-'+technique).with_suffix('')
                    sequence=base.with_suffix('.sequence.json')
                    sequence.write_text(json.dumps(dict(schemaVersion=1,frames=6,sampleFrames=[6],
                        events=[dict(frame=1,cameraSelection=dict(sourceNode=1))])))
                    command=[str(renderer),'--model',str(model),'--import-scale','1','--render-technique',technique,
                        '--hidden','--no-ui','--validation','--no-taa','--no-bloom','--width',str(WIDTH),'--height',str(HEIGHT),
                        '--msaa','1','--directional-intensity','0','--environment-intensity','0','--display-mode','lit',
                        '--path-spp','4','--path-samples','13','--path-bounces','4','--capture-sequence',str(sequence),
                        '--screenshot',str(base.with_suffix('.exr')),'--screenshot-viewport']
                    with base.with_suffix('.log').open('w',encoding='utf-8') as log:
                        process=subprocess.run(command,cwd=renderer.parent,stdout=log,stderr=subprocess.STDOUT,
                            timeout=120,env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(output)),
                            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                    assert process.returncode==0 and not validation_log_error(base.with_suffix('.log').read_text(errors='replace')),base
                    image=base.with_suffix('.exr')
                    rgb=cv2.imdecode(np.frombuffer(image.read_bytes(),dtype=np.uint8),cv2.IMREAD_UNCHANGED)[:,:,::-1]
                    verify_regions(rgb,profile,technique)
                    telemetry=json.loads(base.with_suffix('.telemetry.json').read_text())
                    if technique=='path-tracing':
                        state=telemetry['referencePath']
                        assert state['attemptedSamples']==13 and state['historyResets']==1,state
                        assert state['unsupportedSamples']==state['rejectedSamples']==0,state
                    report['cases'].append(dict(profile=profile,rotated=rotated,technique=technique,image=str(image)))
                    report['captures']+=1
                    print(f'{profile} rotated={rotated} {technique}: passed',flush=True)
                    (output/'results.json').write_text(json.dumps(report,indent=2))
        assert hashlib.sha256(renderer.read_bytes()).hexdigest()==digest
        report['passed']=True
    finally:
        (output/'results.json').write_text(json.dumps(report,indent=2))


if __name__=='__main__': main()
