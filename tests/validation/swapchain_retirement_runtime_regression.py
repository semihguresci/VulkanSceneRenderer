"""Verify Win32 swapchain generation retirement with synchronization validation and API dump."""

import argparse
from collections import defaultdict
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import tempfile

from display_capture_test_support import emitter_fixture, validation_log_error
from frame_resource_initialization_api_trace import INSERTION, SETTINGS, calls, layer_paths, param, params, require, sha


MAINTENANCE_EXTENSIONS = {"VK_KHR_swapchain_maintenance1", "VK_EXT_swapchain_maintenance1"}
QUEUED = {"VK_SUCCESS", "VK_SUBOPTIMAL_KHR", "VK_ERROR_OUT_OF_DATE_KHR", "VK_ERROR_SURFACE_LOST_KHR",
          "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT", "VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT"}
PRESENTED = {"VK_SUCCESS", "VK_SUBOPTIMAL_KHR"}
START = (320, 240)
FINAL = (416, 264)
SAMPLES = {3: START, 9: (392, 252), 15: (364, 236), 22: FINAL, 36: FINAL}
EVENTS = [
    {"frame": 4, "resize": [352, 220]},
    {"frame": 5, "resize": [384, 216]},
    {"frame": 6, "resize": [392, 252]},
    {"frame": 10, "minimized": True},
    {"frame": 11, "resize": [364, 236]},
    {"frame": 13, "minimized": False, "resize": [364, 236]},
    {"frame": 16, "resize": [376, 244], "acquireOutOfDate": True},
    {"frame": 17, "presentSuboptimal": True},
    {"frame": 18, "minimized": True},
    {"frame": 20, "minimized": False, "resize": list(FINAL)},
]


def vk_handle(value):
    if value in {"NULL", "VK_NULL_HANDLE"}:
        return None
    require(re.fullmatch(r"(?:0x)?[0-9a-fA-F]+", value), f"Unexpected Vulkan handle {value!r}")
    return int(value, 16) or None


def handle_field(block, name):
    return vk_handle(param(block, name))


def indexed(block, name):
    return re.findall(rf"(?m)^\s*{re.escape(name)}\[\d+\]:\s*[^=\r\n]*=\s*([^\r\n]+)", block)


def submit_waits(call, block):
    if call == "vkQueueSubmit":
        return {vk_handle(value.strip()) for value in indexed(block, "pWaitSemaphores")}
    sections = re.findall(r"(?ms)^\s*pWaitSemaphoreInfos\[\d+\]:[^\r\n]*\n(.*?)^\s*deviceIndex:[^\r\n]*", block)
    return {handle_field(section, "semaphore") for section in sections}


