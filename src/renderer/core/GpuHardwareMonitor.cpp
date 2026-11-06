#include "Container/renderer/core/GpuHardwareMonitor.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace container::renderer {
namespace {

using Status = GpuHardwareMonitorStatus;

#if defined(_WIN32) || defined(__linux__)
std::optional<GpuHardwarePciAddress> vulkanPciAddress(VkPhysicalDevice device) {
  uint32_t count{};
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
    return std::nullopt;
  std::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS)
    return std::nullopt;
  if (std::ranges::none_of(extensions, [](const auto& extension) {
        return std::strcmp(extension.extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME) == 0;
      })) {
    return std::nullopt;
  }
  VkPhysicalDevicePCIBusInfoPropertiesEXT pci{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pci};
  vkGetPhysicalDeviceProperties2(device, &properties);
  return GpuHardwarePciAddress{pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction};
}

class NvmlLibrary {
 public:
  NvmlLibrary() {
#if defined(_WIN32)
    module_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_) {
      PWSTR programFiles{};
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles))) {
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> folder(programFiles, &CoTaskMemFree);
        std::wstring path(folder.get());
        path += L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
        module_ = LoadLibraryExW(path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
      }
    }
#else
    module_ = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
  }

  ~NvmlLibrary() {
    if (module_) {
#if defined(_WIN32)
      FreeLibrary(module_);
#else
      dlclose(module_);
#endif
    }
  }

  NvmlLibrary(const NvmlLibrary&) = delete;
  NvmlLibrary& operator=(const NvmlLibrary&) = delete;

  explicit operator bool() const { return module_ != nullptr; }

  template <typename Function>
  Function symbol(const char* name) const {
#if defined(_WIN32)
    return reinterpret_cast<Function>(GetProcAddress(module_, name));
#else
    return reinterpret_cast<Function>(dlsym(module_, name));
#endif
  }

 private:
#if defined(_WIN32)
  HMODULE module_{};
#else
  void* module_{};
#endif
};

// Stable C ABI from NVIDIA's public NVML device-query API. The driver supplies
// it at runtime, so development and packaging do not require the NVML SDK.
struct NvmlDeviceHandle;
using NvmlDevice = NvmlDeviceHandle*;
using NvmlStatus = int;
constexpr NvmlStatus nvmlSuccess = 0;
constexpr unsigned int nvmlTemperatureGpu = 0;
constexpr unsigned int nvmlClockGraphics = 0;
constexpr unsigned int nvmlClockMemory = 2;

struct NvmlUtilization { unsigned int gpu, memory; };
struct NvmlMemory { unsigned long long total, free, used; };

class NvmlReader {
 public:
  explicit NvmlReader(const GpuHardwarePciAddress& pci) : pci_(pci) {
    if (!library_) {
      status_ = Status::LibraryUnavailable;
      return;
    }
    const auto initialize = library_.symbol<NvmlStatus (*)()>("nvmlInit_v2");
    const auto shutdown = library_.symbol<NvmlStatus (*)()>("nvmlShutdown");
    const auto findDevice = library_.symbol<NvmlStatus (*)(const char*, NvmlDevice*)>(
        "nvmlDeviceGetHandleByPciBusId_v2");
    if (!initialize || !shutdown || !findDevice || initialize() != nvmlSuccess) {
      status_ = Status::InitializationFailed;
      return;
    }
    shutdown_ = shutdown;
    char pciId[24]{};
    std::snprintf(pciId, sizeof(pciId), "%08x:%02x:%02x.%x",
                  pci.domain, pci.bus, pci.device, pci.function);
    if (findDevice(pciId, &device_) != nvmlSuccess || !device_) {
      status_ = Status::DeviceUnavailable;
      return;
    }
    utilization_ = library_.symbol<decltype(utilization_)>("nvmlDeviceGetUtilizationRates");
    memory_ = library_.symbol<decltype(memory_)>("nvmlDeviceGetMemoryInfo");
    clock_ = library_.symbol<decltype(clock_)>("nvmlDeviceGetClockInfo");
    temperature_ = library_.symbol<decltype(temperature_)>("nvmlDeviceGetTemperature");
    power_ = library_.symbol<decltype(power_)>("nvmlDeviceGetPowerUsage");
    status_ = Status::Available;
  }

