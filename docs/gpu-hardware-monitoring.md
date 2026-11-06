# GPU hardware monitoring

Set `CONTAINER_MONITOR_GPU=1` before launching the viewer, then expand **GPU
hardware sensors** in **Renderer Telemetry**. Monitoring starts only in the
interactive viewer. It is off by default.

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
The UI attempts a single nonblocking snapshot read. Sensor readings appear in
the interactive panel. Capture telemetry records engine timing and allocations.

See the [Vulkan PCI address reference](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDevicePCIBusInfoPropertiesEXT.html)
and the [NVIDIA NVML device queries](https://docs.nvidia.com/deploy/nvml-api/latest/api/group__nvmlDeviceQueries.html)
for the device and sensor contracts.
