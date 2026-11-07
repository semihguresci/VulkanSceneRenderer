# GPU hardware monitoring

Set `CONTAINER_MONITOR_GPU=1` before launching the viewer, then expand **GPU
hardware sensors** in **Renderer Telemetry**. Monitoring starts only in the
interactive viewer with that environment setting. To include it in capture or
benchmark telemetry, pass `--gpu-hardware-telemetry`. This also works with
`--no-ui`. The panel and capture export share one worker when both are active.
Monitoring is off by default, and scripted captures ignore the interactive
environment setting.

The monitor looks up the selected Vulkan NVIDIA GPU by its PCI domain, bus,
device and function. The Vulkan driver must expose `VK_EXT_pci_bus_info`.
The backend loads NVML from the installed NVIDIA driver on Windows or Linux.
The panel identifies unavailable readings when the GPU, PCI identity, library,
permission or a sensor is unavailable.

NVML reports whole-device utilization as a percentage, driver-accounted memory
as used and total MiB, graphics and memory clocks in MHz, temperature in Celsius
and power in watts. Other processes can contribute to the values. Drivers can
average readings over their own sampling windows. Use the separate Vulkan pass
timings and engine allocation counters for renderer-specific measurements.

One worker initializes and queries NVML. It waits one second after each poll
before starting the next one. The panel shows how long the latest poll took.
Both the UI and capture exporter attempt a single nonblocking snapshot read.
Telemetry with the flag adds a separate `gpuHardware` object with the NVML
backend, device scope, status, PCI address, and optional readings. Its UTC
timestamp is Unix milliseconds at poll completion. Sample age is milliseconds
on the monotonic clock. Poll duration is microseconds of worker time, including
NVML initialization on the first poll. Unavailable readings are `null`. Driver
memory is expressed in bytes and power in milliwatts. GPU pass timings and
renderer allocations remain in their existing fields.

The `gpu_hardware_monitor_tests` suite checks rate limiting, null readings,
timestamp and age serialization, and that a blocked driver call on the worker
leaves the reader available. The headless regression at
`tests/validation/gpu_hardware_telemetry_runtime_regression.py` compares captures
and frame timing with the optional monitor enabled and disabled.

See the [Vulkan PCI address reference](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDevicePCIBusInfoPropertiesEXT.html)
and the [NVIDIA NVML device queries](https://docs.nvidia.com/deploy/nvml-api/latest/api/group__nvmlDeviceQueries.html)
for the device and sensor contracts.
