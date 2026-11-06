"""Opt-in physical image tool proof; unique output, independent EXR readback.

python openvdb_transport_image_cli_tests.py --exe OpenVdbTransportRender.exe
Add --gpu for the dedicated validated compute session, or --expect-disabled.
"""
from __future__ import annotations
import argparse
import datetime as dt
import json
import math
import pathlib
import struct
import subprocess
import uuid


def exr(path: pathlib.Path):
    data = path.read_bytes()
    assert len(data) < 64 * 1024 * 1024
    assert struct.unpack_from("<II", data) == (20000630, 2)
    cursor = 8
    def text():
        nonlocal cursor
        end = data.index(b"\0", cursor)
        value = data[cursor:end].decode("ascii")
        cursor = end + 1
        return value
    attributes = {}
    while data[cursor] != 0:
        name, kind = text(), text()
        size, = struct.unpack_from("<I", data, cursor)
        cursor += 4
        attributes[name] = (kind, data[cursor:cursor + size])
        cursor += size
    cursor += 1
    assert attributes["compression"][1] == b"\0"
    xmin, ymin, xmax, ymax = struct.unpack("<iiii", attributes["dataWindow"][1])
    assert xmin == ymin == 0
    width, height = xmax + 1, ymax + 1
    channels = []
    channel_data = attributes["channels"][1]
    i = 0
    while channel_data[i] != 0:
        end = channel_data.index(b"\0", i)
        name = channel_data[i:end].decode("ascii")
        kind, linear, xs, ys = struct.unpack_from("<iB3xii", channel_data, end + 1)
        assert kind == 2 and xs == ys == 1 and linear in (0, 1)
        channels.append(name)
        i = end + 17
    assert channels == ["B", "G", "R", "SE.B", "SE.G", "SE.R"]
    pixels = {name: [0.] * (width * height) for name in channels}
    for row in range(height):
        offset, = struct.unpack_from("<Q", data, cursor + row * 8)
        y, size = struct.unpack_from("<iI", data, offset)
        assert 0 <= y < height and size == width * 4 * 6
        values = struct.unpack_from("<" + "f" * width * 6, data, offset + 8)
        for c, name in enumerate(channels):
            pixels[name][y * width:(y + 1) * width] = values[c * width:(c + 1) * width]
    assert all(math.isfinite(v) and v >= 0 for values in pixels.values() for v in values)
    return width, height, pixels, attributes["container.transport"][1].decode("ascii")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--output-root", type=pathlib.Path, default=pathlib.Path("out/openvdb-validation/transport-images"))
    parser.add_argument("--gpu", action="store_true")
    parser.add_argument("--expect-disabled", action="store_true")
    args = parser.parse_args()
    run = args.output_root / ("run-" + dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:10])
    run.mkdir(parents=True, exist_ok=False)
    results = []
    exe = args.exe.resolve()
    def command(name, scene, expected=0, backend="cpu", extra=(), unicode_paths=False):
        suffix = "-ışık-场景" if unicode_paths else ""
        input_path, output = (run / (name + suffix + ".json")).resolve(), (run / (name + suffix + ".exr")).resolve()
        input_path.write_text(scene if isinstance(scene, str) else json.dumps(scene), encoding="utf-8")
        original = input_path.read_bytes()
        argv = [str(exe), "--scene", str(input_path), "--output", str(output), "--backend", backend, "--samples", "16", *extra]
        if backend == "gpu":
            argv.append("--validation")
        completed = subprocess.run(argv, text=True, encoding="utf-8", errors="replace", capture_output=True, timeout=120)
        (run / (name + ".log")).write_text(completed.stdout + completed.stderr, encoding="utf-8")
        assert input_path.read_bytes() == original
        assert completed.returncode == expected, (name, completed.stdout, completed.stderr)
        assert "VUID-" not in completed.stderr and "Validation Error" not in completed.stderr
        if expected:
            assert not output.exists(), name
        else:
            result = json.loads(completed.stdout.strip().splitlines()[-1])
            assert result["output"] == str(output) and result["encodedBytes"] == output.stat().st_size
            assert result["paths"] == result["width"] * result["height"] * result["samplesPerPixel"]
            exr(output)
        results.append({"name": name, "returnCode": completed.returncode})
        return output, completed
    vacuum = {"schemaVersion": 1, "camera": {"width": 2, "height": 2, "eye": [0, 0, 0], "target": [0, 0, -1], "verticalFovDegrees": 90}, "environment": [.25, .5, 1]}
    if args.expect_disabled:
        command("off-vacuum", vacuum)
        _, result = command("disabled", {"schemaVersion": 1, "media": [{"input": "missing.vdb"}]}, 1)
        assert "ENABLE_OPENVDB=ON" in result.stderr
    else:
        output, _ = command("vacuum", vacuum)
        command("unicode", vacuum, unicode_paths=True)
        width, height, planes, metadata = exr(output)
        assert (width, height) == (2, 2)
        for channel, value in (("R", .25), ("G", .5), ("B", 1)):
            assert planes[channel] == [value] * 4
            assert planes["SE." + channel] == [0.] * 4
        assert "uncertainty_available=1" in metadata
        protected = output.read_bytes()
        result = subprocess.run([str(exe), "--scene", str(run / "vacuum.json"), "--output", str(output)], capture_output=True, text=True, timeout=120)
        assert result.returncode == 1 and output.read_bytes() == protected
        black = dict(vacuum)
        black["occluders"] = [[[-.7, .3, -1], [-.3, .3, -1], [-.3, .7, -1]], [[-.7, .3, -1], [-.3, .7, -1], [-.7, .7, -1]]]
        output, _ = command("black", black)
        assert exr(output)[2]["R"] == [0., .25, .25, .25]
        command("underflow", '{"schemaVersion":1,"environment":[1e-999,0,0]}', 1)
        command("duplicate", '{"schemaVersion":1,"schemaVersion":1}', 1)
        command("unknown", {"schemaVersion": 1, "unknown": 1}, 1)
        command("overflow", '{"schemaVersion":1,"environment":[1e999,0,0]}', 1)
        command("work", vacuum, 1, extra=("--work", "1"))
        tiny = {"schemaVersion": 1, "environment": [1e-200, 0, 0]}
        _, tiny_result = command("export-underflow", tiny, 1)
        assert "overflow/underflow" in tiny_result.stderr
        if args.gpu:
            gpu, _ = command("gpu-vacuum", vacuum, backend="gpu")
            assert exr(gpu)[2]["R"] == [.25] * 4
            gpu, _ = command("gpu-black", black, backend="gpu")
            assert exr(gpu)[2]["R"] == [0., .25, .25, .25]
    (run / "results.json").write_text(json.dumps({"passed": True, "cases": results}, indent=2), encoding="utf-8")
    print(json.dumps({"passed": True, "cases": len(results), "output": str(run)}))


if __name__ == "__main__":
    main()
