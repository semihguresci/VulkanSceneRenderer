"""Validate reference EXR identity and independence from display grading."""
from pathlib import Path
import argparse,hashlib,json,os,re,subprocess,sys
os.environ.setdefault('OPENCV_IO_ENABLE_OPENEXR','1')
import cv2
import numpy as np
from display_capture_test_support import emitter_fixture, validation_log_error

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('renderer',type=Path);parser.add_argument('output',type=Path)
args=parser.parse_args();exe=args.renderer.resolve();root=args.output.resolve();root.mkdir(parents=True,exist_ok=False)
(root/'vk_layer_settings.txt').write_text('khronos_validation.validate_sync = true\n',encoding='utf-8')
model=root/'emitter.gltf'
material={'pbrMetallicRoughness':{'baseColorFactor':[0,0,0,1]},'emissiveFactor':[1,.5,.25],
 'extensions':{'KHR_materials_emissive_strength':{'emissiveStrength':4}},'doubleSided':True}
emitter_fixture(model, material)
digest=hashlib.sha256(exe.read_bytes()).hexdigest();records=[];images=[]
for name,controls in [('aces',['--exposure','.25','--tone-map','aces']),
 ('graded',['--exposure','4','--tone-map','agx','--saturation','0','--contrast','1.6','--white-balance','3000','--vibrance','.8'])]:
 image=root/(name+'.exr');log=root/(name+'.log')
 command=[str(exe),'--model',str(model),'--hidden','--no-ui','--validation','--width','640','--height','360','--msaa','1','--no-taa','--no-bloom','--render-technique','path-tracing','--display-mode','lit','--path-spp','16','--path-samples','128','--path-bounces','8','--path-seed','1','--environment-intensity','0','--directional-intensity','0','--camera-position','0','0','4','--camera-target','0','0','0','--camera-fov','45','--warmup-frames','8','--capture-frame','9','--screenshot',str(image),*controls]
 with log.open('w',encoding='utf-8') as stream:subprocess.run(command,cwd=exe.parent,stdout=stream,stderr=subprocess.STDOUT,check=True,timeout=180,env=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(root)))
 assert not validation_log_error(log.read_text(encoding='utf-8',errors='replace'))
 metadata=json.loads(Path(str(image)+'.capture.json').read_text(encoding='utf-8'))
 assert metadata['source']=='path-trace-mean',metadata
 assert metadata['colorSpace']=='linear-sRGB' and not metadata['includesUi']
 assert metadata['exposureApplied'] is False and metadata['toneMappingApplied'] is False
 reference=metadata['referencePath'];assert reference['attemptedSamplesPerPixel']==128 and not reference['hasDiagnostics']
 assert reference['unsupportedSamples']==reference['rejectedSamples']==0
 data=cv2.imdecode(np.frombuffer(image.read_bytes(),dtype=np.uint8),cv2.IMREAD_UNCHANGED)
 assert data is not None and data.shape==(360,640,3) and data.dtype==np.float32
 rgb=data[:,:,::-1];assert np.isfinite(rgb).all()
 np.testing.assert_allclose(rgb[180,320],[4,2,1],atol=.008,rtol=0)
 images.append(rgb);records.append({'name':name,'command':command,'metadata':metadata,'image':str(image),'log':str(log)})
assert np.array_equal(*images),'Display controls changed reference radiance'
assert hashlib.sha256(exe.read_bytes()).hexdigest()==digest
result={'passed':True,'rendererSha256':digest,'captures':2,'processLogs':2,'sceneLinearSource':'path-trace-mean','displayGradeIndependent':True,'records':records}
(root/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print('PASS reference EXR identity, 128 samples, analytic emission and display independence',flush=True)
