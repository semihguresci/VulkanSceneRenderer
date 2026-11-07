"""Opt-in application vector previews and generated smoke sequence playback.

Retains commands, validation logs, PNGs and results.json. Uses independent image
measurements and static-file comparisons, not implementation-generated pixels.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import traceback
import uuid

WIDTH, HEIGHT = 320, 200
ERROR = re.compile(r"VUID-|Validation Error|SYNC-HAZARD", re.I)

def pixels(image):
    getter=getattr(image,"get_flattened_data",None)
    return list(getter() if getter else image.getdata())


def difference(first, second):
    from PIL import Image, ImageChops
    with Image.open(first) as a, Image.open(second) as b:
        delta = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
        values = pixels(delta)
        return {"meanAbsoluteError": sum(sum(p) for p in values)/(3*len(values)),
                "maximumChannelError": max(max(p) for p in values)}


def shape(empty, capture):
    from PIL import Image, ImageChops
    from statistics import median
    with Image.open(empty) as a, Image.open(capture) as b:
        delta = pixels(ImageChops.difference(a.convert("RGB"), b.convert("RGB")))
        actual = pixels(b.convert("RGB"))
    indices = [i for i, p in enumerate(delta) if max(p) > 16]
    if len(indices) < 20:
        raise AssertionError("field preview is not visibly different from the empty scene")
    xs, ys = [i % WIDTH for i in indices], [i // WIDTH for i in indices]
    return {"bounds": [min(xs), min(ys), max(xs)+1, max(ys)+1], "pixels": len(indices),
            "width": max(xs)-min(xs)+1, "height": max(ys)-min(ys)+1,
            "medianRedMinusGreen": median(actual[i][0]-actual[i][1] for i in indices)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe",type=Path,required=True)
    parser.add_argument("--tool",type=Path,required=True)
    parser.add_argument("--output-dir",type=Path,required=True)
    args=parser.parse_args()
    if os.environ.get("CONTAINER_RUN_OPENVDB_REGRESSION") != "1":
        print("SKIP: set CONTAINER_RUN_OPENVDB_REGRESSION=1"); return 77
    try:
        from PIL import Image
    except ImportError:
        print("SKIP: Pillow is required"); return 77
    exe, tool=args.exe.resolve(),args.tool.resolve()
    output=args.output_dir.resolve()/f"run-{datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    output.mkdir(parents=True,exist_ok=False)
    (output/"vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n",encoding="utf-8")
    environment=dict(os.environ,VK_LAYER_SETTINGS_PATH=str(output))
    report={"schemaVersion":1,"passed":False,"outputDirectory":str(output),
            "commands":[],"captures":{},"comparisons":[],"errors":[]}
    started=time.monotonic()
    def save():
        report["elapsedSeconds"]=round(time.monotonic()-started,3)
        (output/"results.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    def run(name,command):
        remaining=420-(time.monotonic()-started)
        if remaining<=0: raise TimeoutError("vector/sequence regression exceeded 420 seconds")
        log=output/(name+".log")
        record={"name":name,"argv":list(map(str,command)),"log":str(log),"cwd":str(exe.parent)}
        report["commands"].append(record); save()
        begin=time.monotonic()
        try:
            with log.open("w",encoding="utf-8") as stream:
                process=subprocess.run(record["argv"],cwd=exe.parent,env=environment,
                                       stdout=stream,stderr=subprocess.STDOUT,timeout=min(90,remaining))
            record["returnCode"]=process.returncode
        finally:
            record["elapsedSeconds"]=round(time.monotonic()-begin,3); save()
        text=log.read_text(encoding="utf-8",errors="replace")
        record["validationErrors"]=ERROR.findall(text); save()
        if record["validationErrors"] or process.returncode:
            raise AssertionError(f"{name} failed or reported Vulkan validation errors; see {log}")
        return text
    def capture(name,*,model=None,extra=(),display="albedo"):
        png=output/(name+".png")
        command=[exe,"--model",model if model else "","--hidden","--no-ui","--validation",
                 "--width",WIDTH,"--height",HEIGHT,"--camera-position",0,0,4,"--camera-target",0,0,0,
                 "--camera-fov",45,"--render-technique","deferred-raster","--display-mode",display,
                 "--no-bloom","--no-taa","--no-ray-query","--exposure",1,"--fixed-dt",.016666667,
                 "--warmup-frames",3,"--capture-frame",4,"--screenshot",png,*extra]
        run(name,command)
        with Image.open(png) as image:
            if image.size != (WIDTH,HEIGHT): raise AssertionError("incorrect capture dimensions")
        report["captures"][name]=str(png); save(); return png
    try:
        positive,negative=output/"positive.vdb",output/"negative.vdb"
        run("positive-wind",[tool,"--velocity",positive,"--vector",1,0,0])
        run("negative-wind",[tool,"--velocity",negative,"--vector",-1,0,0])
        empty=capture("empty")
        base=["--vdb-model-grid","velocity","--vdb-vector-bounds",-1,-1,-1,1,1,1,
              "--vdb-vector-seeds",1,1,1,"--vdb-preview-radius",.04,"--vdb-preview-color","direction"]
        pos=capture("positive-glyph",model=positive,extra=[*base,"--vdb-vector-mode","glyphs","--vdb-vector-seconds",.8])
        neg=capture("negative-glyph",model=negative,extra=[*base,"--vdb-vector-mode","glyphs","--vdb-vector-seconds",.8])
        p,n=shape(empty,pos),shape(empty,neg)
        expected=.8*HEIGHT/(2*math.tan(math.pi/8))/4
        for label,metrics in (("positive",p),("negative",n)):
            if abs(metrics["width"]-expected)>8 or metrics["height"]>22:
                raise AssertionError(f"{label} glyph does not preserve its world displacement/width: {metrics}")
        if p["bounds"][0]<157 or n["bounds"][2]>163 or p["medianRedMinusGreen"]<15 or n["medianRedMinusGreen"]>-15:
            raise AssertionError(f"glyph world sign or signed velocity color is incorrect: {p}, {n}")
        line=capture("constant-streamline",model=positive,extra=[*base,"--vdb-vector-mode","streamlines",
                                                               "--vdb-vector-step",.05,"--vdb-vector-steps",32])
        l=shape(empty,line)
        if not 112<=l["width"]<=130 or l["height"]>12:
            raise AssertionError(f"constant streamline does not span the requested world bounds: {l}")
        report["comparisons"] += [{"name":"positive-glyph",**p},{"name":"negative-glyph",**n},{"name":"constant-streamline",**l}]
        sdf,fog,initial=output/"sphere.vdb",output/"fog.vdb",output/"initial.vdb"
        zero=output/"zero.vdb"; frames=output/"frames"
        run("sphere",[tool,"--sphere",sdf,"--radius",.6,"--voxel-size",.1])
        run("fog",[tool,"--fog",sdf,fog,"--grid-name","density"])
        run("zero-wind",[tool,"--velocity",zero,"--vector",0,0,0])
        run("initial-mac",[tool,"--project",zero,fog,initial,"--domain-grid","density"])
        sequence=json.loads(run("smoke-sequence",[tool,"--simulate-sequence",fog,initial,initial,frames,
            "--input-grid","density","--velocity-grid","velocity","--domain-grid","fluid",
            "--frames",3,"--time",.2,"--steps",2,"--buoyancy-density",.8,"--max-iterations",10000]))
        if len(sequence["frames"])!=3 or not all(frame["converged"] for frame in sequence["frames"]):
            raise AssertionError("smoke sequence did not produce all three converged states")
        report["sequence"]=sequence
        static=[]
        for index in (0,2):
            source=Path(sequence["frames"][index]["path"])
            medium=["--vdb-grid","density","--vdb-emission-scale",2,"--vdb-emission",1,.5,.1]
            a=capture(f"static-{index}",display="lit",extra=["--vdb-volume",source,*medium])
            b=capture(f"sequence-{index}",display="lit",extra=["--vdb-sequence",frames,"--vdb-fps",5,"--vdb-paused",
                "--vdb-start-frame",index,*medium])
            shape(empty,a) # A parity check must not pass because both images are empty.
            parity=difference(a,b)
            if parity["maximumChannelError"]!=0:
                raise AssertionError(f"generated state and paused sequence frame {index} differ: {parity}")
            report["comparisons"].append({"name":f"static-sequence-{index}",**parity}); static.append(a)
        motion=difference(*static)
        if motion["maximumChannelError"]<1:
            raise AssertionError("buoyant generated sequence has no visible density evolution")
        report["comparisons"].append({"name":"buoyant-density-evolution",**motion})
        mac=capture("native-mac-glyphs",model=sequence["frames"][-1]["path"],extra=[
            "--vdb-model-grid","velocity","--vdb-vector-mode","glyphs","--vdb-vector-domain","fluid",
            "--vdb-vector-bounds",-.45,-.45,-.1,.45,.45,.1,"--vdb-vector-seeds",4,4,1,
            "--vdb-vector-seconds",30,"--vdb-vector-length",.35,"--vdb-preview-radius",.015])
        report["comparisons"].append({"name":"native-mac-visible",**shape(empty,mac)})
        report["passed"]=True
    except Exception as error:
        report["errors"].append({"type":type(error).__name__,"message":str(error),"traceback":traceback.format_exc()})
        print(f"FAIL: {error}",file=sys.stderr)
    finally:
        save(); print(f"OpenVDB vector/sequence results: {output/'results.json'}",flush=True)
    return 0 if report["passed"] else 1

if __name__=="__main__":
    sys.exit(main())
