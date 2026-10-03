"""Opt-in GPU sequences and linear-light temporal metrics. Python + Pillow + NumPy."""
import argparse
import json
import os
import pathlib
import statistics
import shutil
import subprocess
import sys
import unittest

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("SKIP: temporal metrics require Pillow and NumPy")
    sys.exit(77)

ROOT = pathlib.Path(__file__).resolve().parents[2]


def linear(image):
    value = np.asarray(image.convert("RGB"), dtype=np.float32) / 255
    return np.where(value <= .04045, value / 12.92, ((value + .055) / 1.055) ** 2.4)


def metrics(frames, references):
    """Residual flicker removes intended motion; spatial errors measure ghost/detail loss."""
    frames, references = np.asarray(frames), np.asarray(references)
    if frames.shape != references.shape or len(frames) < 2:
        raise ValueError("Metrics need equal-sized sequences with at least two frames")
    error = frames - references
    dx = np.diff(references, axis=2)
    mask = np.max(np.abs(dx), axis=-1) > .015
    retained = np.sum(np.abs(np.diff(frames, axis=2))[mask]) / max(float(np.sum(np.abs(dx)[mask])), 1e-8)
    return {"residualFlickerRms":float(np.sqrt(np.mean(np.diff(error, axis=0) ** 2))),
            "referenceMae":float(np.mean(np.abs(error))),
            "errorP95":float(np.quantile(np.abs(error), .95)),
            "edgeEnergyRatio":float(retained),
            "ghostPersistenceMae":float(np.mean(np.abs(error[-min(4,len(error)):]))) }


class MetricChecks(unittest.TestCase):
    def test_detects_inverted_motion_stale_history_and_missed_reset(self):
        reference = np.zeros((8,32,64,3), dtype=np.float32)
        for frame in range(8): reference[frame,8:24,8+frame*3:12+frame*3,:] = 1
        correct = metrics(reference,reference)
        inverted = metrics(np.stack([np.roll(reference[f],-6,axis=1) for f in range(8)]),reference)
        stale = metrics(np.repeat(reference[:1],8,axis=0),reference)
        missed_reset = reference.copy(); missed_reset[4:] = .6*reference[3]+.4*reference[4:]
        reset = metrics(missed_reset,reference)
        for broken in [inverted,stale,reset]:
            self.assertGreater(broken["ghostPersistenceMae"],correct["ghostPersistenceMae"]+.01)
            self.assertGreater(broken["referenceMae"],correct["referenceMae"]+.01)
        self.assertGreater(inverted["residualFlickerRms"],.05)

    def test_detects_detail_loss_and_rejects_invalid_input(self):
        reference = np.zeros((4,32,32,3),dtype=np.float32); reference[:,:,::2,:] = 1
        self.assertLess(metrics(np.full_like(reference,.5),reference)["edgeEnergyRatio"],.1)
        with self.assertRaises(ValueError): metrics(reference[:1],reference[:1])


