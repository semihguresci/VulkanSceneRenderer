"""Compare PNG and EXR captures while a Windows renderer is visible, covered, or hidden."""

import argparse
import ctypes as ct
from ctypes import wintypes as wt
import hashlib
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time

from display_capture_test_support import emitter_fixture, validation_log_error
from renderer_mcp_runtime_regression import Client, initialize, wait_for


WIDTH, HEIGHT = 640, 480
TECHNIQUES = ("deferred-raster", "forward-raster")
STATES = ("visible", "covered", "hidden")
INTENSITIES = (0, 3)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Desktop:
    """Create an opaque test popup and inspect its coverage of the renderer HWND."""

    def __init__(self):
        user32 = ct.WinDLL("user32", use_last_error=True)
        gdi32 = ct.WinDLL("gdi32", use_last_error=True)
        self.callback_type = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

        def bind(name, result, *arguments):
            fn = getattr(user32, name)
            fn.restype = result
            fn.argtypes = arguments
            return fn

        dpi = bind("SetThreadDpiAwarenessContext", wt.HANDLE, wt.HANDLE)
        require(dpi(ct.c_void_p(-4)), "Could not enable physical-pixel Win32 coordinates")
        self.enum_windows = bind("EnumWindows", wt.BOOL, self.callback_type, wt.LPARAM)
        self.window_pid = bind("GetWindowThreadProcessId", wt.DWORD, wt.HWND, ct.POINTER(wt.DWORD))
        self.title = bind("GetWindowTextW", ct.c_int, wt.HWND, wt.LPWSTR, ct.c_int)
        self.client_rect = bind("GetClientRect", wt.BOOL, wt.HWND, ct.POINTER(wt.RECT))
        self.window_rect = bind("GetWindowRect", wt.BOOL, wt.HWND, ct.POINTER(wt.RECT))
        self.to_screen = bind("ClientToScreen", wt.BOOL, wt.HWND, ct.POINTER(wt.POINT))
        self.at_point = bind("WindowFromPoint", wt.HWND, wt.POINT)
        self.is_visible = bind("IsWindowVisible", wt.BOOL, wt.HWND)
        self.is_iconic = bind("IsIconic", wt.BOOL, wt.HWND)
        self.show = bind("ShowWindow", wt.BOOL, wt.HWND, ct.c_int)
        self.place = bind("SetWindowPos", wt.BOOL, wt.HWND, wt.HWND, ct.c_int, ct.c_int, ct.c_int, ct.c_int, wt.UINT)
        self.update = bind("UpdateWindow", wt.BOOL, wt.HWND)
        self.destroy = bind("DestroyWindow", wt.BOOL, wt.HWND)
        self.post = bind("PostMessageW", wt.BOOL, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
        self.peek = bind("PeekMessageW", wt.BOOL, ct.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT, wt.UINT)
        self.dispatch = bind("DispatchMessageW", wt.LPARAM, ct.POINTER(wt.MSG))
        self.desktop_dc = bind("GetDC", wt.HDC, wt.HWND)
        self.release_dc = bind("ReleaseDC", ct.c_int, wt.HWND, wt.HDC)
        self.get_pixel = gdi32.GetPixel
        self.get_pixel.argtypes = [wt.HDC, ct.c_int, ct.c_int]
        self.get_pixel.restype = wt.DWORD
        create = bind("CreateWindowExW", wt.HWND, wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                      ct.c_int, ct.c_int, ct.c_int, ct.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID)
        self.flush = ct.WinDLL("dwmapi").DwmFlush
        self.flush.argtypes = []
        self.flush.restype = ct.c_long

        class WindowClass(ct.Structure):
            _fields_ = [("style", wt.UINT), ("windowProc", ct.c_void_p), ("classExtra", ct.c_int),
                        ("windowExtra", ct.c_int), ("instance", wt.HINSTANCE), ("icon", wt.HICON),
                        ("cursor", wt.HANDLE), ("background", wt.HBRUSH), ("menuName", wt.LPCWSTR),
                        ("className", wt.LPCWSTR)]

        kernel32 = ct.WinDLL("kernel32")
        kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
        kernel32.GetModuleHandleW.restype = wt.HINSTANCE
        gdi32.GetStockObject.argtypes = [ct.c_int]
        gdi32.GetStockObject.restype = wt.HANDLE
        instance = kernel32.GetModuleHandleW(None)
        register = bind("RegisterClassW", wt.ATOM, ct.POINTER(WindowClass))
        name = "ContainerCaptureTestCover"
        definition = WindowClass(windowProc=ct.cast(user32.DefWindowProcW, ct.c_void_p), instance=instance,
                                 background=gdi32.GetStockObject(4), className=name)  # BLACK_BRUSH
        require(register(ct.byref(definition)), f"Could not register the test popup ({ct.get_last_error()})")
        # WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, WS_POPUP.
        self.cover = create(0x08000088, name, "Container capture test occluder", 0x80000000,
                            0, 0, 0, 0, None, None, instance, None)
        require(self.cover, f"Could not create the Win32 test occluder ({ct.get_last_error()})")

    def find(self, pid):
        found = []

        @self.callback_type
        def examine(hwnd, _):
            owner = wt.DWORD()
            self.window_pid(hwnd, ct.byref(owner))
            if owner.value == pid:
                title = ct.create_unicode_buffer(32)
                self.title(hwnd, title, len(title))
                if title.value == "Vulkan":
                    found.append(hwnd)
            return True

        require(self.enum_windows(examine, 0), "Could not enumerate Win32 windows")
        require(len(found) <= 1, f"Multiple renderer windows belong to process {pid}")
        return found[0] if found else None

    def geometry(self, hwnd):
        client, origin = wt.RECT(), wt.POINT()
        require(self.client_rect(hwnd, ct.byref(client)) and self.to_screen(hwnd, ct.byref(origin)),
                "Could not measure the renderer client area")
        return origin.x, origin.y, client.right, client.bottom

    def apply(self, hwnd, state):
        x, y, width, height = self.geometry(hwnd)
        require((width, height) == (WIDTH, HEIGHT), "Renderer client size differs from the capture extent")
        if state == "visible":
            # Bring only the test window above unrelated windows without activating it.
            # HWND_TOPMOST, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW.
            require(self.place(hwnd, ct.c_void_p(-1), 0, 0, 0, 0, 0x0053),
                    "Could not show the test renderer above other windows")
        elif state == "hidden":
            self.show(self.cover, 0)
            self.show(hwnd, 0)  # SW_HIDE leaves the window and swapchain extent intact.
        elif state == "covered":
            # HWND_TOPMOST, SWP_NOACTIVATE | SWP_SHOWWINDOW. Cover the entire right half.
            require(self.place(self.cover, ct.c_void_p(-1), x + width // 2, y, width // 2, height, 0x0050),
                    "Could not position the opaque test window")
            self.update(self.cover)
        require(self.flush() == 0, "Could not wait for the Windows desktop compositor")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            matches, sample = self.sample(hwnd, state)
            if matches:
                return
            time.sleep(.05)
        raise RuntimeError(f"Could not establish {state} test renderer visibility: {sample}")

    def sample(self, hwnd, state):
        message = wt.MSG()
        while self.peek(ct.byref(message), self.cover, 0, 0, 1):  # PM_REMOVE
            self.dispatch(ct.byref(message))
        x, y, width, height = self.geometry(hwnd)
        visible = bool(self.is_visible(hwnd))
        require((width, height) == (WIDTH, HEIGHT) and not self.is_iconic(hwnd),
                "The capture window resized or was minimized")
        points = [(x + (width * column) // 4, y + (height * row) // 4)
                  for column in (1, 3) for row in (1, 2, 3)]
        owners = [self.at_point(wt.POINT(*point)) for point in points]
        exposed = owners[:3] == [hwnd] * 3
        if state == "visible":
            matches = visible and exposed and owners[3:] == [hwnd] * 3
        elif state == "covered":
            matches = visible and exposed and owners[3:] == [self.cover] * 3
        else:
            matches = not visible and hwnd not in owners
        sample = {"clientRect": [x, y, width, height], "windowVisible": visible,
                  "clientProbeOwners": ["renderer" if owner == hwnd else "cover" if owner == self.cover
                                        else "other" for owner in owners]}
        if state == "covered":
            cover = wt.RECT()
            require(self.window_rect(self.cover, ct.byref(cover)), "Could not measure the opaque test window")
            sample["coverRect"] = [cover.left, cover.top, cover.right, cover.bottom]
            require(sample["coverRect"] == [x + width // 2, y, x + width, y + height] and self.is_visible(self.cover),
                    f"Test occluder is not shown at the intended bounds: {sample}")
        return matches, sample

    def observe(self, hwnd, state):
        matches, sample = self.sample(hwnd, state)
        if not matches:
            return None
        if state == "covered":
            x, y, width, height = sample["clientRect"]
            dc = self.desktop_dc(None)
            require(dc, "Could not sample the desktop pixels covered by the test window")
            try:
                colors = [self.get_pixel(dc, x + 3 * width // 4, y + row * height // 4) for row in (1, 2, 3)]
                require(colors == [0, 0, 0], f"Test occluder is not opaque black on screen: {colors}")
                sample["coveredDesktopColors"] = colors
            finally:
                self.release_dc(None, dc)
        return sample


def read_image(path, *, viewport, includes_ui):
    import cv2
    import numpy as np
    from PIL import Image

    metadata = json.loads(Path(f"{path}.capture.json").read_bytes())
    require((metadata["width"], metadata["height"], metadata["includesUi"]) == (WIDTH, HEIGHT, includes_ui), metadata)
    if path.suffix == ".exr":
        require(metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB"
                and not metadata["exposureApplied"] and not metadata["toneMappingApplied"], metadata)
        bgr = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        require(bgr is not None and bgr.shape == (HEIGHT, WIDTH, 3) and bgr.dtype == np.float32
                and np.isfinite(bgr).all(), f"Missing or nonfinite float RGB capture: {path}")
        return bgr[:, :, ::-1], metadata
    require(metadata["source"] == ("viewport" if viewport else "window")
            and metadata["colorSpace"] in ("sRGB", "linear-sRGB"), metadata)
    with Image.open(path) as image:
        require(image.format == "PNG" and image.size == (WIDTH, HEIGHT), f"Wrong PNG dimensions or format: {path}")
        return np.asarray(image.convert("RGBA")), metadata


def capture(client, desktop, hwnd, folder, name, state, *, viewport=False, includes_ui=False):
    path = folder / name
    before = desktop.observe(hwnd, state)
    require(before, f"Incorrect window visibility before {path}")
    require(client.call("renderer_capture", {"path": name, "viewportOnly": viewport})["queued"], f"Capture did not queue: {path}")

    def saved():
        status = client.call("renderer_get_status")
        require(not status["captureStatus"].startswith("Capture failed:"), status)
        return (not status["capturePending"] and status["captureStatus"] == "Saved"
                and path.is_file() and Path(f"{path}.capture.json").is_file())

    wait_for(saved, timeout=30)
    after = desktop.observe(hwnd, state)
    require(after == before, f"Window coverage changed during {path}")
    image, metadata = read_image(path, viewport=viewport, includes_ui=includes_ui)
    return image, {"image": str(path), "metadata": metadata, "desktop": before,
                   "decodedSha256": hashlib.sha256(image.tobytes()).hexdigest()}


def check_images(images):
    import numpy as np

    visible = images["visible"]
    # The right half must contain analytic emission and changing scene-linear
    # pixels. Bright and dim frames are interleaved in one renderer process.
    dark, bright = (visible[f"radiance-{intensity}.exr"] for intensity in INTENSITIES)
    np.testing.assert_allclose(dark[HEIGHT // 2, WIDTH // 2 + 20], [4, 2, 1], atol=.008, rtol=0)
    changed = np.any(dark[:, WIDTH // 2:] != bright[:, WIDTH // 2:], axis=2)
    require(np.count_nonzero(changed) > 1000, "The covered EXR region has no meaningful frame-to-frame change")
    require(np.count_nonzero(np.any(visible["viewport.png"] != visible["window.png"], axis=2)) > 1000,
            "Window PNG did not include GUI pixels excluded from viewport PNG")
    for state in STATES[1:]:
        for name in visible:
            np.testing.assert_array_equal(images[state][name], visible[name], err_msg=f"{state} {name}")
    return {"changedRightHalfExrPixels": int(np.count_nonzero(changed)),
            "viewportAndWindowDifferent": True, "allVisibilityCapturesEqual": True}


def run_technique(renderer, output, technique, desktop, report):
    folder = output / technique
    folder.mkdir()
    log = folder / "renderer.log"
    model = output / "emitter.gltf"
    command = [str(renderer), "--hidden", "--validation", "--width", str(WIDTH), "--height", str(HEIGHT), "--msaa", "1",
               "--no-taa", "--no-bloom", "--no-ddgi", "--no-ray-query", "--ray-shadows", "raster",
               "--render-technique", technique, "--display-mode", "lit", "--exposure", "1", "--tone-map", "aces",
               "--camera-position", "0", "0", "4", "--camera-target", "0", "0", "0", "--camera-fov", "45",
               "--sky", "plain", "--sky-horizon", "0", "0", "0", "--import-scale", "1", "--model", str(model),
               "--directional-intensity", "0", "--directional-direction", "0", "0", "-1",
               "--environment-intensity", "0", "--bounce-intensity", "0"]
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    token = secrets.token_hex(24)
    env = dict(os.environ, CONTAINER_MCP_PORT=str(port), CONTAINER_MCP_TOKEN=token, CONTAINER_MCP_ROOT=str(folder),
               VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer")
    images = {}
    row = {"technique": technique, "command": command, "log": str(log), "captures": []}
    report["techniques"].append(row)
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(command, cwd=renderer.parent, env=env, stdout=stream, stderr=subprocess.STDOUT)
        hwnd = None
        try:
            client = Client(port, token)
            initialize(client)
            require(client.call("renderer_get_settings")["technique"] == technique, "Wrong renderer technique")
            hwnd = wait_for(lambda: desktop.find(process.pid), timeout=15)
            for state in STATES:
                desktop.apply(hwnd, state)
                images[state] = {}
                for intensity in INTENSITIES:
                    setting = client.call("renderer_set_lighting", {"directionalIntensity": intensity})
                    require(setting["lighting"]["directionalIntensity"] == intensity, setting)
                    name = f"radiance-{intensity}.exr"
                    images[state][name], evidence = capture(client, desktop, hwnd, folder, f"{state}-{name}", state)
                    row["captures"].append({"state": state, "kind": name, **evidence})
                for name, viewport in (("viewport.png", True), ("window.png", False)):
                    images[state][name], evidence = capture(client, desktop, hwnd, folder, f"{state}-{name}", state,
                                                             viewport=viewport, includes_ui=not viewport)
                    row["captures"].append({"state": state, "kind": name, **evidence})
        finally:
            desktop.show(desktop.cover, 0)
            hwnd = hwnd or desktop.find(process.pid)
            if hwnd and process.poll() is None:
                desktop.post(hwnd, 0x0010, 0, 0)  # WM_CLOSE
            try:
                row["exitCode"] = process.wait(timeout=12)
            except subprocess.TimeoutExpired:
                process.kill()
                row["exitCode"] = process.wait(timeout=10)
    log_text = log.read_text(encoding="utf-8", errors="replace")
    require(row["exitCode"] == 0 and not validation_log_error(log_text)
            and 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in log_text, f"Renderer or Vulkan validation failed: {log}")
    row.update(check_images(images))
    print(f"{technique}: {len(row['captures'])} visible, covered and hidden captures matched", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if os.name != "nt" or os.environ.get("CONTAINER_RUN_GPU_CAPTURE_VISIBILITY") != "1":
        print("SKIP: requires Windows desktop and CONTAINER_RUN_GPU_CAPTURE_VISIBILITY=1")
        return 77
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    emitter_fixture(output / "emitter.gltf", {
        "emissiveFactor": [.5, .25, .125],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 8}},
        "pbrMetallicRoughness": {"baseColorFactor": [.45, .4, .35, 1], "metallicFactor": 0, "roughnessFactor": .65},
    })
    renderer_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report = {"rendererSha256": renderer_hash, "techniques": [], "passed": False}
    desktop = Desktop()
    try:
        for technique in TECHNIQUES:
            run_technique(renderer, output, technique, desktop, report)
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == renderer_hash, "Renderer executable changed during the run")
        report["passed"] = True
    finally:
        desktop.destroy(desktop.cover)
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
