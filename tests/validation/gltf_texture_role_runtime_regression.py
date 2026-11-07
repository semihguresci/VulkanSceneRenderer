"""Render a shared color/normal texture against independent role references."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
os.environ.setdefault('OPENCV_IO_ENABLE_OPENEXR','1')
import cv2
import numpy as np
from PIL import Image
from display_capture_test_support import validation_log_error
from texture_upload_runtime_regression import model,linear
from gltf_texture_formats_runtime_regression import ktx2_bytes

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('renderer',type=Path);parser.add_argument('output',type=Path)
    parser.add_argument('--parent-renderer',type=Path)
    args=parser.parse_args();exe=args.renderer.resolve();root=args.output.resolve()
    root.mkdir(parents=True,exist_ok=False);digest=hashlib.sha256(exe.read_bytes()).hexdigest()
    (root/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n',encoding='utf-8')
    image=root/'shared.png';Image.new('RGBA',(1,1),(128,128,255,255)).save(image)
    encoded=np.array([128/255,128/255,1],dtype=np.float32)
    color=[linear(float(v)) for v in encoded]
    for name,normal in [('normal-reference',encoded),('normal-wrong-gamma',np.asarray(color,dtype=np.float32))]:
        pixels=np.array([[[*normal,1]]],dtype=np.float32)
        (root/(name+'.ktx2')).write_bytes(ktx2_bytes([pixels],floating=True,data_role=True))
    def asset(name,emission,normal=None,emissive_texture=None):
        path=root/(name+'.gltf');model(path,emission,emissive_texture)
        document=json.loads(path.read_text(encoding='utf-8'))
        if normal:
            if not emissive_texture:
                document['images']=[dict(uri=normal.name)];document['textures']=[dict(source=0)]
            document['materials'][0]['normalTexture']=dict(index=0)
        path.write_text(json.dumps(document),encoding='utf-8');return path
    source=asset('source',[1,1,1],image,image)
    reference=asset('color-reference',color)
    wrong=asset('color-wrong-data',encoded.tolist())
    normal_reference=asset('normal-reference',color,root/'normal-reference.ktx2')
    normal_wrong=asset('normal-wrong-gamma',color,root/'normal-wrong-gamma.ktx2')
    records=[];checks=[]
    for technique,mode,assets in [('deferred-raster','lit',[source,reference,wrong]),
            ('forward-raster','lit',[source,reference,wrong]),
            ('deferred-raster','normals',[source,normal_reference,normal_wrong])]:
        results=[]
        for path in assets:
            suffix='png' if mode=='normals' else 'exr'
            output=root/f'{technique}-{mode}-{path.stem}.{suffix}';log=output.with_suffix('.log')
            command=[str(exe),'--model',str(path),'--hidden','--no-ui','--validation','--width','320','--height','240',
                '--msaa','1','--no-taa','--no-bloom','--environment-intensity','0','--directional-intensity','0',
                '--exposure','1','--camera-position','0','0','3','--camera-target','0','0','0','--camera-fov','60',
                '--warmup-frames','3','--capture-frame','4','--render-technique',technique,'--display-mode',mode,
                '--screenshot',str(output)]
            with log.open('w',encoding='utf-8') as stream:
                subprocess.run(command,cwd=exe.parent,check=True,stdout=stream,stderr=subprocess.STDOUT,timeout=120,
                    env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(root)),creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            assert not validation_log_error(log.read_text(encoding='utf-8',errors='replace')),log
            pixels=cv2.imread(str(output),cv2.IMREAD_UNCHANGED)
            assert pixels is not None and pixels.shape[:2]==(240,320) and np.isfinite(pixels).all(),output
            pixels=pixels[:,:,:3][:,:,::-1]
            if mode=='normals':pixels=pixels.astype(np.float32)/255
            results.append(pixels)
            records.append(dict(image=str(output),log=str(log),command=command))
        roi=(slice(90,150),slice(130,190))
        error=float(np.max(np.abs(results[0][roi]-results[1][roi])))
        negative=float(np.max(np.abs(results[0][roi]-results[2][roi])))
        assert error<=(2/255 if mode=='normals' else .001),f'Wrong {mode} shared role: {error}'
        assert negative>=.15,f'Vacuous {mode} role reference: {negative}'
        checks.append(dict(technique=technique,mode=mode,maximumReferenceError=error,
            wrongRoleDifference=negative,centerRgb=results[0][120,160].tolist()))
        print('PASS',technique,mode,checks[-1],flush=True)
    parent=None
    if args.parent_renderer:
        old=args.parent_renderer.resolve();old_hash=hashlib.sha256(old.read_bytes()).hexdigest()
        parent_root=root/'parent';parent_root.mkdir();parent_records=[];parent_images=[]
        original=next(r for r in records if Path(r['image']).name=='deferred-raster-lit-source.exr')
        for path in [source,reference]:
            output=parent_root/(path.stem+'.exr');log=output.with_suffix('.log');command=original['command'].copy()
            command[0]=str(old);command[command.index('--model')+1]=str(path)
            command[command.index('--screenshot')+1]=str(output)
            with log.open('w',encoding='utf-8') as stream:
                subprocess.run(command,cwd=old.parent,check=True,stdout=stream,stderr=subprocess.STDOUT,timeout=120,
                    env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(root)),creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            assert not validation_log_error(log.read_text(encoding='utf-8',errors='replace')),log
            pixels=cv2.imread(str(output),cv2.IMREAD_UNCHANGED)
            assert pixels is not None and np.isfinite(pixels).all(),output
            pixels=pixels[:,:,:3][:,:,::-1]
            parent_images.append(pixels);parent_records.append(dict(image=str(output),log=str(log),command=command))
        error=float(np.max(np.abs(parent_images[0][roi]-parent_images[1][roi])))
        assert error>=.15,f'Parent did not reproduce the shared-role emission error: {error}'
        assert hashlib.sha256(old.read_bytes()).hexdigest()==old_hash
        parent=dict(rendererSha256=old_hash,captures=2,sharedRoleColorError=error,records=parent_records)
        print('PASS actual parent shared-role emission reproduction',error,flush=True)
    assert hashlib.sha256(exe.read_bytes()).hexdigest()==digest
    (root/'results.json').write_text(json.dumps(dict(passed=True,rendererSha256=digest,captures=9,
        checks=checks,records=records,parent=parent),indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':main()
