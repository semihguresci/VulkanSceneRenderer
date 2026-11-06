"""Validate thin transmission through the real Vulkan viewport and raw export."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

os.environ.setdefault('OPENCV_IO_ENABLE_OPENEXR', '1')
import cv2
import numpy as np

from gltf_accessor_runtime_regression import Asset
from gltf_extended_materials_runtime_regression import validation_log_error


def fixture(path, profile):
    asset = Asset()
    sheets = [[.8,.4,.2],[.5,.75,1]] if profile != 'textured' else [[1,1,1]]
    materials = []
    primitives = []
    for index, color in enumerate([*sheets, [0,0,0]]):
        z = -2-index if index < len(sheets) else -6
        primitives.append(dict(attributes=dict(
            POSITION=asset.accessor('VEC3',[[-10,-10,z],[10,-10,z],[10,10,z],[-10,10,z]]),
            NORMAL=asset.accessor('VEC3',[[0,0,1]]*4),
            TANGENT=asset.accessor('VEC4',[[1,0,0,1]]*4),
            TEXCOORD_0=asset.accessor('VEC2',[[0,0],[1,0],[1,1],[0,1]])),
            indices=asset.accessor('SCALAR',[0,1,2,0,2,3],5123,indices=True),material=index))
        material = dict(pbrMetallicRoughness=dict(baseColorFactor=[*color,1],metallicFactor=0,
            roughnessFactor=1 if profile == 'ior-one' else 0),
            extensions={'KHR_materials_specular':dict(specularFactor=0)})
        if index < len(sheets):
            material['extensions']['KHR_materials_transmission'] = dict(transmissionFactor=1)
            if profile == 'ior-one':
                material['extensions']['KHR_materials_ior'] = dict(ior=1)
            if profile == 'solid-volume':
                material['extensions']['KHR_materials_volume'] = dict(thicknessFactor=1)
            if profile == 'textured':
                texture=asset.image(np.tile(np.array([128,64,192,255],dtype=np.uint8),(2,2,1)))
                material['extensions']['KHR_materials_transmission']['transmissionTexture'] = dict(index=texture)
        else:
            material['emissiveFactor'] = [1,.5,.25]
            material['extensions']['KHR_materials_emissive_strength'] = dict(emissiveStrength=4)
        materials.append(material)
    asset.model['materials']=materials
    asset.model['meshes']=[dict(primitives=primitives)]
    asset.model['extensionsUsed']=sorted({extension for m in materials for extension in m['extensions']})
    asset.write(path)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    renderer,output=args.renderer.resolve(),args.output.resolve()
    output.mkdir(parents=True,exist_ok=True)
    (output/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n')
    executable_hash=hashlib.sha256(renderer.read_bytes()).hexdigest()
    report=dict(passed=False,executableSha256=executable_hash,captures=0,runs=[])

    def capture(profile,frames=(4,),events=()):
        model=output/(profile+'.gltf');fixture(model,profile)
        sequence=output/(profile+'.sequence.json')
        sequence.write_text(json.dumps(dict(schemaVersion=1,frames=max(frames),sampleFrames=list(frames),events=list(events))))
        log=output/(profile+'.log')
        command=[str(renderer),'--hidden','--validation','--no-ui','--screenshot-viewport',
            '--width','320','--height','240','--msaa','1','--fixed-dt','.016666667',
            '--model',str(model),'--import-scale','1','--render-technique','path-tracing',
            '--camera-position','0','0','0','--camera-target','0','0','-1',
            '--environment-intensity','0','--directional-intensity','0','--no-bloom','--no-taa',
            '--display-mode','lit','--exposure','1',
            '--path-spp','4','--path-samples','13','--path-bounces','1',
            '--capture-sequence',str(sequence),'--screenshot',str(output/(profile+'.exr'))]
        with log.open('w',encoding='utf-8') as stream:
            process=subprocess.run(command,cwd=renderer.parent,stdout=stream,stderr=subprocess.STDOUT,
                timeout=240,env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(output)),
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        assert process.returncode==0 and not validation_log_error(log.read_text(encoding='utf-8',errors='replace')),log
        samples=[]
        for frame in frames:
            stem=output/(profile if frame==max(frames) else f'{profile}.frame-{frame:04d}')
            state=json.loads(Path(f'{stem}.telemetry.json').read_text())
            metadata=json.loads(Path(f'{stem}.exr.capture.json').read_text())
            rgb=cv2.imdecode(np.frombuffer(Path(f'{stem}.exr').read_bytes(),dtype=np.uint8),cv2.IMREAD_UNCHANGED)[:,:,::-1]
            assert rgb.dtype==np.float32 and rgb.shape==(240,320,3) and np.isfinite(rgb).all()
            path=state['referencePath']
            assert state['technique']=='path-tracing' and path['active'] and path['attemptedSamples']==13,path
            assert path['rejectedSamples']==0,path
            assert metadata['referencePath']['attemptedSamplesPerPixel']==13
            if profile=='solid-volume':
                assert path['unsupportedSamples']==320*240*13,path
                np.testing.assert_array_equal(rgb,0)
            else:
                assert path['unsupportedSamples']==0,path
            samples.append((rgb,state))
            report['captures']+=1
        report['runs'].append(dict(profile=profile,log=str(log),frames=list(frames),referencePaths=[s[1]['referencePath'] for s in samples]))
        return samples

    try:
        smooth=capture('smooth',(4,8),[dict(frame=5,reloadShaders=True)])
        np.testing.assert_allclose(smooth[0][0],np.broadcast_to([1.6,.6,.2],smooth[0][0].shape),atol=5e-7,rtol=0)
        np.testing.assert_array_equal(smooth[0][0],smooth[1][0])
        assert smooth[1][1]['shaderReload']['generation']==1
        assert [s[1]['referencePath']['historyResets'] for s in smooth]==[1,2]
        matched=capture('ior-one')[0]
        np.testing.assert_array_equal(matched[0],smooth[0][0])
        textured=capture('textured')[0]
        np.testing.assert_allclose(textured[0][80:160,120:200].mean(axis=(0,1)),np.array([4,2,1])*128/255,atol=.03,rtol=0)
        events=[dict(frame=5,materialEdit=dict(sourceMaterial=0,values=[dict(property='extensions/KHR_materials_transmission/transmissionFactor',value=0)])),
                dict(frame=9,sceneHistory='undo'),dict(frame=13,sceneHistory='redo')]
        history=capture('history',(4,8,12,16),events)
        np.testing.assert_array_equal(history[0][0],history[2][0])
        np.testing.assert_array_equal(history[1][0],history[3][0])
        np.testing.assert_array_equal(history[1][0],0)
        assert [s[1]['referencePath']['historyResets'] for s in history]==[1,2,3,4]
        capture('solid-volume')
        assert hashlib.sha256(renderer.read_bytes()).hexdigest()==executable_hash
        report['passed']=True
    finally:
        (output/'results.json').write_text(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
