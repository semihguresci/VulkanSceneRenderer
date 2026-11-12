"""Exercise Slang's real include search and the runtime shader cache without Vulkan."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


CMAKE_DIR = Path(__file__).resolve().parents[2] / "cmake"
SHADER_BODY = """RWStructuredBuffer<uint> result;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain() { result[0] = witness(); }
"""
STAGES = ("entry", "unrelated", "module")


def check_reload(root, slangc, cmake):
    baseline_source = root / "baseline source"
    source = root / "edited source"
    baseline_shaders = baseline_source / "shaders"
    shaders = source / "shaders"
    baseline_spirv = root / "baseline spirv"
    interfaces = root / "baseline interfaces"
    direct = root / "direct compile"
    for folder in (baseline_shaders / "subdir", baseline_spirv, interfaces, direct):
        folder.mkdir(parents=True)

    (baseline_shaders / "entry.slang").write_text('#include "subdir/helper.slang"\n' + SHADER_BODY, encoding="utf-8")
    (baseline_shaders / "subdir/helper.slang").write_text('#include "witness.slang"\n', encoding="utf-8")
    (baseline_shaders / "witness.slang").write_text("uint witness() { return 17u; }\n", encoding="utf-8")
    (baseline_shaders / "unrelated.slang").write_text("uint witness() { return 5u; }\n" + SHADER_BODY, encoding="utf-8")
    (baseline_shaders / "module.slang").write_text("import report;\n" + SHADER_BODY, encoding="utf-8")
    (baseline_shaders / "report.slang").write_text("public uint witness() { return 11u; }\n", encoding="utf-8")
    corrupt_module = b"not a Slang module\n"
    (baseline_shaders / "report.slang-module").write_bytes(corrupt_module)
    shutil.copytree(baseline_source, source)

    def run(*command):
        result = subprocess.run([str(arg) for arg in command], cwd=root, capture_output=True,
                                text=True, errors="replace", timeout=20)
        if result.returncode:
            raise RuntimeError(f"Command {command} exited with {result.returncode}\n{result.stdout}{result.stderr}")

    def compiler_flags(shader_root):
        return ["-target", "spirv", "-profile", "spirv_1_6", "-emit-spirv-directly",
                "-fvk-use-entrypoint-name", "-I", shader_root.as_posix(), "-matrix-layout-column-major"]

    def compile_baseline(stage):
        run(slangc, baseline_shaders / f"{stage}.slang", *compiler_flags(baseline_shaders),
            "-entry", "computeMain", "-o", baseline_spirv / f"{stage}.spv", "-depfile", interfaces / f"{stage}.spv.d",
            "-reflection-json", interfaces / f"{stage}.spv.json", "-dump-repro", interfaces / f"{stage}.spv.slang-repro")

    def publish_baseline(stage):
        depfile = interfaces / f"{stage}.spv.d"
        repro = interfaces / f"{stage}.spv.slang-repro"
        run(cmake, f"-DFINGERPRINT_OUTPUT={depfile.as_posix()}.sha256", f"-DDEPFILE={depfile.as_posix()}",
            f"-DSOURCE_ROOT={baseline_source.as_posix()}", f"-DLOOKUP_OUTPUT={depfile.as_posix()}.lookups",
            f"-DLOOKUP_REPRO={repro.as_posix()}", f"-DSLANGC={slangc.as_posix()}",
            "-P", CMAKE_DIR / "ShaderDependencies.cmake")

    for stage in STAGES:
        compile_baseline(stage)
        publish_baseline(stage)
    baseline_outputs = {stage: (baseline_spirv / f"{stage}.spv").read_bytes() for stage in STAGES}

    reload_script = (CMAKE_DIR / "RuntimeShaderReload.cmake.in").read_text(encoding="utf-8")
    settings = {
        "SLANGC_EXECUTABLE": slangc.as_posix(),
        "SLANG_SPIRV_FLAGS": ";".join(compiler_flags(shaders)),
        "RELOAD_INTERFACE_DIR": interfaces.as_posix(),
        "COMPILED_SHADERS_DIR": baseline_spirv.as_posix(),
        "CMAKE_SOURCE_DIR": baseline_source.as_posix(),
        "CMAKE_CURRENT_LIST_DIR": CMAKE_DIR.as_posix(),
    }
    for name, value in settings.items():
        reload_script = reload_script.replace(f"@{name}@", value)
    reload_script = reload_script.replace(f"set(source_root [==[{baseline_source.as_posix()}]==])",
                                          f"set(source_root [==[{source.as_posix()}]==])")
    for stage in STAGES:
        reload_script += f'\ncompile_reload_shader("{shaders.as_posix()}/{stage}.slang" "computeMain" "{stage}.spv")\n'
    script = root / "runtime_shader_reload.cmake"
    script.write_text(reload_script, encoding="utf-8")

    def reload(name, **recompiled):
        candidate = root / name
        candidate.mkdir()
        run(cmake, f"-DRELOAD_DIRECTORY={candidate.as_posix()}", "-P", script)
        compiled = (candidate / "compiled.txt").read_text(encoding="utf-8").splitlines()
        assert compiled == [f"{stage}.spv" for stage in recompiled], (name, compiled, list(recompiled))
        for stage in STAGES:
            expected = recompiled.get(stage, baseline_outputs[stage])
            assert (candidate / "spv_shaders" / f"{stage}.spv").read_bytes() == expected, (name, stage)

    def compile_direct(stage):
        output = direct / f"{stage}.spv"
        run(slangc, shaders / f"{stage}.slang", *compiler_flags(shaders), "-entry", "computeMain", "-o", output)
        return output.read_bytes()

    reload("clean")
    (shaders / "other").mkdir()
    (shaders / "other/witness.slang").write_text("unused source", encoding="utf-8")
    reload("unrelated-file")

    shadow = shaders / "subdir/witness.slang"
    shadow.write_text("uint witness() { return 29u; }\n", encoding="utf-8")
    shadowed_entry = compile_direct("entry")
    assert shadowed_entry != baseline_outputs["entry"], "Direct Slang compilation did not resolve the new include"
    reload("new-shadow", entry=shadowed_entry)
    shadow.unlink()
    reload("restored")

    # Slang reads the invalid binary before falling back to report.slang. That
    # attempted binary is absent from the depfile of the successful build.
    module_depfile = (interfaces / "module.spv.d").read_text(encoding="utf-8")
    assert "report.slang" in module_depfile and "report.slang-module" not in module_depfile
    module_source = direct / "report.slang"
    module_source.write_text("public uint witness() { return 23u; }\n", encoding="utf-8")
    binary_module = shaders / "report.slang-module"
    run(slangc, module_source, "-emit-ir", "-o", binary_module)
    repaired_module = compile_direct("module")
    assert repaired_module != baseline_outputs["module"], "Direct Slang compilation did not use the repaired module"
    reload("repaired-module", module=repaired_module)
    binary_module.write_bytes(corrupt_module)
    reload("restored-module-fallback")

    entry_lookups = interfaces / "entry.spv.d.lookups"
    lookup_metadata = entry_lookups.read_bytes()
    entry_lookups.unlink()
    reload("missing-lookups", entry=baseline_outputs["entry"])
    entry_lookups.write_bytes(lookup_metadata)

    compile_baseline("unrelated")
    # An editor can save between the compiler reading a file and CMake recording
    # the dependency sidecars. The baseline still contains the earlier code.
    edited_shader = "uint witness() { return 37u; }\n" + SHADER_BODY
    for shader_root in (baseline_shaders, shaders):
        (shader_root / "unrelated.slang").write_text(edited_shader, encoding="utf-8")
    publish_baseline("unrelated")
    edited_spirv = compile_direct("unrelated")
    assert edited_spirv != baseline_outputs["unrelated"]
    reload("edited-before-sidecars", unrelated=edited_spirv)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slangc", required=True, type=Path)
    parser.add_argument("--cmake", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="shader lookups ") as folder:
        check_reload(Path(folder).resolve(), args.slangc.resolve(strict=True), args.cmake.resolve(strict=True))
    print("PASS cache reuse, include shadowing, module replacement, sidecar race, and missing metadata")


if __name__ == "__main__":
    main()