class RetirementTrace:
    def __init__(self):
        self.generations = {}
        self.presents = []
        self.acquires = []
        self.latest_present = {}
        self.fence_work = defaultdict(list)
        self.acquire_semaphores = {}
        self.semaphore_destroys = {}
        self.fence_destroys = {}
        self.device_extensions = set()
        self.maintenance_feature = False
        self.device_idle = []
        self.device_destroy = None

    def observe(self, index, frame, call, result, block):
        status = re.search(r"\bVK_(?:SUCCESS|NOT_READY|TIMEOUT|SUBOPTIMAL_KHR|ERROR_[A-Z_]+)\b", result)
        status = status[0] if status else None
        point = {"call": index, "apiFrame": int(frame)}
        if call == "vkCreateDevice" and status == "VK_SUCCESS":
            self.device_extensions = {name.strip('\"') for name in indexed(block, "ppEnabledExtensionNames")}
            self.maintenance_feature = any(value in {"1", "VK_TRUE"} for value in params(block, "swapchainMaintenance1"))
        elif call == "vkCreateSwapchainKHR" and status == "VK_SUCCESS":
            chain = handle_field(block, "pSwapchain")
            self.generations[chain] = {"handle": chain, "old": handle_field(block, "oldSwapchain"), "created": point,
                                       "extent": [int(param(block, key)) for key in ("width", "height")], "presents": []}
        elif call == "vkQueuePresentKHR":
            chains = indexed(block, "pSwapchains")
            require(len(chains) == 1, f"Expected one swapchain per present at {point}")
            chain = vk_handle(chains[0].strip())
            image = int(param(block, "pImageIndices[0]"))
            semaphores = [vk_handle(value.strip()) for value in indexed(block, "pWaitSemaphores")]
            present_fences = [vk_handle(value.strip()) for value in indexed(block, "pFences")]
            require(len(semaphores) == 1 and len(present_fences) <= 1,
                    f"Unexpected presentation resource count at {point}")
            fence = present_fences[0] if present_fences else None
            record = {**point, "result": status, "image": image, "semaphore": semaphores[0], "fence": fence}
            self.presents.append((chain, record))
            self.generations[chain]["presents"].append(record)
            if status in QUEUED and fence is not None:
                self.fence_work[fence].append((record, "presentFenceReleased", point))
            self.latest_present[chain, image] = record if status in PRESENTED else None
        elif call == "vkAcquireNextImageKHR" and status in PRESENTED:
            chain = handle_field(block, "swapchain")
            image = int(param(block, "pImageIndex"))
            fence, semaphore = handle_field(block, "fence"), handle_field(block, "semaphore")
            acquisition = {**point, "chain": chain, "image": image, "fence": fence, "semaphore": semaphore}
            self.acquires.append(acquisition)
            if fence is not None:
                self.fence_work[fence].append((acquisition, "acquireFenceCompleted", point))
            last = self.latest_present.get((chain, image))
            if last is not None and "reacquired" not in last:
                if fence is not None:
                    self.fence_work[fence].append((last, "reacquired", point))
                elif semaphore is not None:
                    self.acquire_semaphores[semaphore] = (last, "reacquired", point)
        elif call in {"vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR"} and status == "VK_SUCCESS":
            for semaphore in submit_waits(call, block) & self.acquire_semaphores.keys():
                work = self.acquire_semaphores.pop(semaphore)
                fence = handle_field(block, "fence")
                if fence is not None:
                    self.fence_work[fence].append(work)
        elif call in {"vkGetFenceStatus", "vkWaitForFences"} and status == "VK_SUCCESS":
            if call == "vkGetFenceStatus":
                fences = [handle_field(block, "fence")]
            else:
                fences = [vk_handle(value.strip()) for value in indexed(block, "pFences")]
                if len(fences) > 1 and param(block, "waitAll") not in {"1", "VK_TRUE"}:
                    fences = []
            for fence in fences:
                for record, role, witness in self.fence_work.pop(fence, ()):
                    record.setdefault(role, {"started": witness, "completed": point, "fence": fence})
        elif call == "vkResetFences" and status == "VK_SUCCESS":
            for value in indexed(block, "pFences"):
                self.fence_work.pop(vk_handle(value.strip()), None)
        elif call == "vkDeviceWaitIdle" and status == "VK_SUCCESS":
            self.device_idle.append(index)
        elif call == "vkDestroyFence":
            fence = handle_field(block, "fence")
            self.fence_destroys[fence] = index
            self.fence_work.pop(fence, None)
        elif call == "vkDestroySemaphore":
            self.semaphore_destroys[handle_field(block, "semaphore")] = index
        elif call == "vkDestroySwapchainKHR":
            chain = handle_field(block, "swapchain")
            self.generations[chain]["destroyed"] = point
        elif call == "vkDestroyDevice":
            self.device_destroy = index

    def verify(self, mode, expect_eager_reclamation=False):
        maintenance = mode == "maintenance1"
        expected = MAINTENANCE_EXTENSIONS & self.device_extensions
        require(bool(expected) == maintenance and self.maintenance_feature == maintenance,
                f"Device did not create the requested {mode} configuration: {expected}, feature={self.maintenance_feature}")
        require(self.device_destroy is not None, "The renderer did not complete device teardown")
        if not expect_eager_reclamation:
            for acquisition in self.acquires:
                fence = acquisition["fence"]
                completed = acquisition.get("acquireFenceCompleted", {}).get("completed", {}).get("call")
                require(fence is not None and completed is not None and
                        completed < self.fence_destroys.get(fence, 0) < self.device_destroy,
                        f"Acquire fence was not observed complete before teardown: {acquisition}")
                if acquisition["semaphore"] is not None:
                    require(completed < self.semaphore_destroys.get(acquisition["semaphore"], 0) < self.device_destroy,
                            f"Acquire semaphore was destroyed before the acquire fence completed: {acquisition}")
        generations = list(self.generations.values())
        require(len(generations) >= 7 and len(self.presents) >= 24,
                "The trace did not exercise repeated recreation and continued presentation")
        observed_extents = [generation["extent"] for generation in generations]
        require(all(size in observed_extents for size in
                    ([352, 220], [384, 216], [392, 252], [364, 236], [376, 244], list(FINAL))),
                f"Missing real swapchain resize: {observed_extents}")
        latest_present = self.presents[-1][1]["call"]
        retired_while_rendering = 0
        eager_reclamations = 0
        unused_replacements = 0
        for ordinal, generation in enumerate(generations):
            chain = generation["handle"]
            destroyed = generation["destroyed"]["call"]
            require(destroyed < self.device_destroy, f"Swapchain {chain:#x} outlived the device")
            if ordinal:
                require(generation["old"] == generations[ordinal - 1]["handle"], f"Replacement did not name its predecessor: {generation}")
            queued = [present for present in generation["presents"] if present["result"] in QUEUED]
            replacements = generations[ordinal + 1:]
            if queued and replacements:
                require(destroyed > replacements[0]["created"]["call"], f"Released swapchain before replacement creation: {generation}")
                retired_while_rendering += destroyed < latest_present
            if not generation["presents"] and replacements:
                first_next_present = min(present["call"] for replacement in replacements for present in replacement["presents"])
                require(replacements[0]["created"]["call"] < destroyed < first_next_present,
                        f"Unused replacement was not reclaimed before the next present: {generation}")
                if generation["extent"] == [376, 244] == replacements[0]["extent"]:
                    unused_replacements += 1
            if maintenance:
                for present in queued:
                    fence = present["fence"]
                    completion = present.get("presentFenceReleased", {}).get("completed", {}).get("call")
                    semaphore_destroy = self.semaphore_destroys.get(present["semaphore"], self.device_destroy)
                    require(fence is not None and completion is not None and completion < min(destroyed, semaphore_destroy),
                            f"Released presentation resources without a completed present fence: {present}, swapchain {chain:#x}")
                    require(fence in self.fence_destroys and self.fence_destroys[fence] < self.device_destroy,
                            f"Present fence {fence:#x} survived device teardown")
                generation["retirementEvidence"] = "present-fences" if queued else "no-present"
            elif queued and replacements:
                progress = [present["reacquired"] for replacement in replacements for present in replacement["presents"]
                            if "reacquired" in present and present["reacquired"]["completed"]["call"] < destroyed]
                if progress:
                    release = min(proof["completed"]["call"] for proof in progress)
                    for present in queued:
                        semaphore_destroy = self.semaphore_destroys.get(present["semaphore"], self.device_destroy)
                        require(release < semaphore_destroy, f"Released old present wait semaphore early: {present}")
                    generation["retirementEvidence"] = "replacement-reacquire"
                    generation["replacementProgress"] = release
                else:
                    require(expect_eager_reclamation,
                            f"Released old swapchain before replacement reacquisition was complete: {generation}")
                    generation["retirementEvidence"] = "old-swapchain-freed-before-reacquire"
                    eager_reclamations += 1
            else:
                generation["retirementEvidence"] = "base-wsi-shutdown" if queued else "no-present"
                if queued:
                    require(any(latest_present < idle < destroyed for idle in self.device_idle),
                            "Base WSI shutdown must retire graphics work before cleanup")
            for present in generation["presents"]:
                semaphore_destroy = self.semaphore_destroys.get(present["semaphore"])
                require(semaphore_destroy is not None and present["call"] < semaphore_destroy < self.device_destroy,
                        f"Lost or prematurely destroyed present wait semaphore: {present}")
                require(maintenance or present["fence"] is None, f"Unexpected maintenance fence in fallback: {present}")
        require(retired_while_rendering >= 2, "No repeated reclamation while the renderer was still presenting")
        require(unused_replacements == 1, f"Expected one unused 376×244 replacement, got {unused_replacements}")
        if expect_eager_reclamation:
            require(eager_reclamations >= 2, "Expected multiple pre-fix swapchain removals before a replacement reacquisition")
        return {"maintenanceExtensions": sorted(expected), "maintenanceFeature": self.maintenance_feature,
                "presentCount": len(self.presents), "acquireCount": len(self.acquires),
                "retiredWhileRendering": retired_while_rendering,
                "unusedReplacements": unused_replacements, "eagerReclamationCount": eager_reclamations,
                "swapchains": generations, "deviceDestroyCall": self.device_destroy}