  ~NvmlReader() {
    if (shutdown_) (void)shutdown_();
  }

  GpuHardwareSnapshot poll() const {
    GpuHardwareSnapshot sample{.status = status_, .pci = pci_};
    if (status_ != Status::Available) return sample;

    NvmlUtilization utilization{};
    if (utilization_ && utilization_(device_, &utilization) == nvmlSuccess && utilization.gpu <= 100)
      sample.utilizationPercent = utilization.gpu;

    NvmlMemory memory{};
    if (memory_ && memory_(device_, &memory) == nvmlSuccess && memory.total && memory.used <= memory.total) {
      sample.usedMemoryBytes = memory.used;
      sample.totalMemoryBytes = memory.total;
    }

    unsigned int value{};
    if (clock_ && clock_(device_, nvmlClockGraphics, &value) == nvmlSuccess)
      sample.graphicsClockMHz = value;
    if (clock_ && clock_(device_, nvmlClockMemory, &value) == nvmlSuccess)
      sample.memoryClockMHz = value;
    if (temperature_ && temperature_(device_, nvmlTemperatureGpu, &value) == nvmlSuccess)
      sample.temperatureCelsius = value;
    if (power_ && power_(device_, &value) == nvmlSuccess)
      sample.powerMilliwatts = value;

    if (!sample.utilizationPercent && !sample.usedMemoryBytes && !sample.graphicsClockMHz &&
        !sample.memoryClockMHz && !sample.temperatureCelsius && !sample.powerMilliwatts) {
      sample.status = Status::SensorsUnavailable;
    }
    return sample;
  }

 private:
  NvmlLibrary library_;
  GpuHardwarePciAddress pci_;
  Status status_{Status::Initializing};
  NvmlDevice device_{};
  NvmlStatus (*shutdown_)(){};
  NvmlStatus (*utilization_)(NvmlDevice, NvmlUtilization*){};
  NvmlStatus (*memory_)(NvmlDevice, NvmlMemory*){};
  NvmlStatus (*clock_)(NvmlDevice, unsigned int, unsigned int*){};
  NvmlStatus (*temperature_)(NvmlDevice, unsigned int, unsigned int*){};
  NvmlStatus (*power_)(NvmlDevice, unsigned int*){};
};
#endif

} // namespace

GpuHardwareMonitor::GpuHardwareMonitor(VkPhysicalDevice device, bool enabled) {
  if (!enabled) return;
  snapshot_.status = Status::UnsupportedDevice;
#if defined(_WIN32) || defined(__linux__)
  if (!device) return;
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);
  if (properties.vendorID != 0x10de) return;
  snapshot_.pci = vulkanPciAddress(device);
  if (!snapshot_.pci) {
    snapshot_.status = Status::PciIdentityUnavailable;
    return;
  }
  start([pci = *snapshot_.pci, reader = std::unique_ptr<NvmlReader>{}]() mutable {
    if (!reader) reader = std::make_unique<NvmlReader>(pci);
    return reader->poll();
  });
#else
  (void)device;
#endif
}

GpuHardwareMonitor::GpuHardwareMonitor(PollFunction poll) { start(std::move(poll)); }

void GpuHardwareMonitor::start(PollFunction poll) {
  snapshot_.status = Status::Initializing;
  try {
    worker_ = std::jthread([this, poll = std::move(poll)](std::stop_token stop) mutable {
      std::condition_variable_any wake;
      std::mutex waitMutex;
      while (!stop.stop_requested()) {
        const auto begin = std::chrono::steady_clock::now();
        GpuHardwareSnapshot sample;
        try {
          sample = poll();
        } catch (...) {
          sample.status = Status::WorkerUnavailable;
        }
        sample.sampledAt = std::chrono::steady_clock::now();
        sample.pollMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(
            *sample.sampledAt - begin).count();
        sample.timestampUnixMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        {
          const std::lock_guard lock(snapshotMutex_);
          snapshot_ = sample;
        }
        if (sample.status != Status::Available && sample.status != Status::SensorsUnavailable) return;
        std::unique_lock lock(waitMutex);
        wake.wait_for(lock, stop, pollInterval, [] { return false; });
      }
    });
  } catch (const std::system_error&) {
    snapshot_.status = Status::WorkerUnavailable;
  }
}

