"""Compare external PNG16 emission with analytic material-only references."""
import argparse,hashlib,json,os,struct,subprocess,zlib
from pathlib import Path
os.environ.setdefault('OPENCV_IO_ENABLE_OPENEXR','1')
import cv2
import numpy as np
from display_capture_test_support import Asset,validation_log_error

def png16(path,rgba):
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    row=struct.pack('>4H',*rgba)*2
    raw=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,2,16,6,0,0,0))
    path.write_bytes(raw+chunk(b'IDAT',zlib.compress((b'\0'+row)*2))+chunk(b'IEND',b''))

def linear(value):return value/12.92 if value<=.04045 else ((value+.055)/1.055)**2.4

def model(path,color,texture=None):
    asset=Asset()
    material=dict(doubleSided=True,pbrMetallicRoughness=dict(baseColorFactor=[0,0,0,1],metallicFactor=0),emissiveFactor=color)
    if texture:
        material['emissiveTexture']=dict(index=0)
        asset.model['images']=[dict(uri=texture.name)]
        asset.model['textures']=[dict(source=0)]
    positions=[[-1,-.8,0],[1,-.8,0],[1,.8,0],[-1,.8,0]]
    asset.model['materials']=[material]
    asset.model['meshes']=[dict(primitives=[dict(attributes=dict(
        POSITION=asset.accessor('VEC3',positions),NORMAL=asset.accessor('VEC3',[[0,0,1]]*4),
        TEXCOORD_0=asset.accessor('VEC2',[[.5,.5]]*4)),
        indices=asset.accessor('SCALAR',[0,1,2,0,2,3],5123,indices=True),material=0)])]
    asset.write(path)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('renderer',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();exe=args.renderer.resolve();root=args.output.resolve();root.mkdir(parents=True,exist_ok=False)
    digest=hashlib.sha256(exe.read_bytes()).hexdigest()
    (root/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n',encoding='utf-8')
    rgba=[12345,23456,34567,65535];texture=root/'emission16.png';png16(texture,rgba)
    reference=[linear(value/65535) for value in rgba[:3]]
    truncated=[linear((value>>8)/255) for value in rgba[:3]]
    model(root/'source.gltf',[1,1,1],texture)
    model(root/'reference.gltf',reference)
    model(root/'truncated8.gltf',truncated)
    records=[];checks=[]
    for technique in ['deferred-raster','forward-raster']:
        images={}
        for label in ['source','reference','truncated8']:
            image=root/(technique+'-'+label+'.exr');log=image.with_suffix('.log')
            command=[str(exe),'--hidden','--no-ui','--validation','--no-taa','--no-bloom','--width','320','--height','240','--msaa','1',
                '--render-technique',technique,'--display-mode','lit','--environment-intensity','0','--directional-intensity','0',
                '--no-ray-query','--ray-shadows','raster','--camera-position','0','0','4','--camera-target','0','0','0','--camera-fov','45',
                '--import-scale','1','--warmup-frames','3','--capture-frame','4','--model',str(root/(label+'.gltf')),'--screenshot',str(image)]
            with log.open('w',encoding='utf-8') as stream:
                subprocess.run(command,cwd=exe.parent,stdout=stream,stderr=subprocess.STDOUT,check=True,timeout=120,
                    env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(root)))
            assert not validation_log_error(log.read_text(encoding='utf-8',errors='replace')),log
            metadata=json.loads(Path(str(image)+'.capture.json').read_text(encoding='utf-8'))
            assert metadata['source']=='raster-scene-composite' and metadata['colorSpace']=='linear-sRGB' and not metadata['includesUi']
            data=cv2.imdecode(np.frombuffer(image.read_bytes(),dtype=np.uint8),cv2.IMREAD_UNCHANGED)
            assert data.dtype==np.float32 and data.shape==(240,320,3) and np.isfinite(data).all()
            images[label]=data[:,:,::-1]
            records.append(dict(technique=technique,label=label,image=str(image),log=str(log),metadata=metadata,command=command))
        # Independent constant-emission material passes through the same render
        # targets, so it includes their storage rounding without baking a golden.
        np.testing.assert_array_equal(images['source'],images['reference'])
        # An explicit eight-bit truncation reference proves this test distinguishes
        # the old upload path instead of accepting any plausible-looking image.
        delta=np.abs(images['source'][100:140,140:180]-images['truncated8'][100:140,140:180])
        assert float(delta.max())>1/4096,'Precision witness did not distinguish the old eight-bit path'
        checks.append(dict(technique=technique,analyticReferencePixelExact=True,truncated8MaxDifference=float(delta.max()),centerRgb=images['source'][120,160].tolist()))
        print('PASS',technique,checks[-1],flush=True)
    assert hashlib.sha256(exe.read_bytes()).hexdigest()==digest
    (root/'results.json').write_text(json.dumps(dict(passed=True,rendererSha256=digest,captures=6,processLogs=6,
        sourceRgba16=rgba,analyticLinearRgb=reference,checks=checks,records=records),indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':main()