def check_trace(path, mode, expect_eager_reclamation=False):
    trace = RetirementTrace()
    for index, (_, frame, call, result, block) in enumerate(calls(path)):
        trace.observe(index, frame, call, result, block)
    return trace.verify(mode, expect_eager_reclamation)


def run_on_test_desktop(command, cwd, env, log):
    # A separate desktop lets GLFW show and minimize a real HWND without giving
    # our process focus or putting test windows onto the user's desktop.
    import ctypes as ct
    from ctypes import wintypes as wt
    import msvcrt

    class StartupInfo(ct.Structure):
        _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                    *[(name, wt.DWORD) for name in ("dwX", "dwY", "dwXSize", "dwYSize", "dwXCountChars",
                                                   "dwYCountChars", "dwFillAttribute", "dwFlags")],
                    ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD), ("lpReserved2", wt.LPVOID),
                    ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]

    class ProcessInfo(ct.Structure):
        _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]

    user = ct.WinDLL("user32", use_last_error=True)
    kernel = ct.WinDLL("kernel32", use_last_error=True)

    def bind(library, name, result, *args):
        function = getattr(library, name)
        function.argtypes, function.restype = args, result
        return function

    create_desktop = bind(user, "CreateDesktopW", wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, wt.LPVOID, wt.DWORD,
                          wt.DWORD, wt.LPVOID)
    close_desktop = bind(user, "CloseDesktop", wt.BOOL, wt.HANDLE)
    create_process = bind(kernel, "CreateProcessW", wt.BOOL, wt.LPCWSTR, wt.LPWSTR, wt.LPVOID, wt.LPVOID,
                          wt.BOOL, wt.DWORD, wt.LPVOID, wt.LPCWSTR, ct.POINTER(StartupInfo), ct.POINTER(ProcessInfo))
    close_handle = bind(kernel, "CloseHandle", wt.BOOL, wt.HANDLE)
    wait = bind(kernel, "WaitForSingleObject", wt.DWORD, wt.HANDLE, wt.DWORD)
    exit_code = bind(kernel, "GetExitCodeProcess", wt.BOOL, wt.HANDLE, wt.LPDWORD)
    terminate = bind(kernel, "TerminateProcess", wt.BOOL, wt.HANDLE, wt.UINT)

    name = f"ContainerSwapchainRegression-{secrets.token_hex(8)}"
    # DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_WRITEOBJECTS.
    desktop = create_desktop(name, None, None, 0, 0x0083, None)
    if not desktop:
        raise ct.WinError(ct.get_last_error())
    try:
        with open(os.devnull, "rb") as stdin, log.open("wb") as stream:
            input_handle = msvcrt.get_osfhandle(stdin.fileno())
            output_handle = msvcrt.get_osfhandle(stream.fileno())
            os.set_handle_inheritable(input_handle, True)
            os.set_handle_inheritable(output_handle, True)
            startup = StartupInfo(cb=ct.sizeof(StartupInfo), lpDesktop=name, dwFlags=0x0100,
                                  hStdInput=input_handle, hStdOutput=output_handle, hStdError=output_handle)
            process = ProcessInfo()
            command_line = ct.create_unicode_buffer(subprocess.list2cmdline(command))
            # CreateProcessW requires a sorted, double-NUL-terminated Unicode environment block.
            environment = ct.create_unicode_buffer("\0".join(f"{key}={env[key]}" for key in sorted(env, key=str.casefold)) + "\0")
            # CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW.
            if not create_process(command[0], command_line, None, None, True, 0x08000400, environment, str(cwd),
                                  ct.byref(startup), ct.byref(process)):
                raise ct.WinError(ct.get_last_error())
            close_handle(process.hThread)
            try:
                result = wait(process.hProcess, 180_000)
                if result == 258:  # WAIT_TIMEOUT
                    terminate(process.hProcess, 1)
                    wait(process.hProcess, 5000)
                    raise RuntimeError(f"Renderer timed out. See {log}")
                code = wt.DWORD()
                if result != 0 or not exit_code(process.hProcess, ct.byref(code)):
                    raise ct.WinError(ct.get_last_error())
                return code.value
            finally:
                close_handle(process.hProcess)
    finally:
        close_desktop(desktop)


