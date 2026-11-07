"""Compile the matching UI baseline from a Visual Studio developer shell."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=root / "out/scene-browser-validation/baseline")
    parser.add_argument("--current-ui", action="store_true",
                        help="Use modern UI APIs for a captured pre-packed search index")
    parser.add_argument("--before-interning", action="store_true",
                        help="Label the captured modern UI before model-scoped interning")
    args = parser.parse_args()
    baseline = args.baseline.resolve()
    manifest = json.loads((baseline / "manifest.json").read_text(encoding="utf-8"))
    command = ["cl.exe", "/nologo", "/std:c++latest", "/O2", "/MD", "/EHsc",
               "/permissive-", "/Zc:__cplusplus", "/utf-8", "/DNDEBUG", "/DNOMINMAX",
               "/DWIN32_LEAN_AND_MEAN", "/DGLM_FORCE_COLUMN_MAJOR",
               "/DGLM_FORCE_DEPTH_ZERO_TO_ONE", "/DGLM_FORCE_RADIANS", "/DGLFW_DLL",
               "/DSPDLOG_SHARED_LIB", "/DSPDLOG_COMPILED_LIB", "/DSPDLOG_FMT_EXTERNAL",
               "/DFMT_SHARED", "/DCONTAINER_HAS_TINYUSDZ=1",
               ("/DCONTAINER_SCENE_BROWSER_CURRENT_BASELINE" if args.current_ui or args.before_interning else
                "/DCONTAINER_SCENE_BROWSER_BASELINE")]
    if args.before_interning:
        command += ["/DCONTAINER_SCENE_BROWSER_BEFORE_INTERNING"]
    command += [f"/I{path}" for path in manifest["includeDirectories"]]
    command += [str(root / "tests/benchmarks/scene_browser_benchmark.cpp"),
                f"/Fo{baseline / 'scene_browser_baseline.obj'}", "/link",
                f"/OUT:{baseline / 'scene_browser_baseline.exe'}"]
    command += manifest["libraries"]
    subprocess.run(command, cwd=baseline, check=True)


if __name__ == "__main__":
    main()