def run_capture(exe, output, name, sequence="taa_pan.json", model="taa_equivalent.gltf", enabled=True,
                technique="deferred-raster", width=640, height=360, display="lit", extra_args=()):
    output.mkdir(parents=True,exist_ok=True)
    command = [str(exe),"--hidden","--no-ui","--validation","--taa" if enabled else "--no-taa",
        "--msaa","1","--width",str(width),"--height",str(height),"--display-mode",display,
        "--render-technique",technique,"--model","models/validation/"+model,
        "--capture-sequence",str(exe.parent/"models/validation"/sequence),
        "--screenshot",str(output/(name+".png")),"--fixed-dt","0.016666667","--taa-jitter-seed","0",
        "--exposure","0.25","--environment-intensity","0.5","--directional-intensity","3","--no-bloom"]
    command.extend(extra_args)
    env = dict(os.environ, VK_LAYER_SETTINGS_PATH=str(output))
    (output/"vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n",encoding="utf-8")
    with (output/(name+".log")).open("w",encoding="utf-8") as log:
        result = subprocess.run(command,cwd=output,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
    logtext = (output/(name+".log")).read_text(encoding="utf-8",errors="replace")
    if result.returncode or "VUID-" in logtext or "SYNC-HAZARD" in logtext:
        raise RuntimeError(f"Capture {name} failed; inspect {output/(name+'.log')}")
    files = sorted(output.glob(name+".frame-*.png"))
    final = output/(name+".png")
    if final.exists(): files.append(final)
    if not files: raise RuntimeError("Capture produced no images: "+name)
    print(f"{name}: {len(files)} captures, synchronization validation clean",flush=True)
    return files


def images(files, scale=1):
    result = []
    for path in files:
        with Image.open(path) as image: value = linear(image)
        if scale > 1:
            h,w,c = value.shape
            value = value.reshape(h//scale,scale,w//scale,scale,c).mean(axis=(1,3))
        # Exclude unrelated sky and floor boundaries, retain thin edges/cube/wall.
        h,w,_ = value.shape
        result.append(value[int(.16*h):int(.74*h),int(.25*w):int(.75*w)])
    return np.stack(result)


def telemetry(files):
    samples = [json.loads(file.with_suffix(".telemetry.json").read_text()) for file in files]
    def median(values): return statistics.median(values) if values else 0
    names = sorted({p["name"] for s in samples for p in s.get("passes",[])})
    return {"gpu":samples[-1]["gpu"],"driverVersion":samples[-1]["driverVersion"],
        "resolution":samples[-1]["resolution"],"taa":samples[-1]["taa"],
        "medianGpuKnownMs":median([s.get("gpuKnownMs",0) for s in samples]),
        "medianCpuSubmitMs":median([s.get("cpuPhasesMs",{}).get("Queue submit",0) for s in samples]),
        "medianCpuSceneMs":median([s.get("cpuPhasesMs",{}).get("Scene update",0) for s in samples]),
        "medianPassGpuMs":{name:median([p["gpuMs"] for s in samples for p in s.get("passes",[]) if p["name"]==name and p["gpuTimed"]]) for name in names}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test",action="store_true")
    parser.add_argument("--exe",type=pathlib.Path)
    parser.add_argument("--output",type=pathlib.Path,default=ROOT/"out/taa-regression")
    parser.add_argument("--suite",choices=["quality","stress","performance","mutations","all"],default="all")
    parser.add_argument("--slangc",type=pathlib.Path,help="Required for deliberate GPU shader mutations")
    parser.add_argument("--analyze",action="store_true")
    args = parser.parse_args()
    if args.self_test:
        result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(MetricChecks))
        return 0 if result.wasSuccessful() else 1
    if os.environ.get("CONTAINER_RUN_TAA_REGRESSION") != "1":
        print("SKIP: set CONTAINER_RUN_TAA_REGRESSION=1 for GPU captures"); return 77
    if not args.exe: parser.error("--exe is required")
    exe,output = args.exe.resolve(),args.output.resolve()
    results = {"schemaVersion":1,"platform":"windows-nvidia","quality":{},"performance":{},"stress":{},"mutations":{}}
    def capture(name,**kwargs):
        if args.analyze:
            files = sorted(output.glob(name+".frame-*.png"))
            if (output/(name+".png")).exists(): files.append(output/(name+".png"))
            return files
        return run_capture(exe,output,name,**kwargs)
    if args.suite in ["all","quality"]:
        for technique in ["deferred-raster","forward-raster"]:
            prefix = technique.split("-")[0]
            off = capture(prefix+"-off",enabled=False,technique=technique)
            on = capture(prefix+"-on",technique=technique)
            ref = capture(prefix+"-reference",enabled=False,technique=technique,width=2560,height=1440)
            references = images(ref,4)
            a,b = metrics(images(off),references),metrics(images(on),references)
            quality = {"off":a,"on":b,"flickerRatio":b["residualFlickerRms"]/max(a["residualFlickerRms"],1e-8)}
            quality["passed"] = quality["flickerRatio"] < .95 and b["edgeEnergyRatio"] > .65 and b["referenceMae"] <= a["referenceMae"]*1.3+.002
            results["quality"][prefix] = quality
            results["performance"][prefix+"-640-off"] = telemetry(off)
            results["performance"][prefix+"-640-on"] = telemetry(on)
            object_on = capture(prefix+"-object",sequence="taa_object.json",model="taa_scene.gltf",technique=technique)
            object_off = capture(prefix+"-object-off",sequence="taa_object.json",model="taa_scene.gltf",technique=technique,enabled=False)
            object_ref = capture(prefix+"-object-reference",sequence="taa_object.json",model="taa_scene.gltf",technique=technique,enabled=False,width=2560,height=1440)
            moving = metrics(images(object_on),images(object_ref,4))
            moving_off = metrics(images(object_off),images(object_ref,4))
            quality["movingObject"] = {"on":moving,"off":moving_off,"passed":moving["ghostPersistenceMae"] <= moving_off["ghostPersistenceMae"]*1.5+.003}
            quality["passed"] = quality["passed"] and quality["movingObject"]["passed"]
            capture(prefix+"-transparency",sequence="taa_transparency.json",model="taa_scene.gltf",technique=technique)
            capture(prefix+"-emissive",sequence="taa_emissive.json",model="taa_scene.gltf",technique=technique)
            capture(prefix+"-reactive",sequence="taa_static.json",model="taa_scene.gltf",technique=technique,display="taa-reactive")
    if args.suite in ["all","stress"]:
        velocities = {}
        for model in ["taa_equivalent.gltf","taa_equivalent.bim","taa_equivalent.usda"]:
            suffix = model.split(".")[-1]
            files = capture("provider-"+suffix,model=model,sequence="taa_pan.json",display="taa-velocity")
            results["stress"][suffix] = {"captures":len(files),"validation":"passed"}
            sample = next(file for file in files if "frame-0032" in file.name)
            with Image.open(sample) as image: velocities[suffix] = linear(image)[120:140,320:340].mean(axis=(0,1))
            if suffix != "gltf":
                capture("provider-root-"+suffix,model=model,sequence="taa_provider_motion.json",display="taa-velocity")
                capture("provider-events-"+suffix,model=model,sequence="taa_provider_events.json",display="taa-rejection")
            capture("provider-forward-"+suffix,model=model,sequence="taa_pan.json",display="taa-velocity",technique="forward-raster")
        for technique in ["deferred-raster","forward-raster"]:
            prefix = technique.split("-")[0]
            capture(prefix+"-mixed-providers",model="taa_scene.gltf",sequence="taa_object.json",
                technique=technique,extra_args=["--bim-model","models/validation/taa_equivalent.bim"])
            coverage = capture(prefix+"-coverage",model="taa_coverage.gltf",sequence="taa_static.json",
                technique=technique,display="taa-velocity")
            with Image.open(coverage[-1]) as image:
                # This rectangle covers both cubes, cutouts and thin geometry,
                # staying inside the back-wall silhouette (sky is invalid).
                pixels = linear(image)[100:260,200:440]
                velocity = pixels.mean(axis=(0,1))
            if np.mean(np.max(np.abs(pixels-.5),axis=-1) < .01) < .99:
                raise RuntimeError("Static geometry under jitter reported physical motion")
            results["stress"][prefix+"-staticVelocity"] = velocity.tolist()
            reset_files = capture(prefix+"-reset",sequence="taa_reset.json",display="taa-rejection",technique=technique)
            with Image.open(reset_files[1]) as image:
                reset = linear(image)[100:140,280:350]
            fraction = float(np.mean(np.max(np.abs(reset-.25),axis=-1) < .015))
            if fraction < .95: raise RuntimeError("First frame after reset used old history")
            before = json.loads(reset_files[1].with_suffix(".telemetry.json").read_text())["taa"]["epoch"]
            after = json.loads(reset_files[2].with_suffix(".telemetry.json").read_text())["taa"]["epoch"]
            if before != after: raise RuntimeError("Display exposure unexpectedly reset scene-linear history")
            results["stress"][prefix+"-reset"] = {"resetGrayFraction":fraction,"exposureRetainsEpoch":True}
        life = capture("lifecycle",sequence="taa_lifecycle.json",model="taa_scene.gltf",display="taa-age")
        results["stress"]["lifecycle"] = {"captures":len(life),"telemetry":[json.loads(file.with_suffix(".telemetry.json").read_text()) for file in life]}
        differences = {key:float(np.max(np.abs(value-velocities["gltf"]))) for key,value in velocities.items()}
        results["stress"]["equivalentVelocity"] = {"linearRgb":{key:value.tolist() for key,value in velocities.items()},"maxDifference":differences}
        if any(value > .012 for value in differences.values()) or velocities["gltf"][0] <= velocities["gltf"][1]:
            raise RuntimeError("Equivalent providers disagree on signed camera velocity")
        final = results["stress"]["lifecycle"]["telemetry"][-1]
        if final["taa"]["submittedFrame"] != 45: raise RuntimeError("Skipped/acquire-failed frames consumed temporal history")
        for sample in results["stress"]["lifecycle"]["telemetry"]:
            enabled = sample["taa"]["enabled"]
            if any(pass_["active"] != enabled for pass_ in sample["passes"] if pass_["name"].startswith("Temporal")):
                raise RuntimeError("Temporal pass activation disagrees with effective AA mode")
    if args.suite in ["all","performance"]:
        for width,height in [(1280,720),(1920,1080)]:
            for enabled in [False,True]:
                name = f"performance-{width}-{'on' if enabled else 'off'}"
                files = capture(name,width=width,height=height,enabled=enabled,sequence="taa_static.json")
                results["performance"][name] = telemetry(files[2:])
    if args.suite == "mutations":
        compiler = args.slangc or (pathlib.Path(shutil.which("slangc")) if shutil.which("slangc") else None)
        if compiler is None: parser.error("--slangc is required for the mutation suite")
        baseline_files = sorted(output.glob("deferred-on.frame-*.png")) + [output/"deferred-on.png"]
        reference_files = sorted(output.glob("deferred-reference.frame-*.png")) + [output/"deferred-reference.png"]
        reference = images(reference_files,4); baseline = metrics(images(baseline_files),reference)
        mutations = [
            ("inverted-velocity","temporal_velocity.slang","fragMain","temporal_velocity.frag.spv",
             "float4(motion.uv,", "float4(-motion.uv,"),
            ("stale-history","temporal_resolve.slang","computeMain","temporal_resolve.comp.spv",
             "float2 historyUv = uv + motion.xy;", "float2 historyUv = uv;"),
            ("skipped-invalidation","temporal_resolve.slang","computeMain","temporal_resolve.comp.spv",
             "pc.historyValid == 0u || uCamera.temporalInfo.x == 0u", "false")]
        for name,source,entry,binary,before,after in mutations:
            runtime = exe.parent/"spv_shaders"/binary
            original = runtime.read_bytes()
            text = (ROOT/"shaders"/source).read_text(); assert before in text
            temporary = output/(name+".slang"); temporary.write_text(text.replace(before,after,1))
            try:
                subprocess.run([str(compiler),str(temporary),"-target","spirv","-profile","spirv_1_6",
                    "-emit-spirv-directly","-fvk-use-entrypoint-name","-matrix-layout-column-major",
                    "-I",str(ROOT/"shaders"),"-entry",entry,"-o",str(runtime)],check=True,capture_output=True)
                if name == "skipped-invalidation":
                    files = capture("mutation-"+name,sequence="taa_reset.json",display="taa-rejection")
                    with Image.open(files[1]) as image: reset = linear(image)[100:140,280:350]
                    fraction = float(np.mean(np.max(np.abs(reset-.25),axis=-1) < .015))
                    results["mutations"][name] = {"expectedResetGrayFraction":fraction,"detected":fraction < .95}
                else:
                    files = capture("mutation-"+name)
                    broken = metrics(images(files),reference)
                    detected = broken["referenceMae"] > baseline["referenceMae"]*1.05 or broken["residualFlickerRms"] > baseline["residualFlickerRms"]*1.05
                    results["mutations"][name] = {"baseline":baseline,"mutated":broken,"detected":detected}
            finally:
                runtime.write_bytes(original)
    output.mkdir(parents=True,exist_ok=True)
    (output/(args.suite+"-results.json")).write_text(json.dumps(results,indent=2)+"\n",encoding="utf-8")
    if any(not value["passed"] for value in results["quality"].values()):
        print("Temporal quality budget failed; inspect results and captures"); return 1
    if any(not value["detected"] for value in results["mutations"].values()):
        print("Deliberate shader regression was not detected"); return 1
    return 0


if __name__ == "__main__": sys.exit(main())
