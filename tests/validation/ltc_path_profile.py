"""Stage and decode the opt-in deferred LTC shader path profile."""

import hashlib
import os
from pathlib import Path
import shutil
import tempfile


PROFILE_SHADERS = (
    "deferred_directional.frag.spv",
    "deferred_directional_no_ddgi.frag.spv",
    "post_process.frag.spv",
)

# Match shaders/ltc_path_profile.slang. A path is present if any emitter or
# BRDF lobe took it for a receiving pixel. Several bits can be set together.
PATH_FLAGS = (
    "coarse_disk",
    "adaptive_disk",
    "subdivided_disk",
    "horizon_clip",
    "sampled_blend",
    "diffuse_fresnel_correction",
    "sampled_fallback",
    "analytic_rectangle",
    "material_residual",
)


def _digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stage_profile_executable(base_exe: Path, shader_dir: Path, output_dir: Path) -> Path:
    """Use a fresh runtime copy and reject mismatched ordinary shaders."""
    base_exe = base_exe.resolve(strict=True)
    shader_dir = shader_dir.resolve(strict=True)
    output_dir = output_dir.resolve()
    base_runtime = base_exe.parent
    asset_dirs = tuple((base_runtime / name).resolve(strict=True)
                       for name in ("spv_shaders", "hdr", "materials"))
    if any(output_dir == source or source in output_dir.parents for source in asset_dirs):
        raise ValueError("the profile output must be outside the runtime asset directories")
    hashes = {}
    for line in (shader_dir / "shader-pairs.sha256").read_text(encoding="ascii").splitlines():
        name, ordinary_sha, profile_sha = line.split()
        if name in hashes:
            raise ValueError(f"duplicate profile shader hash: {name}")
        hashes[name] = ordinary_sha, profile_sha
    if set(hashes) != set(PROFILE_SHADERS):
        raise ValueError("incomplete LTC profile shader hash manifest")
    for name in PROFILE_SHADERS:
        baseline = _digest(asset_dirs[0] / name)
        profile = _digest(shader_dir / f"ltc_profile_{name}")
        if (baseline, profile) != hashes[name] or profile == baseline:
            raise ValueError(f"profile shader does not match the ordinary build: {name}")

    output_dir.mkdir(parents=True, exist_ok=True)
    runtime = Path(tempfile.mkdtemp(prefix="ltc-profile-runtime-", dir=output_dir))
    shutil.copy2(base_exe, runtime / base_exe.name)
    for pattern in ("*.dll", "*.so", "*.so.*", "*.dylib"):
        for library in base_runtime.glob(pattern):
            if library.is_file():
                shutil.copy2(library, runtime / library.name)
    for name, source in zip(("spv_shaders", "hdr", "materials"), asset_dirs):
        shutil.copytree(source, runtime / name)
    for name in PROFILE_SHADERS:
        shutil.copy2(shader_dir / f"ltc_profile_{name}", runtime / "spv_shaders" / name)
    return runtime / base_exe.name


def summarize_path_capture(exr: Path) -> dict:
    """Decode exact integer RGB payloads from the diagnostic EXR."""
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    import cv2
    import numpy as np

    image = cv2.imdecode(np.frombuffer(exr.read_bytes(), dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    if image is None or image.ndim != 3 or image.shape[2] != 3 or not np.isfinite(image).all():
        raise ValueError(f"invalid LTC profile EXR: {exr}")
    zero, marker, payload = np.moveaxis(image, -1, 0)  # OpenCV BGR order
    marked = marker == 1
    if (not marked.any() or not np.all(marked | (marker == 0)) or np.any(zero) or
            np.any(payload[~marked])):
        raise ValueError(f"missing or invalid LTC receiver markers: {exr}")
    bits = payload[marked]
    if not np.all((bits >= 0) & (bits < (1 << len(PATH_FLAGS))) & (bits == np.floor(bits))):
        raise ValueError(f"invalid LTC path mask: {exr}")
    bits = bits.astype(np.uint16)
    receiver_pixels = int(bits.size)
    counts = {
        name: int(np.count_nonzero(bits & (1 << index)))
        for index, name in enumerate(PATH_FLAGS)
    }
    return {
        "imagePixels": int(marked.size),
        "receiverPixels": receiver_pixels,
        "pathPixels": counts,
        "pathFractions": {name: count / receiver_pixels for name, count in counts.items()},
    }
