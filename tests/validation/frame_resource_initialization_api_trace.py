"""Count Vulkan submissions and completion waits for frame image initialization."""

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


from display_capture_test_support import validation_log_error


HARNESS = Path(__file__).with_name("frame_resource_initialization_runtime_regression.py").resolve()
SETTINGS = (
    "khronos_validation.validate_sync = true\n"
    "lunarg_api_dump.detailed = true\n"
    "lunarg_api_dump.file = true\n"
    "lunarg_api_dump.output_format = text\n"
    "lunarg_api_dump.flush = false\n"
    "lunarg_api_dump.no_addr = false\n"
    "lunarg_api_dump.log_filename = {filename}\n"
)
INSERTION = re.compile(r'Insert(?:ed)? (instance|device) layer "([^"]+)" \(([^\r\n]+)\)')


CALL = re.compile(r"(?m)^Thread (\d+), Frame (\d+):\r?\n(vk\w+)\([^\r\n)]*\)([^\r\n]*)\r?\n")
TRACE_PHASES = {0: "startup", 3: "resize", 5: "msaa-4x", 7: "msaa-1x"}
DEPTH_STENCIL = "VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT"
ADDRESS = r"(?:0x)?[0-9a-fA-F]{8,16}"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def layer_paths(validation, api_dump):
    layers = {}
    for name, folder in (("VK_LAYER_KHRONOS_validation", validation), ("VK_LAYER_LUNARG_api_dump", api_dump)):
        stem = name.replace("VK_LAYER_KHRONOS_validation", "VkLayer_khronos_validation").replace(
            "VK_LAYER_LUNARG_api_dump", "VkLayer_api_dump")
        manifest = folder / f"{stem}.json"
        spec = json.loads(manifest.read_text(encoding="utf-8-sig"))["layer"]
        library = (folder / spec["library_path"]).resolve(strict=True)
        require(spec["name"] == name, f"Wrong layer manifest: {manifest}")
        layers[name] = {"library": library, "apiVersion": spec["api_version"],
                        "manifestSha256": sha(manifest), "librarySha256": sha(library)}
    return layers


def params(block, field):
    pattern = rf"(?m)^\s*{re.escape(field)}:\s*[^=\r\n]*=\s*([^\r\n]+)"
    return [value.strip() for value in re.findall(pattern, block)]


def param(block, field):
    values = params(block, field)
    require(len(values) == 1, f"Expected one {field}, got {values}")
    return values[0]


def handle(block, field):
    value = param(block, field)
    require(value == "NULL" or re.fullmatch(ADDRESS, value), f"Unexpected {field} handle {value!r}")
    return value.lower()


