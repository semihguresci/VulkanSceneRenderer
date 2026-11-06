"""Save matching pre-change headers and built Release libraries for UI timings.

Run before rebuilding the application. This preserves the active checkout and
uses the existing Visual Studio test project's dependency list for linking.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET
import zipfile


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", default="cb8406a937ce1223e9d7239e9fbe1532d2c05d0c")
    parser.add_argument("--build-dir", type=Path, default=root / "out/build/visual-studio")
    parser.add_argument("--output", type=Path, default=root / "out/scene-browser-validation/baseline")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    destination = args.output.resolve()
    commit = subprocess.check_output(["git", "rev-parse", "--verify", "--end-of-options",
                                      f"{args.ref}^{{commit}}"], cwd=root, text=True).strip()
    archive = subprocess.check_output(["git", "archive", "--format=zip", commit, "include"], cwd=root)
    project = ET.parse(build / "technique_debug_model_tests.vcxproj")
    ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
    group = next(group for group in project.findall("m:ItemDefinitionGroup", ns)
                 if "'Release|x64'" in group.attrib.get("Condition", ""))
    dependencies = group.find("m:Link/m:AdditionalDependencies", ns).text.split(";")
    libraries = []
    for item in dependencies:
        if not item or item.startswith("%") or Path(item).name in ("gtest.lib", "gtest_main.lib"):
            continue
        path = Path(item)
        if not path.is_absolute():
            candidate = build / path
            if not candidate.is_file():
                libraries.append(item)  # Windows SDK import library.
                continue
            path = candidate
        if path.is_relative_to(build):
            target = destination / "libraries" / path.relative_to(build)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            path = target
        libraries.append(str(path))
    with zipfile.ZipFile(io.BytesIO(archive)) as contents:
        for member in contents.infolist():
            target = destination / member.filename
            if not target.resolve().is_relative_to(destination):
                raise ValueError("Archive member is outside the output directory")
            if member.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(contents.read(member))
    # Explicit dependency includes avoid accidentally selecting the live headers.
    dependency_include = root / "out/build/windows-release/vcpkg_installed/x64-windows/include"
    include_dirs = [str(destination / "include"), str(dependency_include),
                    str(dependency_include / "vma"), str(dependency_include / "miniz"),
                    str(root / "out/build/windows-release/_deps/tinyusdz-src/src")]
    manifest = {"baselineCommit": commit, "includeDirectories": include_dirs,
                "libraries": libraries,
                "copiedLibrarySha256": {str(path.relative_to(destination)):
                    hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in sorted((destination / "libraries").rglob("*.lib"))}}
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Captured {len(manifest['copiedLibrarySha256'])} Release libraries and headers from {commit}")


if __name__ == "__main__":
    main()
