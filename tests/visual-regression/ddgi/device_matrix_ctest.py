"""Execute the registered DDGI CTest commands and preserve their receipts."""

from pathlib import Path
import re
import xml.etree.ElementTree as ET


CPU_TESTS = (
    "ddgi_python_regressions",
    "ddgi_probe_volume_tests", "ddgi_settings_tests", "ddgi_volume_fit_tests",
    "ddgi_scene_bounds_tests", "ddgi_volume_set_tests", "ddgi_priority_scheduler_tests",
    "ddgi_preset_tests", "ddgi_async_history_tests", "ddgi_volume_placement_tests",
    "ddgi_bim_room_plan_tests", "bim_ddgi_rooms_tests", "ddgi_volume_scroll_tests",
    "ddgi_scene_changes_tests", "ray_scene_tests", "render_graph_tests",
    "technique_debug_model_tests", "temporal_capture_tests",
    "deferred_directional_lighting_recorder_tests", "forward_raster_lighting_pass_recorder_tests",
)
SPIRV_TESTS = (
    "ddgi_lookup_spirv_validation", "ddgi_transport_spirv_validation",
    "ray_surface_spirv_validation", "ray_query_contract_spirv_validation",
)
GPU_TESTS = (
    "ddgi_gpu_tests", "ddgi_lookup_gpu_tests", "ddgi_transport_gpu_tests",
    "ddgi_extended_material_gpu_tests", "ddgi_manager_gpu_tests",
    "ray_scene_gpu_tests", "ray_shadow_surface_gpu_tests",
)
VALIDATION_ERROR = re.compile(r"VUID-|SYNC-HAZARD|\bValidation Error\b", re.IGNORECASE)
VALIDATION_INSERTION = re.compile(r'Insert(?:ed)? (instance|device) layer "VK_LAYER_KHRONOS_validation"')


def read_gtest_xml(path):
    cases = []
    for case in ET.parse(path).getroot().iter("testcase"):
        name = f'{case.get("classname")}.{case.get("name")}'
        failure = case.find("failure")
        skipped = case.find("skipped")
        if failure is not None:
            status, reason = "failed", failure.text or failure.get("message")
        elif skipped is not None:
            status, reason = "skipped", skipped.text or skipped.get("message")
        elif case.get("status") == "run" and case.get("result") == "completed":
            status, reason = "passed", None
        else:
            status, reason = "untested", f'GoogleTest recorded {case.get("status")}/{case.get("result")}'
        cases.append(dict(name=name, status=status, reason=reason))
    if not cases:
        raise ValueError(f"No GoogleTest cases in {path}")
    return cases


def run_registered_tests(names, inventory, build, output, invoke, fingerprint, environment, *,
                         allow_skips=False, verify_validation=False):
    """Run CTest's exact commands, with fresh private GoogleTest XML destinations."""
    output.mkdir()
    results = []
    for name in names:
        spec = inventory.get(name)
        if not spec or not spec.get("command"):
            results.append(dict(name=name, status="failed", reason="CTest registration or command is missing"))
            continue
        properties = {row["name"]: row["value"] for row in spec.get("properties", [])}
        directory = Path(properties.get("WORKING_DIRECTORY", build))
        if properties.get("DISABLED") or properties.get("FIXTURES_REQUIRED") or properties.get("ENVIRONMENT_MODIFICATION"):
            results.append(dict(name=name, status="failed", reason="This CTest command requires additional runner semantics"))
            continue
        env = environment.copy()
        for pair in properties.get("ENVIRONMENT", []):
            key, _, value = pair.partition("=")
            env[key] = value

        command = spec["command"].copy()
        xml_path = None
        for index, argument in enumerate(command):
            if argument.startswith("--gtest_output=xml:"):
                xml_path = output / f"{name}.xml"
                command[index] = f"--gtest_output=xml:{xml_path}"
                break
        result = invoke(command, output / f"{name}.log", cwd=directory, environment=env)
        result.update(name=name)
        tool = Path(command[0])
        if tool.is_file():
            result["commandBinary"] = fingerprint(tool)
        for shader in (Path(arg) for arg in command[1:] if arg.endswith(".spv")):
            if shader.is_file():
                result["shader"] = fingerprint(shader)
        if xml_path:
            result["gtestXml"] = str(xml_path)
            try:
                cases = read_gtest_xml(xml_path)
                result["cases"] = cases
            except (OSError, ValueError, ET.ParseError) as error:
                result.update(status="failed", reason=f"Missing or invalid fresh GoogleTest results: {error}")
            else:
                statuses = {row["status"] for row in cases}
                if statuses.intersection({"failed", "untested"}):
                    result.update(status="failed", reason="A required GoogleTest case failed or did not execute")
                elif result["status"] == "passed" and "skipped" in statuses:
                    if allow_skips:
                        result["status"] = "untested" if statuses == {"skipped"} else "partial"
                    else:
                        result.update(status="failed", reason="A required CPU GoogleTest case was skipped")
        if verify_validation:
            log = Path(result["log"]).read_text(encoding="utf-8", errors="replace")
            errors = [line for line in log.splitlines() if VALIDATION_ERROR.search(line)]
            result["validationLayerInsertions"] = sorted(set(VALIDATION_INSERTION.findall(log)))
            result["validationErrorCount"] = len(errors)
            if errors:
                result.update(status="failed", reason="Vulkan validation errors in the contract log",
                              firstValidationErrors=errors[:8])
            elif result["status"] in {"passed", "partial"} and result["validationLayerInsertions"] != ["device", "instance"]:
                result.update(status="failed", reason="The loader did not confirm Khronos validation in the contract")
        results.append(result)
    statuses = {result["status"] for result in results}
    if "failed" in statuses:
        status = "failed"
    elif "partial" in statuses or statuses == {"passed", "untested"}:
        status = "partial"
    elif "passed" in statuses:
        status = "passed"
    else:
        status = "untested"
    return dict(status=status, tests=results)