def calls(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    previous = None
    for current in CALL.finditer(text):
        if previous is not None:
            yield (*previous.groups(), text[previous.end():current.start()])
        previous = current
    if previous is not None:
        yield (*previous.groups(), text[previous.end():])


def barrier_records(block):
    for barrier in re.split(r"(?m)^\s*pImageMemoryBarriers\[\d+\]:[^\r\n]+", block)[1:]:
        old = param(barrier, "oldLayout").split(" ", 1)[0]
        new = param(barrier, "newLayout").split(" ", 1)[0]
        aspect = param(barrier, "aspectMask")
        layer_count = int(param(barrier, "layerCount"))
        image = handle(barrier, "image")
        yield (old.removeprefix("VK_IMAGE_LAYOUT_"), new.removeprefix("VK_IMAGE_LAYOUT_"), aspect, layer_count, image)


def verify_trace(api, capture_rows, *, batched):
    image_counts = {row["phase"]: row["swapchainImages"] for row in capture_rows}
    by_frame = defaultdict(list)
    allocations, recordings, pending = set(), {}, {}
    swapchains = []
    acquire_seen = set()
    for thread, frame, name, result, block in calls(api):
        frame = int(frame)
        if name == "vkCreateSwapchainKHR":
            require("VK_SUCCESS" in result, f"Swapchain recreation failed at API frame {frame}")
            swapchains.append(frame)
        if name in ("vkAcquireNextImageKHR", "vkAcquireNextImage2KHR"):
            acquire_seen.add(frame)
        if name == "vkAllocateCommandBuffers" and "VK_SUCCESS" in result:
            for value in re.findall(rf"(?m)^\s*pCommandBuffers\[\d+\]:[^=\r\n]*=\s*({ADDRESS})", block):
                allocations.add(value.lower())
        if name == "vkBeginCommandBuffer" and "VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT" in block:
            cmd = handle(block, "commandBuffer")
            require(cmd in allocations and cmd not in pending and "VK_SUCCESS" in result, f"Unexpected command buffer begin {cmd}")
            recordings[cmd] = {"beginFrame": frame, "clears": [], "barriers": [], "ended": False}
        if name == "vkCmdClearColorImage":
            cmd = handle(block, "commandBuffer")
            if cmd in recordings:
                rgba = tuple(float(param(block, f"float32[{i}]")) for i in range(4))
                if rgba == (0, 0, 0, 0) and int(param(block, "layerCount")) == 1:
                    recordings[cmd]["clears"].append(handle(block, "image"))
        if name in ("vkCmdPipelineBarrier", "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR"):
            cmd = handle(block, "commandBuffer")
            if cmd in recordings and "pImageMemoryBarriers[0]" in block:
                recordings[cmd]["barriers"].extend(barrier_records(block))
        if name == "vkEndCommandBuffer":
            cmd = handle(block, "commandBuffer")
            if cmd in recordings:
                require("VK_SUCCESS" in result, f"Command buffer end failed {cmd}")
                recordings[cmd]["ended"] = True
        if name in ("vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR"):
            field = r"pCommandBuffers\[\d+\]" if name == "vkQueueSubmit" else "commandBuffer"
            for value in re.findall(rf"(?m)^\s*{field}:[^=\r\n]*=\s*({ADDRESS})", block):
                cmd = value.lower()
                record = recordings.pop(cmd, None)
                if record is None:
                    continue
                cleared = set(record["clears"])
                cleared = {image for image in cleared if any(
                    old == "TRANSFER_DST_OPTIMAL" and new == "SHADER_READ_ONLY_OPTIMAL" and target == image
                    for old, new, _, _, target in record["barriers"])}
                oit, picking, fallbacks = set(), set(), set()
                for old, new, aspect, layers, image in record["barriers"]:
                    if old != "UNDEFINED" or layers != 1:
                        continue
                    if new == "GENERAL" and "VK_IMAGE_ASPECT_COLOR_BIT" in aspect:
                        oit.add(image)
                    elif new == "DEPTH_STENCIL_ATTACHMENT_OPTIMAL" and DEPTH_STENCIL in aspect:
                        picking.add(image)
                    elif new == "SHADER_READ_ONLY_OPTIMAL" and DEPTH_STENCIL in aspect:
                        fallbacks.add(image)
                # Match only the initialization combinations, not regular
                # frame batches that also contain OIT and picking transitions.
                has_frame_targets = len(cleared) >= 9 and len(cleared) % 9 == 0
                isolated_init = not cleared and len(record["barriers"]) == 1 and (oit or picking or fallbacks)
                if not (has_frame_targets or isolated_init):
                    continue
                require(record["ended"] and record["beginFrame"] == frame and "VK_SUCCESS" in result and
                        frame in TRACE_PHASES and frame not in acquire_seen,
                        f"Unexpected frame-resource init command submit at API frame {frame}")
                details = {"submit": name, "queue": handle(block, "queue"), "fence": handle(block, "fence"),
                           "colorClears": len(cleared), "oitTransitions": len(oit), "pickTransitions": len(picking),
                           "fallbackTransitions": len(fallbacks), "waits": []}
                pending[cmd] = details
                by_frame[frame].append(details)
        if name in ("vkQueueWaitIdle", "vkWaitForFences"):
            for details in pending.values():
                matched = (details["queue"] == handle(block, "queue") if name == "vkQueueWaitIdle"
                           else details["fence"] in [f.lower() for f in params(block, "pFences[0]")])
                if matched:
                    require("VK_SUCCESS" in result, f"Unsuccessful frame initialization wait at API frame {frame}")
                    details["waits"].append(name)
        if name == "vkFreeCommandBuffers":
            for value in re.findall(rf"(?m)^\s*pCommandBuffers\[\d+\]:[^=\r\n]*=\s*({ADDRESS})", block):
                cmd = value.lower()
                details = pending.pop(cmd, None)
                if details is not None:
                    require(len(details["waits"]) == 1, f"Frame init command {cmd} did not complete exactly once")
                    details["freedAfterWait"] = True
                allocations.discard(cmd)
                recordings.pop(cmd, None)
    require(swapchains == [0, 3], f"Unexpected swapchain startup/resize frame indices: {swapchains}")
    require(not pending and set(by_frame) == set(TRACE_PHASES), f"Incomplete frame init trace: {set(by_frame)}")
    batches = []
    for frame, phase in TRACE_PHASES.items():
        details = by_frame[frame]
        images = image_counts[phase]
        fallback = 2 if frame == 0 else 0
        expected_counts = {"colorClears": 9 * images, "oitTransitions": images, "pickTransitions": images,
                           "fallbackTransitions": fallback}
        expected_submits = 1 if batched else 3 * images + fallback
        sums = {key: sum(d[key] for d in details) for key in expected_counts}
        require(sums == expected_counts, f"Wrong initialized image counts for {phase}: {sums}")
        require(len(details) == expected_submits and all(d.get("freedAfterWait") and d["waits"] ==
                ["vkWaitForFences" if batched else "vkQueueWaitIdle"] for d in details),
                f"Wrong initialization submit/wait behavior for {phase}: {details}")
        batches.append({"phase": phase, "apiDumpFrame": frame, "frameImages": images, **sums,
                        "submits": len(details), "waits": len(details),
                        "submitCalls": dict(Counter(d["submit"] for d in details)),
                        "waitCall": details[0]["waits"][0]})
    return batches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--validation-layer", required=True, type=Path, help="directory containing the Khronos validation layer manifest")
    parser.add_argument("--api-dump-layer", required=True, type=Path, help="directory containing the LunarG API dump layer manifest")
    parser.add_argument("--batched", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    validation, api_dump = args.validation_layer.resolve(strict=True), args.api_dump_layer.resolve(strict=True)
    layers = layer_paths(validation, api_dump)
    api = output / "api.txt"
    settings = SETTINGS.format(filename=api.as_posix())
    if not args.verify_only:
        output.mkdir(parents=True)
        (output / "trace-settings.txt").write_text(settings, encoding="utf-8")
        env = dict(os.environ, CONTAINER_RUN_GPU_FRAME_INITIALIZATION="1", VK_LOADER_DEBUG="layer",
                   VK_LAYER_PATH=os.pathsep.join(map(str, (validation, api_dump))),
                   VK_LOADER_LAYERS_ENABLE="VK_LAYER_LUNARG_api_dump")
        result = subprocess.run([sys.executable, "-B", str(HARNESS), "--renderer", str(renderer),
                                 "--output", str(output / "native"), "--technique", "forward-raster",
                                 "--validation-settings", str(output / "trace-settings.txt")], env=env)
        (output / "exit-code.txt").write_text(f"{result.returncode}\n", encoding="utf-8")
        require(result.returncode == 0, "Traced renderer lifecycle failed")
    native = json.loads((output / "native/results.json").read_text())
    require(native["passed"] and native["executableSha256"] == sha(renderer), "Invalid traced native receipt")
    row, = native["runs"]
    text = Path(row["log"]).read_text(encoding="utf-8", errors="replace")
    insertions = Counter((kind, name, Path(file).resolve()) for kind, name, file in INSERTION.findall(text) if name in layers)
    expected = Counter({(kind, name, proof["library"]): 1
                        for name, proof in layers.items() for kind in ("instance", "device")})
    require(insertions == expected and not validation_log_error(text), f"Wrong trace layers or validation diagnostics: {insertions}")
    require((output / "native/vk_layer_settings.txt").read_text() == settings, "Trace layer settings changed")
    layer_proof = {name: {key: value for key, value in proof.items() if key != "library"} for name, proof in layers.items()}
    report = {"rendererSha256": native["executableSha256"], "apiSha256": sha(api), "layers": layer_proof,
              "technique": row["technique"], "revision": row["captures"][0]["revision"],
              "initialization": verify_trace(api, row["captures"], batched=args.batched), "passed": True}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print([(b["phase"], b["submits"], b["waits"]) for b in report["initialization"]], flush=True)


if __name__ == "__main__":
    main()
