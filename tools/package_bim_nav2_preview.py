#!/usr/bin/env python3
"""Package the Linux x64 headless BIM-to-Nav2 example from a Release build."""

import argparse
from hashlib import sha256
from io import BytesIO
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile


REPO = Path(__file__).resolve().parents[1]
TOOLS = ("BimRoboticsSim", "BimRoboticsNavMap", "BimRoboticsCollision")
SOURCE_FILES = (
    "LICENSE",
    "docs/bim-robotics-preview.md",
    "tools/bim_nav2_preview.py",
    "tests/robotics/bim_nav2_map.py",
    "tests/robotics/bim_nav2_planner.py",
    "tests/robotics/bim_ros2_bag.py",
    "tests/robotics/bim_ros2_drive_verify.py",
    "tests/robotics/bim_ros2_nav2_demo.py",
    "tests/fixtures/robotics/doorway.ifc",
    "tests/fixtures/robotics/doorway-ros2-drive.json",
    "tests/fixtures/robotics/doorway-ros2-camera.json",
    "tests/fixtures/robotics/room-connectivity.ifc",
    "tests/fixtures/robotics/room-connectivity-metres.ifc",
    "ros2/bim_ros_bridge/package.xml",
    "ros2/bim_ros_bridge/setup.cfg",
    "ros2/bim_ros_bridge/setup.py",
    "ros2/bim_ros_bridge/resource/bim_ros_bridge",
    "ros2/bim_ros_bridge/launch/bridge.launch.py",
    "ros2/bim_ros_bridge/bim_ros_bridge/__init__.py",
    "ros2/bim_ros_bridge/bim_ros_bridge/node.py",
    "ros2/bim_ros_bridge/bim_ros_bridge/transport.py",
)
SYSTEM_LIBRARIES = frozenset({
    "ld-linux-x86-64.so.2", "linux-vdso.so.1", "libc.so.6", "libdl.so.2",
    "libgcc_s.so.1", "libm.so.6", "libpthread.so.0", "librt.so.1", "libstdc++.so.6",
})


def enabled(cache, option):
    value = cache.get(option, "").upper()
    return value not in {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"} and not value.endswith("-NOTFOUND")


def runtime_dependencies(executable):
    with executable.open("rb") as stream:
        header = stream.read(20)
    if not header.startswith(b"\x7fELF\x02\x01") or header[18:20] != b"\x3e\0":
        raise ValueError(f"Expected a Linux x86-64 executable: {executable}")
    result = subprocess.run(["ldd", executable], capture_output=True, text=True, timeout=20)
    diagnostic = (result.stdout + result.stderr).strip()
    if result.returncode or "not found" in diagnostic:
        raise ValueError(f"Cannot resolve runtime dependencies for {executable.name}: {diagnostic}")
    libraries = {Path(line.split()[0]).name for line in diagnostic.splitlines() if line.strip()}
    extra = libraries - SYSTEM_LIBRARIES
    if extra:
        raise ValueError(f"{executable.name} needs {', '.join(sorted(extra))}. "
                         "Build with the static x64-linux vcpkg triplet for this archive.")
    return sorted(libraries)


def payload(build, vcpkg_installed):
    cache = dict(re.findall(r"^([A-Z_0-9]+):\w+=(.*)$", (build / "CMakeCache.txt").read_text(), re.M))
    if cache.get("CMAKE_BUILD_TYPE") != "Release" or not enabled(cache, "ENABLE_PHYSICS"):
        raise ValueError("Use a native Release build with ENABLE_PHYSICS=ON.")

    files = {name: REPO / name for name in SOURCE_FILES}
    files["THIRD_PARTY_NOTICES.md"] = REPO / "docs/third-party-notices.md"
    files.update({f"bin/{name}": build / name for name in TOOLS})
    for notice in ("Apache-2.0.txt", "Boost-1.0.txt", "Clipper2-Boost-1.0.txt",
                   "Earcut-ISC.txt", "Manifold-Apache-2.0.txt"):
        files[f"THIRD_PARTY_LICENSES/{notice}"] = REPO / "cmake/licenses" / notice
    files["THIRD_PARTY_LICENSES/MikkTSpace-bounded-zlib.txt"] = REPO / "src/third_party/mikktspace_bounded/LICENSE.txt"
    notices = build / "third_party_licenses"
    for notice in ("Jolt-MIT.txt", "GLM-1.0.3-LICENSE.txt", "selected-dependencies.json"):
        files[f"THIRD_PARTY_LICENSES/{notice}"] = notices / notice
    for item in json.loads((notices / "selected-dependencies.json").read_bytes())["libraries"]:
        notice = item["notice"]
        source = notices / notice
        if Path(notice).name != notice or sha256(source.read_bytes()).hexdigest() != item["sha256"]:
            raise ValueError(f"Invalid selected dependency notice: {notice}")
        files[f"THIRD_PARTY_LICENSES/{notice}"] = source
    for port in ("mikktspace", "tinyxml2") if enabled(cache, "ENABLE_ROBOTICS_URDF") else ("mikktspace",):
        files[f"THIRD_PARTY_LICENSES/{port}.txt"] = vcpkg_installed / "x64-linux/share" / port / "copyright"
    return files


def package(build, vcpkg_installed, version, output):
    if not sys.platform.startswith("linux"):
        raise ValueError("Create the Nav2 archive from a native Linux shell.")
    files = payload(build, vcpkg_installed)
    missing = [str(path) for path in files.values() if not path.is_file()]
    if missing:
        raise ValueError("Missing preview payload files:\n" + "\n".join(missing))
    dependencies = {name: runtime_dependencies(files[f"bin/{name}"]) for name in TOOLS}
    name = f"VulkanSceneRenderer-{version}-linux-x64-nav2-preview"
    metadata = {
        "version": version,
        "platform": "linux-x64",
        "configuration": "Release",
        "physics_enabled": True,
        "runtime_dependencies": dependencies,
        "runtime_sha256": {relative: sha256(path.read_bytes()).hexdigest() for relative, path in files.items()},
    }
    output.mkdir(parents=True, exist_ok=True)
    archive_name = output / f"{name}.tar.gz"
    stream = archive_name.open("xb")
    try:
        with stream, tarfile.open(fileobj=stream, mode="w:gz") as archive:
            for relative, path in sorted(files.items()):
                member = archive.gettarinfo(str(path.resolve()), arcname=f"{name}/{relative}")
                member.mode = 0o755 if relative.startswith("bin/") else 0o644
                member.uid = member.gid = 0
                member.uname = member.gname = ""
                with path.open("rb") as content:
                    archive.addfile(member, content)
            data = (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode()
            manifest = tarfile.TarInfo(f"{name}/build-info.json")
            manifest.size = len(data)
            archive.addfile(manifest, BytesIO(data))
    except BaseException:
        archive_name.unlink()
        raise
    return archive_name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True, help="CMake Release build directory")
    parser.add_argument("--version", required=True, help="Version used in the archive name")
    parser.add_argument("--output", type=Path, required=True, help="Archive destination directory")
    parser.add_argument("--vcpkg-installed-dir", type=Path, help="Defaults to BUILD_DIR/vcpkg_installed")
    args = parser.parse_args()
    if not re.fullmatch(r"v?\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?", args.version):
        parser.error("--version must look like 0.1.0-preview")
    try:
        archive = package(args.build_dir.resolve(), args.vcpkg_installed_dir or args.build_dir / "vcpkg_installed",
                          args.version, args.output.resolve())
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"Cannot package the Nav2 preview: {error}\n")
    print(f"Created {archive}")
    print(f"SHA256  {sha256(archive.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