std::optional<GpuHardwareSnapshot> GpuHardwareMonitor::trySnapshot() const {
  const std::unique_lock lock(snapshotMutex_, std::try_to_lock);
  if (!lock.owns_lock()) return std::nullopt;
  return snapshot_;
}

std::string_view gpuHardwareMonitorStatusText(GpuHardwareMonitorStatus status) {
  switch (status) {
    case Status::Disabled: return "Disabled. Set CONTAINER_MONITOR_GPU=1 or use --gpu-hardware-telemetry.";
    case Status::Initializing: return "Initializing NVML on the worker.";
    case Status::UnsupportedDevice: return "NVML monitoring is unavailable on this platform or GPU.";
    case Status::PciIdentityUnavailable: return "Vulkan did not expose the GPU's PCI address.";
    case Status::LibraryUnavailable: return "The NVIDIA driver NVML library is unavailable.";
    case Status::InitializationFailed: return "NVML initialization failed or its driver API is incomplete.";
    case Status::DeviceUnavailable: return "NVML could not access the Vulkan GPU at its PCI address.";
    case Status::SensorsUnavailable: return "NVML opened this GPU. Its sensor readings are unavailable.";
    case Status::Available: return "Device-wide NVML readings. Poll interval is one second.";
    case Status::WorkerUnavailable: return "The hardware monitor worker is unavailable.";
  }
  return "Hardware monitor status unavailable.";
}

nlohmann::json gpuHardwareTelemetry(const std::optional<GpuHardwareSnapshot>& snapshot,
                                    std::chrono::steady_clock::time_point now) {
  using Json = nlohmann::json;
  constexpr std::array statusNames{"disabled", "initializing", "unsupported-device", "pci-unavailable",
      "library-unavailable", "initialization-failed", "device-unavailable", "sensors-unavailable",
      "available", "worker-unavailable"};
  const auto value = [](const auto& optional) { return optional ? Json(*optional) : Json(nullptr); };
  const auto sample = snapshot.value_or(GpuHardwareSnapshot{});
  const auto age = sample.sampledAt ? std::max(int64_t{0}, std::chrono::duration_cast<std::chrono::milliseconds>(
      now - *sample.sampledAt).count()) : 0;
  Json pci = nullptr;
  if (sample.pci) {
    const auto& address = *sample.pci;
    pci = {{"domain", address.domain}, {"bus", address.bus}, {"device", address.device}, {"function", address.function}};
  }
  return {{"backend", "nvml"}, {"scope", "device"},
      {"status", snapshot ? statusNames.at(static_cast<size_t>(sample.status)) : "publishing"},
      {"pci", std::move(pci)},
      {"sampleUnixMilliseconds", sample.sampledAt ? Json(sample.timestampUnixMilliseconds) : Json(nullptr)},
      {"sampleAgeMilliseconds", sample.sampledAt ? Json(age) : Json(nullptr)},
      {"pollMicroseconds", sample.sampledAt ? Json(sample.pollMicroseconds) : Json(nullptr)},
      {"utilizationPercent", value(sample.utilizationPercent)},
      {"driverUsedMemoryBytes", value(sample.usedMemoryBytes)}, {"driverTotalMemoryBytes", value(sample.totalMemoryBytes)},
      {"graphicsClockMHz", value(sample.graphicsClockMHz)}, {"memoryClockMHz", value(sample.memoryClockMHz)},
      {"temperatureCelsius", value(sample.temperatureCelsius)}, {"powerMilliwatts", value(sample.powerMilliwatts)}};
}

} // namespace container::renderer