def write_inputs(output):
    model = output / "emitter.gltf"
    emitter_fixture(model, {"emissiveFactor": [.5, .25, .125],
                            "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 8}},
                            "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1]}})
    sequence = output / "sequence.json"
    sequence.write_text(json.dumps({"schemaVersion": 1, "frames": max(SAMPLES), "sampleFrames": list(SAMPLES),
                                    "events": EVENTS}, indent=2) + "\n", encoding="utf-8")
    return model, sequence


def check_captures(image):
    from PIL import Image

    captures = []
    for frame, extent in SAMPLES.items():
        path = image if frame == max(SAMPLES) else image.with_name(f"{image.stem}.frame-{frame:04d}.png")
        state = json.loads(path.with_suffix(".telemetry.json").read_bytes())
        metadata = json.loads(Path(f"{path}.capture.json").read_bytes())
        require(state["validationEnabled"] and state["resolution"] == list(extent), f"Wrong renderer state at frame {frame}")
        require(metadata["source"] == "viewport" and not metadata["includesUi"] and
                (metadata["width"], metadata["height"]) == extent, f"Wrong viewport metadata at frame {frame}")
        with Image.open(path) as png:
            require(png.format == "PNG" and png.size == extent, f"Wrong PNG at frame {frame}")
            rgb = png.convert("RGB").getpixel((extent[0] // 2, extent[1] // 2))
            require(all(abs(actual - expected) <= 2 for actual, expected in zip(rgb, (231, 213, 188), strict=True)),
                    f"Restored swapchain did not contain its expected image at frame {frame}: {rgb}")
        captures.append({"frame": frame, "extent": list(extent), "rgb": rgb, "revision": state["build"]["revision"],
                         "recreates": state["swapchainRecreates"], "pngSha256": sha(path)})
    require(captures[2]["recreates"] >= 4 and captures[-1]["recreates"] >= 7,
            f"Missing minimize/restore or repeated recreation: {captures}")
    return captures


def run(mode, renderer, output, layers, env, report):
    output.mkdir()
    model, sequence = write_inputs(output)
    api, image, log = output / "api.txt", output / "capture.png", output / "renderer.log"
    (output / "vk_layer_settings.txt").write_text(SETTINGS.format(filename=api.as_posix()), encoding="utf-8")
    command = [str(renderer), "--model", str(model), "--import-scale", "1", "--validation", "--no-ui",
               "--width", str(START[0]), "--height", str(START[1]), "--msaa", "1", "--no-taa", "--no-bloom",
               "--no-ray-query", "--no-ddgi", "--ray-shadows", "raster", "--render-technique", "forward-raster",
               "--display-mode", "lit", "--exposure", "1", "--tone-map", "reinhard", "--camera-fov", "45",
               "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0", "--sky", "plain",
               "--sky-horizon", "0", "0", "0", "--environment-intensity", "0", "--bounce-intensity", "0",
               "--directional-intensity", "0", "--fixed-dt", ".016666667", "--screenshot-viewport", "--capture-sequence",
               str(sequence), "--screenshot", str(image)]
    if mode == "fallback":
        env = dict(env, CONTAINER_DISABLE_SWAPCHAIN_MAINTENANCE1="1")
    else:
        env = {key: value for key, value in env.items() if key != "CONTAINER_DISABLE_SWAPCHAIN_MAINTENANCE1"}
    row = {"mode": mode, "command": command, "log": str(log)}
    report["runs"].append(row)
    row["rendererExitCode"] = run_on_test_desktop(command, renderer.parent, dict(env, VK_LAYER_SETTINGS_PATH=str(output)), log)
    text = log.read_text(encoding="utf-8", errors="replace")
    require(row["rendererExitCode"] == 0 and not validation_log_error(text), f"Renderer or synchronization validation failed. See {log}")
    insertions = {(kind, name, Path(dll).resolve()) for kind, name, dll in INSERTION.findall(text)}
    require(all((kind, name, proof["library"]) in insertions for name, proof in layers.items()
                for kind in ("instance", "device")), f"Unexpected validation or API dump layer. See {log}")
    row["captures"] = check_captures(image)
    row["apiSha256"] = sha(api)
    row.update(check_trace(api, mode, report["expectedEagerReclamation"]))
    print(f"{mode}: {len(row['swapchains'])} swapchains, {row['presentCount']} presents, "
          f"{row['retiredWhileRendering']} generations reclaimed while rendering", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--validation-layer-dir", type=Path,
                        default=os.environ.get("CONTAINER_SWAPCHAIN_RETIREMENT_VALIDATION_LAYER_DIR"))
    parser.add_argument("--api-dump-layer-dir", type=Path,
                        default=os.environ.get("CONTAINER_SWAPCHAIN_RETIREMENT_API_DUMP_LAYER_DIR"))
    parser.add_argument("--mode", choices=("maintenance1", "fallback"), action="append")
    parser.add_argument("--managed-output", action="store_true")
    parser.add_argument("--expect-eager-reclamation", action="store_true", help="verify early retirement in a pre-fix renderer")
    args = parser.parse_args()
    if os.name != "nt" or os.environ.get("CONTAINER_RUN_GPU_SWAPCHAIN_RETIREMENT") != "1":
        print("SKIP: requires Windows and CONTAINER_RUN_GPU_SWAPCHAIN_RETIREMENT=1")
        return 77
    if not args.validation_layer_dir or not args.api_dump_layer_dir:
        parser.error("supply --validation-layer-dir and --api-dump-layer-dir or set "
                     "CONTAINER_SWAPCHAIN_RETIREMENT_VALIDATION_LAYER_DIR and CONTAINER_SWAPCHAIN_RETIREMENT_API_DUMP_LAYER_DIR")

    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    validation = args.validation_layer_dir.resolve(strict=True)
    api_dump = args.api_dump_layer_dir.resolve(strict=True)
    layers = layer_paths(validation, api_dump)
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    else:
        output.mkdir(parents=True)
    env = dict(os.environ, VK_LAYER_PATH=os.pathsep.join(map(str, (validation, api_dump))), VK_LOADER_DEBUG="layer",
               VK_LOADER_LAYERS_ENABLE="VK_LAYER_LUNARG_api_dump")
    require(not args.expect_eager_reclamation or args.mode == ["fallback"], "The baseline control requires --mode fallback")
    report = {"executableSha256": sha(renderer), "passed": False, "runs": [],
              "expectedEagerReclamation": args.expect_eager_reclamation,
              "layers": {name: {**proof, "library": str(proof["library"])} for name, proof in layers.items()}}
    try:
        for mode in args.mode or ("maintenance1", "fallback"):
            run(mode, renderer, output / mode, layers, env, report)
        require(sha(renderer) == report["executableSha256"], "Renderer changed during the test")
        report["passed"] = True
    finally:
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
