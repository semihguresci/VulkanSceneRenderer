from collections import Counter
import json
from pathlib import Path
import tempfile
import unittest

from device_matrix_capabilities import match_capture_device, parse_vulkaninfo, select_device
from device_matrix_ctest import run_registered_tests
from run_device_matrix import fingerprint, matrix_status, receipt


GPU = """GPU0:
VkPhysicalDeviceProperties:
    apiVersion = 1.4.5 (4210693)
    driverVersion = 7.8.9 (12345)
    vendorID = 0x1002
    deviceID = 0x1234
    deviceName = Example Adapter
VkPhysicalDeviceLimits:
    maxPerStageDescriptorSamplers = 32
VkPhysicalDeviceIDProperties:
    deviceUUID = 01234567-89ab-cdef-0123-456789abcdef
VkPhysicalDeviceVulkan12Features:
    timelineSemaphore = true
VkPhysicalDeviceRayQueryFeaturesKHR:
    rayQuery = false
VkQueueFamilyProperties:
    queueProperties[0]:
        queueCount = 2
        queueFlags = QUEUE_GRAPHICS_BIT | QUEUE_COMPUTE_BIT
        timestampValidBits = 64
Format Properties:
    FORMAT_R16G16B16A16_SFLOAT:
        linearTilingFeatures:
            FORMAT_FEATURE_SAMPLED_IMAGE_BIT
        optimalTilingFeatures:
            FORMAT_FEATURE_STORAGE_IMAGE_BIT
        bufferFeatures: count = 0
"""

TELEMETRY = {"gpu": "Example Adapter", "vendorId": 0x1002, "deviceId": 0x1234,
             "driverVersion": 12345, "vulkanApiVersion": 4210693,
             "validationEnabled": True, "build": {"configuration": "Release", "revision": "verified"}}


class DeviceMatrixTests(unittest.TestCase):
    def test_full_inventory_retains_limits_formats_and_queue_caps(self):
        device = select_device(parse_vulkaninfo(GPU), "01234567-89ab-cdef-0123-456789abcdef")
        self.assertEqual(device["identity"]["raw"]["driverVersion"], "7.8.9 (12345)")
        self.assertEqual(device["limits"]["maxPerStageDescriptorSamplers"], 32)
        self.assertEqual(device["queueFamilies"][0]["queueCount"], 2)
        self.assertTrue(device["features"]["timelineSemaphore"])
        self.assertFalse(device["features"]["rayQuery"])
        self.assertEqual(device["formats"]["VK_FORMAT_R16G16B16A16_SFLOAT"]["optimalTilingFeatures"],
                         ["FORMAT_FEATURE_STORAGE_IMAGE_BIT"])

    def test_explicit_index_does_not_hide_an_indistinguishable_device(self):
        second = GPU.replace("GPU0:", "GPU1:").replace("01234567-89ab-cdef-0123-456789abcdef",
                                                     "01234567-89ab-cdef-0123-456789abcdee")
        inventory = parse_vulkaninfo(GPU + second)
        with self.assertRaises(ValueError):
            match_capture_device(inventory, TELEMETRY, intended_device=0)
        unique = dict(TELEMETRY, deviceUUID="01234567-89ab-cdef-0123-456789abcdef")
        self.assertEqual(match_capture_device(inventory, unique, intended_device=0)["deviceIndex"], 0)

    def test_explicit_index_rejects_another_devices_capture(self):
        other = GPU.replace("GPU0:", "GPU1:").replace("0x1234", "0x1235").replace("Example Adapter", "Other Adapter")
        with self.assertRaises(ValueError):
            match_capture_device(parse_vulkaninfo(GPU + other), TELEMETRY, intended_device=1)

    def test_lane_must_keep_its_validation_mode_and_build(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frame.json"
            path.write_text(json.dumps(TELEMETRY), encoding="utf-8")
            result = {"passed": True, "captures": [str(path)]}
            inventory = parse_vulkaninfo(GPU)
            self.assertEqual(receipt(result, inventory, 0, True, TELEMETRY["build"])["matchedCaptures"], 1)
            self.assertEqual(receipt(result, inventory, 0, False, TELEMETRY["build"])["status"], "failed")
            self.assertEqual(receipt(result, inventory, 0, True, {"revision": "another"})["status"], "failed")

    def test_default_gtest_success_does_not_count_all_skips_as_executed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            old_xml = root / "build.xml"
            old_xml.write_text("existing result", encoding="utf-8")
            inventory = {"gpu-contract": {"command": ["contract", f"--gtest_output=xml:{old_xml}"]}}

            def invoke(command, log, **_):
                report = Path(command[-1].removeprefix("--gtest_output=xml:"))
                report.write_text('<testsuites><testsuite><testcase name="Query" classname="Gpu">'
                                  '<skipped>ray queries unavailable</skipped></testcase></testsuite></testsuites>',
                                  encoding="utf-8")
                return {"command": command, "log": str(log), "status": "passed", "exitCode": 0}

            gpu = run_registered_tests(inventory, inventory, root, root / "gpu", invoke, fingerprint, {},
                                       allow_skips=True)
            cpu = run_registered_tests(inventory, inventory, root, root / "cpu", invoke, fingerprint, {})
            self.assertEqual(gpu["status"], "untested")
            self.assertEqual(gpu["tests"][0]["cases"][0]["reason"], "ray queries unavailable")
            self.assertEqual(cpu["status"], "failed")
            self.assertEqual(old_xml.read_text(encoding="utf-8"), "existing result")

    def test_gpu_receipt_requires_layer_insertion_and_a_clean_log(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inventory = {"gpu-contract": {"command": ["contract", "--gtest_output=xml:original.xml"]}}
            layer_log = ('Insert instance layer "VK_LAYER_KHRONOS_validation"\n'
                         'Inserted device layer "VK_LAYER_KHRONOS_validation"\n')
            for label, log_contents, expected in (("clean", layer_log, "passed"),
                                                  ("missing-layer", "", "failed"),
                                                  ("hazard", layer_log + "SYNC-HAZARD-WRITE-AFTER-READ", "failed")):
                with self.subTest(label=label):
                    def invoke(command, log, **_):
                        report = Path(command[-1].removeprefix("--gtest_output=xml:"))
                        report.write_text('<testsuites><testsuite><testcase name="Trace" classname="Gpu" '
                                          'status="run" result="completed" /></testsuite></testsuites>',
                                          encoding="utf-8")
                        log.write_text(log_contents, encoding="utf-8")
                        return {"command": command, "log": str(log), "status": "passed", "exitCode": 0}

                    result = run_registered_tests(inventory, inventory, root, root / label, invoke,
                                                  fingerprint, {}, verify_validation=True)
                    self.assertEqual(result["status"], expected)

    def test_unsupported_timing_and_mixed_coverage_are_explicit(self):
        phases = Counter(unsupported=1)
        self.assertEqual(matrix_status("timing", phases, None), "unsupported")
        self.assertEqual(matrix_status("all", Counter(passed=3, unsupported=1), None), "partial")
        self.assertEqual(matrix_status("all", Counter(passed=3, failed=1), None), "failed")


if __name__ == "__main__":
    unittest.main()
