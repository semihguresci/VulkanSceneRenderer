"""Compare PNG and EXR captures across native Win32 visibility and region changes."""

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
import tempfile
import time

from display_capture_test_support import emitter_fixture, validation_log_error
from renderer_mcp_runtime_regression import Client, initialize, wait_for


WIDTH, HEIGHT = 640, 480
TECHNIQUES = ("deferred-raster", "forward-raster")
STATES = ("visible", "covered", "cropped", "hidden", "restored")
MONITOR_EDGE_STATE = "partially-offscreen"
INTENSITIES = (0, 3)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Desktop:
    """Cover or crop the test renderer HWND and verify the resulting desktop state."""

    def __init__(self, window_origin=None, *, monitor_edge=False):
        user32 = ct.WinDLL("user32", use_last_error=True)
        gdi32 = ct.WinDLL("gdi32", use_last_error=True)
        self.window_origin = window_origin
        self.restore_origin = None
        self.states = (*STATES[:-1], MONITOR_EDGE_STATE, STATES[-1]) if monitor_edge else STATES
        self.callback_type = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

        def bind(name, result, *arguments):
            fn = getattr(user32, name)
            fn.restype = result
            fn.argtypes = arguments
            return fn

        dpi = bind("SetThreadDpiAwarenessContext", wt.HANDLE, wt.HANDLE)
        require(dpi(ct.c_void_p(-4)), "Could not enable physical-pixel Win32 coordinates")
        if monitor_edge:
            self.monitor_from_point = bind("MonitorFromPoint", wt.HANDLE, wt.POINT, wt.DWORD)
            monitor_callback = ct.WINFUNCTYPE(wt.BOOL, wt.HANDLE, wt.HDC, ct.POINTER(wt.RECT), wt.LPARAM)
            enumerate_monitors = bind("EnumDisplayMonitors", wt.BOOL, wt.HDC, ct.POINTER(wt.RECT), monitor_callback, wt.LPARAM)
            monitors = []

            @monitor_callback
            def collect_monitor(_monitor, _dc, bounds, _data):
                rect = bounds.contents
                monitors.append((rect.left, rect.top, rect.right, rect.bottom))
                return True

            require(enumerate_monitors(None, None, collect_monitor, 0) and monitors,
                    "Could not enumerate Win32 display monitors")
            self.monitor_right = max(rect[2] for rect in monitors)
            rightmost = [rect for rect in monitors if rect[2] == self.monitor_right
                         and rect[2] - rect[0] >= WIDTH // 2 and rect[3] - rect[1] >= HEIGHT]
            require(rightmost, "The right desktop edge cannot fit half the capture extent")
            _, top, _, bottom = max(rightmost, key=lambda rect: rect[3] - rect[1])
            self.edge_origin = (self.monitor_right - WIDTH // 2, top + (bottom - top - HEIGHT) // 2)
        self.enum_windows = bind("EnumWindows", wt.BOOL, self.callback_type, wt.LPARAM)
        self.window_pid = bind("GetWindowThreadProcessId", wt.DWORD, wt.HWND, ct.POINTER(wt.DWORD))
        self.title = bind("GetWindowTextW", ct.c_int, wt.HWND, wt.LPWSTR, ct.c_int)
        self.window_class = bind("GetClassNameW", ct.c_int, wt.HWND, wt.LPWSTR, ct.c_int)
        self.ancestor = bind("GetAncestor", wt.HWND, wt.HWND, wt.UINT)
        self.client_rect = bind("GetClientRect", wt.BOOL, wt.HWND, ct.POINTER(wt.RECT))
        self.window_rect = bind("GetWindowRect", wt.BOOL, wt.HWND, ct.POINTER(wt.RECT))
        self.to_screen = bind("ClientToScreen", wt.BOOL, wt.HWND, ct.POINTER(wt.POINT))
        self.at_point = bind("WindowFromPoint", wt.HWND, wt.POINT)
        self.is_visible = bind("IsWindowVisible", wt.BOOL, wt.HWND)
        self.is_iconic = bind("IsIconic", wt.BOOL, wt.HWND)
        self.show = bind("ShowWindow", wt.BOOL, wt.HWND, ct.c_int)
        self.set_region = bind("SetWindowRgn", ct.c_int, wt.HWND, wt.HANDLE, wt.BOOL)
        self.region_box = bind("GetWindowRgnBox", ct.c_int, wt.HWND, ct.POINTER(wt.RECT))
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
        self.rect_region = gdi32.CreateRectRgn
        self.rect_region.argtypes = [ct.c_int] * 4
        self.rect_region.restype = wt.HANDLE
        self.delete_object = gdi32.DeleteObject
        self.delete_object.argtypes = [wt.HANDLE]
        self.delete_object.restype = wt.BOOL
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

    def left_half_region(self, hwnd, client_x, client_width):
        outer = wt.RECT()
        require(self.window_rect(hwnd, ct.byref(outer)), "Could not measure the renderer window for clipping")
        # SetWindowRgn coordinates are window-relative, including the border.
        return [0, 0, client_x - outer.left + client_width // 2, outer.bottom - outer.top]

    def owner_details(self, hwnd):
        if not hwnd:
            return None
        owner = wt.DWORD()
        self.window_pid(hwnd, ct.byref(owner))
        name = ct.create_unicode_buffer(128)
        self.window_class(hwnd, name, len(name))
        return {"pid": owner.value, "class": name.value, "visible": bool(self.is_visible(hwnd))}

    def apply(self, hwnd, state):
        x, y, width, height = self.geometry(hwnd)
        require((width, height) == (WIDTH, HEIGHT), "Renderer client size differs from the capture extent")
        if state != "covered":
            self.show(self.cover, 0)
        if state in ("visible", "restored"):
            # Bring only the test window above unrelated windows without activating it.
            # HWND_TOPMOST, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW and optional SWP_NOMOVE.
            origin = self.restore_origin or self.window_origin
            left, top = origin or (0, 0)
            flags = 0x0051 if origin is not None else 0x0053
            require(self.place(hwnd, ct.c_void_p(-1), left, top, 0, 0, flags),
                    "Could not show the test renderer above other windows")
            self.restore_origin = None
        elif state == "hidden":
            self.show(hwnd, 0)  # SW_HIDE leaves the window and swapchain extent intact.
            require(self.set_region(hwnd, None, False), "Could not remove the crop from the hidden renderer window")
        elif state == "covered":
            # HWND_TOPMOST, SWP_NOACTIVATE | SWP_SHOWWINDOW. Cover the entire right half.
            require(self.place(self.cover, ct.c_void_p(-1), x + width // 2, y, width // 2, height, 0x0050),
                    "Could not position the opaque test window")
            self.update(self.cover)
        elif state == "cropped":
            region = self.rect_region(*self.left_half_region(hwnd, x, width))
            require(region, "Could not create a Win32 region that crops the right half")
            if not self.set_region(hwnd, region, True):
                self.delete_object(region)  # On success Windows takes ownership.
                raise RuntimeError("Could not crop the renderer's native client region")
        elif state == MONITOR_EDGE_STATE:
            outer = wt.RECT()
            require(self.window_rect(hwnd, ct.byref(outer)), "Could not measure the test window border")
            self.restore_origin = (outer.left, outer.top)
            edge_x, edge_y = self.edge_origin
            require(self.place(hwnd, ct.c_void_p(-1), edge_x + outer.left - x, edge_y + outer.top - y,
                               0, 0, 0x0051), "Could not move half of the test renderer outside the desktop")
        else:
            raise ValueError(f"Unknown test desktop state {state!r}")
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
        hits = [self.at_point(wt.POINT(*point)) for point in points]
        # WindowFromPoint may return a child inside the renderer or cover.
        owners = [self.ancestor(hit, 2) or hit for hit in hits]  # GA_ROOT
        exposed = owners[:3] == [hwnd] * 3
        rect = wt.RECT()
        region_type = self.region_box(hwnd, ct.byref(rect))
        window_region = [rect.left, rect.top, rect.right, rect.bottom] if region_type else None
        if state in ("visible", "restored"):
            matches = visible and exposed and owners[3:] == [hwnd] * 3
        elif state == "covered":
            matches = visible and exposed and owners[3:] == [self.cover] * 3
        elif state == "cropped":
            matches = (visible and exposed and hwnd not in owners[3:] and self.cover not in owners[3:]
                       and region_type == 2 and window_region == self.left_half_region(hwnd, x, width))
        elif state == MONITOR_EDGE_STATE:
            # WindowFromPoint can still identify a window beyond a monitor's edge.
            monitor_presence = [bool(self.monitor_from_point(wt.POINT(*point), 0)) for point in points]
            matches = (visible and exposed and x + width // 2 == self.monitor_right
                       and monitor_presence == [True] * 3 + [False] * 3)
        else:
            matches = not visible and hwnd not in owners
        if state != "cropped":
            matches = matches and window_region is None
        sample = {"clientRect": [x, y, width, height], "windowVisible": visible,
                  "windowRegion": window_region,
                  "clientProbeOwners": ["renderer" if owner == hwnd else "cover" if owner == self.cover
                                        else "other" for owner in owners]}
        if state == MONITOR_EDGE_STATE:
            sample.update(monitorRight=self.monitor_right, clientProbeOnMonitor=monitor_presence)
        if not matches:
            sample["rendererOwner"] = self.owner_details(hwnd)
            sample["probeOwnerWindows"] = [self.owner_details(owner) for owner in owners]
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


def read_image(path, *, viewport, includes_ui, allow_nonfinite=False):
    import cv2
    import numpy as np
    from PIL import Image

    metadata = json.loads(Path(f"{path}.capture.json").read_bytes())
    require((metadata["width"], metadata["height"], metadata["includesUi"]) == (WIDTH, HEIGHT, includes_ui), metadata)
    if path.suffix == ".exr":
        require(metadata["source"] == "raster-scene-composite" and metadata["colorSpace"] == "linear-sRGB"
                and not metadata["exposureApplied"] and not metadata["toneMappingApplied"], metadata)
        bgr = cv2.imdecode(np.frombuffer(path.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
        require(bgr is not None and bgr.shape == (HEIGHT, WIDTH, 3) and bgr.dtype == np.float32,
                f"Missing float RGB capture: {path}")
        require(allow_nonfinite or np.isfinite(bgr).all(), f"Nonfinite float RGB capture: {path}")
        return bgr[:, :, ::-1], metadata
    require(metadata["source"] == ("viewport" if viewport else "window")
            and metadata["colorSpace"] in ("sRGB", "linear-sRGB"), metadata)
    with Image.open(path) as image:
        require(image.format == "PNG" and image.size == (WIDTH, HEIGHT), f"Wrong PNG dimensions or format: {path}")
        return np.asarray(image.convert("RGBA")), metadata


def capture(client, desktop, hwnd, folder, name, state, *, viewport=False, includes_ui=False,
            allow_nonfinite=False):
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
    image, metadata = read_image(path, viewport=viewport, includes_ui=includes_ui, allow_nonfinite=allow_nonfinite)
    return image, {"image": str(path), "metadata": metadata, "desktop": before,
                   "decodedSha256": hashlib.sha256(image.tobytes()).hexdigest()}


def check_images(images, states):
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
    for state in states[1:]:
        for name in visible:
            np.testing.assert_array_equal(images[state][name], visible[name], err_msg=f"{state} {name}")
    return {"changedRightHalfExrPixels": int(np.count_nonzero(changed)),
            "viewportAndWindowDifferent": True, "allVisibilityCapturesEqual": True}


def native_clipping_pixels(images, production_visible, row, states):
    import numpy as np

    visible = images["visible"]
    for name, reference in production_visible.items():
        np.testing.assert_array_equal(visible[name], reference, err_msg=f"VK_TRUE visible control {name}")

    # Each rectangle describes the affected part in capture-local coordinates.
    # For the native control, an unaffected pixel still has to match exactly.
    regions = {"visible": WIDTH, "covered": WIDTH // 2, "cropped": WIDTH // 2, MONITOR_EDGE_STATE: WIDTH // 2,
               "hidden": 0, "restored": WIDTH}
    dark_exr, bright_exr = (f"radiance-{intensity}.exr" for intensity in INTENSITIES)
    other_exr = {dark_exr: bright_exr, bright_exr: dark_exr}
    reports = []
    row["pixelComparisons"] = reports
    for state in states:
        start = regions[state]
        for name, reference in visible.items():
            sample = images[state][name]
            differs = np.any(sample != reference, axis=-1)
            exposed_count = int(np.count_nonzero(differs[:, :start]))
            affected_count = int(np.count_nonzero(differs[:, start:]))
            result = {"state": state, "kind": name, "referenceState": "visible",
                      "exposedRect": [0, 0, start, HEIGHT], "affectedRect": [start, 0, WIDTH - start, HEIGHT],
                      "changedExposedPixels": exposed_count, "changedAffectedPixels": affected_count}
            if name in other_exr:
                prior = visible[other_exr[name]]
                prior_match = np.all(sample == prior, axis=-1)
                result["nonfiniteAffectedPixels"] = int(np.count_nonzero(~np.isfinite(sample[:, start:]).all(axis=-1)))
                result["affectedPixelsMatchingOtherIntensity"] = int(np.count_nonzero((differs & prior_match)[:, start:]))
                if start < WIDTH:
                    x, y = WIDTH // 2 + 20, HEIGHT // 2
                    result["sentinel"] = {"pixel": [x, y], "expected": reference[y, x].tolist(),
                                          "otherIntensity": prior[y, x].tolist(),
                                          "captured": [float(value) if np.isfinite(value) else None
                                                       for value in sample[y, x]]}
            reports.append(result)
            require(exposed_count == 0, f"VK_TRUE native control changed {exposed_count} exposed pixels in {state} {name}")

    observed = any(item["changedAffectedPixels"] for item in reports)
    row.update(nativeClippingObserved=observed, visibleMatchesProduction=True)


def clipping_provenance(log_text, log, layer_name):
    import re

    for action in ("Insert instance", "Inserted device"):
        require(f'{action} layer "{layer_name}"' in log_text, f"Native clipping control layer missing from {action}: {log}")
    generations = re.findall(r"\[clip-fixture\] swapchain generation=(\d+) requestedClipped=(\d+) "
                             r"effectiveClipped=(\d+) format=(\d+) extent=(\d+)x(\d+) mode=(\w+) startFrame=(\d+)",
                             log_text)
    require(generations and len(generations) == log_text.count("[clip-fixture] swapchain "),
            f"Missing swapchain flag evidence: {log}")
    swapchains = []
    for generation, requested, effective, format_id, width, height, mode, first_frame in generations:
        extent = [int(width), int(height)]
        require((requested, effective, mode, first_frame, extent) == ("0", "1", "visible", "1", [WIDTH, HEIGHT]),
                f"Unexpected native clipping control swapchain in {log}: {generations}")
        swapchains.append({"generation": int(generation), "requestedClipped": False, "effectiveClipped": True,
                           "format": int(format_id), "extent": extent})

    scissors = re.findall(r"\[clip-fixture\] scissor generation=(\d+) mode=(\w+) applied=(\d+) frame=(\d+) "
                          r"x=(-?\d+) y=(-?\d+) width=(\d+) height=(\d+) "
                          r"clippedX=(-?\d+) clippedY=(-?\d+) clippedWidth=(\d+) clippedHeight=(\d+)", log_text)
    require(scissors and len(scissors) == log_text.count("[clip-fixture] scissor "),
            f"Missing scissor pass-through evidence: {log}")
    require(all(mode == "visible" and applied == "0" and rect[:4] == rect[4:]
                for _, mode, applied, _, *rect in scissors), f"Native clipping control mutated a scissor: {log}")
    return {"swapchains": swapchains, "scissorsObserved": len(scissors), "scissorMutations": 0}


def renderer_identity(log_text):
    import re

    gpu = re.search(r"\[Renderer\] \[info\] Vulkan ([\d.]+) on (.+?): dynamic rendering", log_text)
    device = gpu[2] if gpu else None
    driver = next((library for name, library in re.findall(r'Using "(.+?)" with driver: "(.+?)"', log_text)
                   if name == device), None)
    return {"wsi": "Win32", "vulkanApiVersion": gpu[1] if gpu else None, "device": device, "driverLibrary": driver}


def run_technique(renderer, output, technique, desktop, report, *, clipping_control=None, production_visible=None):
    folder = output / "native-clipping-control" / technique if clipping_control else output / technique
    folder.mkdir(parents=True)
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
               VK_LAYER_SETTINGS_PATH=str(output), VK_LOADER_DEBUG="layer", VK_LOADER_LAYERS_DISABLE="~implicit~")
    env.pop("VK_INSTANCE_LAYERS", None)
    env.pop("VK_LOADER_LAYERS_ENABLE", None)
    if clipping_control:
        # The fixture's visible mode forwards every scissor unchanged. Desktop
        # visibility below comes from Win32, independently of that mode name.
        env.update(VK_LOADER_LAYERS_ENABLE=clipping_control["layerName"], CONTAINER_CLIP_TEST_MODE="visible",
                   CONTAINER_CLIP_TEST_FORCE_TRUE="1", CONTAINER_CLIP_TEST_START_FRAME="1")
        search_variable = "VK_LAYER_PATH" if env.get("VK_LAYER_PATH") else "VK_ADD_LAYER_PATH"
        env[search_variable] = os.pathsep.join(filter(None, (env.get(search_variable), clipping_control["layerDir"])))
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
            for state in desktop.states:
                desktop.apply(hwnd, state)
                images[state] = {}
                for intensity in INTENSITIES:
                    setting = client.call("renderer_set_lighting", {"directionalIntensity": intensity})
                    require(setting["lighting"]["directionalIntensity"] == intensity, setting)
                    name = f"radiance-{intensity}.exr"
                    images[state][name], evidence = capture(
                        client, desktop, hwnd, folder, f"{state}-{name}", state,
                        allow_nonfinite=bool(clipping_control) and state not in ("visible", "restored"))
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
    row["renderer"] = renderer_identity(log_text)
    if clipping_control:
        row["clippingProvenance"] = clipping_provenance(log_text, log, clipping_control["layerName"])
        native_clipping_pixels(images, production_visible, row, desktop.states)
        outcome = "observed" if row["nativeClippingObserved"] else "not observed"
        print(f"{technique}: VK_TRUE native clipping {outcome} in {len(row['captures'])} captures", flush=True)
    else:
        require("[clip-fixture]" not in log_text, f"The production run loaded the clipping fixture: {log}")
        row.update(check_images(images, desktop.states))
        print(f"{technique}: {len(row['captures'])} production captures matched across all window states", flush=True)
    return images["visible"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--managed-output", action="store_true",
                        help="write to a fresh run directory under output")
    parser.add_argument("--window-origin", nargs=2, type=int, metavar=("X", "Y"),
                        help="physical screen coordinates for the test window's upper-left corner")
    parser.add_argument("--probe-monitor-edge", action="store_true",
                        help="move half of the test window beyond the desktop's rightmost monitor")
    parser.add_argument("--clipping-control-layer-dir", type=Path,
                        help="run an additional real-window VK_TRUE control using the pass-through fixture")
    parser.add_argument("--require-native-clipping", action="store_true",
                        help="require a pixel loss or stale sample caused by native clipping in the VK_TRUE control")
    args = parser.parse_args()
    if args.require_native_clipping and args.clipping_control_layer_dir is None:
        parser.error("--require-native-clipping requires --clipping-control-layer-dir")
    if os.name != "nt" or os.environ.get("CONTAINER_RUN_GPU_CAPTURE_VISIBILITY") != "1":
        print("SKIP: requires Windows desktop and CONTAINER_RUN_GPU_CAPTURE_VISIBILITY=1")
        return 77
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    renderer, output = args.renderer.resolve(strict=True), args.output.resolve()
    if args.managed_output:
        output.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix="run-", dir=output))
        print(f"Captures: {output}", flush=True)
    else:
        output.mkdir(parents=True)
    (output / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n", encoding="utf-8")
    emitter_fixture(output / "emitter.gltf", {
        "emissiveFactor": [.5, .25, .125],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 8}},
        "pbrMetallicRoughness": {"baseColorFactor": [.45, .4, .35, 1], "metallicFactor": 0, "roughnessFactor": .65},
    })
    renderer_hash = hashlib.sha256(renderer.read_bytes()).hexdigest()
    report = {"rendererSha256": renderer_hash, "windowOrigin": args.window_origin,
              "monitorEdgeRequested": args.probe_monitor_edge, "techniques": [], "passed": False}
    control = None
    if args.clipping_control_layer_dir is not None:
        layer_dir = args.clipping_control_layer_dir.resolve(strict=True)
        manifest = layer_dir / "VkLayer_clip_fixture.json"
        manifest_bytes = manifest.read_bytes()
        layer = json.loads(manifest_bytes)["layer"]
        require(layer["name"] == "VK_LAYER_CONTAINER_clip_fixture", f"Unexpected Vulkan control layer: {manifest}")
        library = layer_dir / layer["library_path"]
        control = {"layerDir": str(layer_dir), "layerName": layer["name"],
                   "manifestSha256": hashlib.sha256(manifest_bytes).hexdigest(),
                   "librarySha256": hashlib.sha256(library.read_bytes()).hexdigest(),
                   "qualificationRequired": args.require_native_clipping, "techniques": []}
        report["nativeClippingControl"] = control
    desktop = Desktop(args.window_origin, monitor_edge=args.probe_monitor_edge)
    try:
        visible = {}
        for technique in TECHNIQUES:
            visible[technique] = run_technique(renderer, output, technique, desktop, report)
        if control is not None:
            for technique in TECHNIQUES:
                run_technique(renderer, output, technique, desktop, control, clipping_control=control,
                              production_visible=visible[technique])
            observed = any(technique["nativeClippingObserved"] for technique in control["techniques"])
            control["nativeClippingObserved"] = observed
            require(not args.require_native_clipping or observed,
                    "The VK_TRUE real-window control observed no native clipping on this WSI/driver")
        require(hashlib.sha256(renderer.read_bytes()).hexdigest() == renderer_hash, "Renderer executable changed during the run")
        report["passed"] = True
    finally:
        desktop.destroy(desktop.cover)
        (output / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
