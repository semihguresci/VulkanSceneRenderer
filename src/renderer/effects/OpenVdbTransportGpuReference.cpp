#include "Container/renderer/effects/OpenVdbTransportGpuReference.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#if CONTAINER_HAS_NANOVDB
#include <nanovdb/NanoVDB.h>
#endif

namespace container::renderer {
namespace {
using namespace geometry::vdb;
using F4 = std::array<float, 4>;
using U4 = std::array<uint32_t, 4>;
using V = TransportVector;
struct alignas(16) Controls {
  U4 counts{}, limits{};
  F4 minimumMajorant{}, maximum{}, environment{};
  U4 random{};
};
struct alignas(16) Medium {
  F4 minimumExtinction{}, maximumG{}, albedo{}, emission{};
  U4 dimensionsFlags{}, offsets{};
  F4 nativeControls{};
};
struct alignas(16) Light {
  F4 vectorType{}, value{};
};
struct alignas(16) Triangle {
  F4 a{}, b{}, c{};
};
struct alignas(16) Ray {
  F4 origin{}, direction{};
};
struct alignas(16) PathResult {
  F4 radianceCosine{};
  U4 counters{}, diagnostics{};
};
static_assert(sizeof(Controls) == 96 && sizeof(Medium) == 112 && sizeof(Light) == 32 &&
              sizeof(Triangle) == 48 && sizeof(Ray) == 32 && sizeof(PathResult) == 48);
constexpr double inf = std::numeric_limits<double>::infinity();
void check(VkResult result, const char *what) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(what);
}
float f32(double value) {
  const float converted = static_cast<float>(value);
  // Do not depend on optional shader denorm preservation / driver FTZ modes.
  if (!std::isfinite(value) || !std::isfinite(converted) ||
      (value != 0 && std::abs(converted) < std::numeric_limits<float>::min()))
    throw std::invalid_argument(
        "GPU transport requires finite normal Float32 values (or exact zero); rescale the scene");
  return converted;
}
double spacing(float x) {
  return std::max(double(std::nextafter(x, std::numeric_limits<float>::infinity())) - x,
                  double(x) - std::nextafter(x, -std::numeric_limits<float>::infinity()));
}
void position(const V &p) {
  for (double x : p)
    if (!std::isfinite(x) || std::abs(x) > 1e12)
      throw std::invalid_argument(
          "GPU transport world coordinates must be finite and at most 1e12");
}
V unit(const V &v) {
  const double n = std::hypot(v[0], v[1], v[2]);
  if (!std::isfinite(n) || n == 0)
    throw std::invalid_argument("GPU transport direction must be finite and nonzero");
  return {v[0] / n, v[1] / n, v[2] / n};
}
F4 vector(const V &v) { return {f32(v[0]), f32(v[1]), f32(v[2]), 0}; }
F4 color(const V &v, double limit = 1e6) {
  for (double x : v)
    if (!std::isfinite(x) || x < 0 || x > limit)
      throw std::invalid_argument("GPU transport color/coefficient out of range");
  return vector(v);
}
uint64_t add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX - a)
    throw std::invalid_argument("GPU transport payload size overflow");
  return a + b;
}
double product(double a, double b) {
  const double p = a * b;
  if (!std::isfinite(p) || (a != 0 && b != 0 && p == 0))
    throw std::runtime_error("GPU transport aggregate dynamic range unsupported");
  return p;
}
struct Moment {
  double scale = 0, mean = 0, m2 = 0;
  void push(double value, uint32_t n) {
    if (value > scale) {
      double r = scale / value;
      mean *= r;
      m2 *= r * r;
      scale = value;
    }
    const double normalized = scale == 0 ? 0 : value / scale, d = normalized - mean;
    mean += d / n;
    m2 += d * (normalized - mean);
  }
  double value() const { return product(mean, scale); }
  double error(uint32_t n) const {
    return n < 2 ? 0 : product(std::sqrt(std::max(0.0, m2) / double(n - 1) / n), scale);
  }
};
struct Prepared {
  Controls controls;
  std::array<Medium, 4> media{};
  std::vector<uint32_t> payload;
  std::vector<Light> lights;
  std::vector<Triangle> triangles;
  std::vector<Ray> rays;
  uint32_t paths{};
};
Prepared prepare(const TransportScene &scene, std::span<const TransportRay> rays,
                 const TransportGpuReferenceOptions &options,
                 const std::array<std::shared_ptr<const TransportGpuNativeSource>, 4> &sources,
                 const VkPhysicalDeviceLimits &limits) {
  const auto &o = options.transport;
  if (o.samplesPerRay < 1 || o.samplesPerRay > 1'000'000 || o.maxScatteringEvents > 16 ||
      o.rouletteStart < 1 || o.rouletteStart > 65 || o.maxWork < 1 || o.maxWork > 1'000'000'000 ||
      o.maxInputBytes < 1 || o.maxDensityValues < 1 || options.maxWorkPerPath < 1 ||
      options.maxWorkPerPath > 65536 || !std::isfinite(o.emissionSamplingRate) ||
      o.emissionSamplingRate < 1e-6 || o.emissionSamplingRate > 1e6)
    throw std::invalid_argument("invalid GPU transport sampling/order/memory/work options");
  if (scene.media.size() > 4 || scene.lights.size() > 32 || scene.occluders.size() > 4096 ||
      rays.empty() || rays.size() > 4096)
    throw std::invalid_argument(
        "GPU transport supports 4 media, 32 lights, 4096 triangles and 1..4096 rays");
  for (size_t i = scene.media.size(); i < 4; ++i)
    if (sources[i])
      throw std::invalid_argument("GPU transport native source has no matching medium");
  const uint64_t paths = uint64_t(rays.size()) * o.samplesPerRay;
  if (paths > 1'000'000)
    throw std::invalid_argument("GPU transport total path count exceeds 1M");
  const uint64_t quota = std::min<uint64_t>(options.maxWorkPerPath, o.maxWork / paths);
  if (quota == 0)
    throw std::invalid_argument("GPU transport total work budget cannot cover all paths");
  if ((paths + 63) / 64 > limits.maxComputeWorkGroupCount[0])
    throw std::invalid_argument("GPU transport dispatch exceeds device limits");
  uint64_t payloadBytes = 0, values = 0, callerBytes = 0;
  for (size_t i = 0; i < scene.media.size(); ++i) {
    const auto &m = scene.media[i];
    callerBytes = add(callerBytes, uint64_t(m.density.size()) * 4);
    if (sources[i]) {
      const auto &native = sources[i]->volume();
      if (native.sparseDensity.empty())
        throw std::invalid_argument("GPU transport native source is empty");
      if (m.dimensions != native.dimensions)
        throw std::invalid_argument(
            "GPU transport native dimensions/bounds must match factory source");
      for (int k = 0; k < 3; ++k)
        if (m.boundsMin[k] != native.boundsMin[k] || m.boundsMax[k] != native.boundsMax[k])
          throw std::invalid_argument(
              "GPU transport native dimensions/bounds must match factory source");
      payloadBytes =
          add((add(payloadBytes, 31) / 32) * 32, uint64_t(native.sparseDensity.size()) * 4);
      callerBytes = add(callerBytes, uint64_t(native.sparseDensity.size()) * 4);
      values = add(values, native.sourceActiveVoxels);
    } else {
      uint64_t count = 1;
      for (uint32_t d : m.dimensions) {
        if (d < 1 || d > 256)
          throw std::invalid_argument("GPU transport dense dimensions must be in [1,256]");
        count *= d;
      }
      if (count != m.density.size())
        throw std::invalid_argument("GPU transport dense dimensions do not match payload");
      payloadBytes = add(payloadBytes, count * 4);
      values = add(values, count);
    }
  }
  if (values > o.maxDensityValues || payloadBytes > UINT32_MAX - 32u ||
      payloadBytes > limits.maxStorageBufferRange)
    throw std::invalid_argument(
        "GPU transport density/payload exceeds configured or device limits");
  const uint64_t resultBytes = paths * sizeof(PathResult);
  if (resultBytes > limits.maxStorageBufferRange)
    throw std::invalid_argument("GPU transport results exceed storage buffer range");
  for (uint64_t bytes : {uint64_t(sizeof(Medium) * 4),
                         uint64_t(std::max<size_t>(1, scene.lights.size()) * sizeof(Light)),
                         uint64_t(std::max<size_t>(1, scene.occluders.size()) * sizeof(Triangle)),
                         uint64_t(rays.size() * sizeof(Ray))})
    if (bytes > limits.maxStorageBufferRange)
      throw std::invalid_argument("GPU transport scene buffer exceeds device storage range");
  // Caller payload + packed CPU/GPU copies, mapped results and CPU estimates.
  // This is a logical live-workspace bound, not a process-RSS guarantee.
  uint64_t workspace =
      add(callerBytes, add(2 * std::max<uint64_t>(payloadBytes, 4), 3 * resultBytes));
  workspace =
      add(workspace, 65536 + scene.occluders.size() * sizeof(TransportTriangle) * 3 +
                         scene.lights.size() * sizeof(TransportLight) * 3 +
                         rays.size() * (sizeof(TransportRay) * 3 + sizeof(TransportRayEstimate)));
  if (workspace > o.maxInputBytes)
    throw std::invalid_argument("GPU transport combined workspace exceeds input-memory budget");
  Prepared p;
  p.paths = static_cast<uint32_t>(paths);
  p.controls.counts = {uint32_t(scene.media.size()), uint32_t(scene.lights.size()),
                       uint32_t(scene.occluders.size()), o.samplesPerRay};
  p.controls.limits = {uint32_t(rays.size()), o.maxScatteringEvents, o.rouletteStart,
                       uint32_t(quota)};
  p.controls.random = {uint32_t(o.seed), uint32_t(o.seed >> 32), 0, 0};
  p.controls.environment = color(scene.environment);
  p.payload.reserve(size_t(std::max<uint64_t>(payloadBytes, 4) / 4));
  double majorant = 0, minStep = inf;
  bool emission = false, hasMedium = false;
  V minimum{inf, inf, inf}, maximum{-inf, -inf, -inf};
  for (size_t i = 0; i < scene.media.size(); ++i) {
    const auto &m = scene.media[i];
    auto &g = p.media[i];
    position(m.boundsMin);
    position(m.boundsMax);
    g.minimumExtinction = vector(m.boundsMin);
    g.maximumG = vector(m.boundsMax);
    g.albedo = color(m.albedo, 1);
    g.emission = color(m.emission);
    if (!std::isfinite(m.extinction) || m.extinction < 0 || m.extinction > 1e6 ||
        !std::isfinite(m.anisotropy) || std::abs(m.anisotropy) > .95)
      throw std::invalid_argument("invalid GPU transport extinction/HG anisotropy");
    g.minimumExtinction[3] = f32(m.extinction);
    g.maximumG[3] = f32(m.anisotropy);
    double maxDensity = 0;
    for (int k = 0; k < 3; ++k) {
      if (!(m.boundsMax[k] > m.boundsMin[k]) || !(g.maximumG[k] > g.minimumExtinction[k]))
        throw std::invalid_argument("GPU transport bounds collapse in Float32");
      const double step = sources[i] ? sources[i]->volume().sparseStepSize
                                     : (m.boundsMax[k] - m.boundsMin[k]) / m.dimensions[k];
      if (!(step > 0) || !std::isfinite(step) ||
          std::max(spacing(g.minimumExtinction[k]), spacing(g.maximumG[k])) > step / 1024)
        throw std::invalid_argument(
            "GPU transport density bounds have insufficient Float32 world precision");
      minStep = std::min(minStep, step);
    }
    g.dimensionsFlags = {m.dimensions[0], m.dimensions[1], m.dimensions[2], sources[i] ? 1u : 0u};
    if (sources[i]) {
#if !CONTAINER_HAS_NANOVDB
      throw std::invalid_argument("GPU transport native support was not enabled");
#else
      const auto &v = sources[i]->volume();
      while (p.payload.size() % 8)
        p.payload.push_back(0);
      g.offsets[0] = uint32_t(p.payload.size() * 4);
      g.nativeControls[0] = v.sparseImportScale;
      p.payload.insert(p.payload.end(), v.sparseDensity.begin(), v.sparseDensity.end());
      maxDensity = v.maxDensity;
#endif
    } else {
      g.offsets[0] = uint32_t(p.payload.size());
      for (float d : m.density) {
        if (!std::isfinite(d) || d < 0 || d > 1e6)
          throw std::invalid_argument("GPU transport density must be finite in [0,1e6]");
        (void)f32(d);
        maxDensity = std::max(maxDensity, double(d));
        p.payload.push_back(std::bit_cast<uint32_t>(d));
      }
    }
    const double extinction = maxDensity * g.minimumExtinction[3];
    if (extinction > 0)
      (void)f32(extinction);
    for (float source : g.emission)
      if (source > 0 && maxDensity > 0)
        (void)f32(maxDensity * source);
    majorant += extinction;
    const bool source =
        maxDensity > 0 && std::max({m.emission[0], m.emission[1], m.emission[2]}) > 0;
    emission |= source;
    if (extinction > 0 || source) {
      hasMedium = true;
      for (int k = 0; k < 3; ++k) {
        minimum[k] = std::min(minimum[k], m.boundsMin[k]);
        maximum[k] = std::max(maximum[k], m.boundsMax[k]);
      }
    }
  }
  if (emission)
    majorant = std::max(majorant, o.emissionSamplingRate);
  if (hasMedium) {
    p.controls.minimumMajorant = vector(minimum);
    p.controls.maximum = vector(maximum);
    float bound = f32(majorant);
    // Upward margin covers interpolation / four-medium addition roundoff.
    for (int n = 0; n < 32; ++n)
      bound = std::nextafter(bound, std::numeric_limits<float>::infinity());
    if (!std::isfinite(bound))
      throw std::invalid_argument("GPU transport majorant cannot be represented");
    p.controls.minimumMajorant[3] = bound;
    for (size_t i = 0; i < scene.media.size(); ++i) {
      const double maximum = sources[i] ? sources[i]->volume().maxDensity
                                        : double(*std::max_element(scene.media[i].density.begin(),
                                                                   scene.media[i].density.end()));
      for (int k = 0; k < 3; ++k)
        if (maximum > 0 && p.media[i].emission[k] > 0)
          (void)f32(maximum * p.media[i].emission[k] / bound);
    }
  }
  const auto coordinate = [&](const V &v) {
    position(v);
    F4 converted = vector(v);
    if (hasMedium)
      for (int k = 0; k < 3; ++k)
        if (spacing(converted[k]) > minStep / 1024 ||
            std::abs(double(converted[k]) - v[k]) > minStep / 1024)
          throw std::invalid_argument(
              "GPU transport world coordinate has insufficient Float32 precision");
    return converted;
  };
  for (const auto &l : scene.lights) {
    Light light;
    light.value = color(l.value);
    if (l.type == TransportLightType::Directional)
      light.vectorType = vector(unit(l.vector));
    else if (l.type == TransportLightType::Point) {
      light.vectorType = coordinate(l.vector);
      light.vectorType[3] = 1;
    } else
      throw std::invalid_argument("unknown GPU transport light type");
    p.lights.push_back(light);
  }
  for (const auto &t : scene.occluders) {
    Triangle tri{coordinate(t.a), coordinate(t.b), coordinate(t.c)};
    V e1{}, e2{};
    for (int k = 0; k < 3; ++k) {
      e1[k] = double(tri.b[k]) - tri.a[k];
      e2[k] = double(tri.c[k]) - tri.a[k];
    }
    const V cross{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                  e1[0] * e2[1] - e1[1] * e2[0]};
    if (std::hypot(cross[0], cross[1], cross[2]) == 0)
      throw std::invalid_argument("GPU transport occluder degenerates in Float32");
    for (double x : cross)
      (void)f32(x);
    p.triangles.push_back(tri);
  }
  for (const auto &ray : rays)
    p.rays.push_back({coordinate(ray.origin), vector(unit(ray.direction))});
  if (p.payload.empty())
    p.payload.push_back(0);
  if (p.lights.empty())
    p.lights.emplace_back();
  if (p.triangles.empty())
    p.triangles.emplace_back();
  return p;
}
struct HostBuffer {
  gpu::VulkanMemoryManager &memory;
  gpu::AllocatedBuffer buffer;
  HostBuffer(gpu::VulkanMemoryManager &m, size_t size, VkBufferUsageFlags usage)
      : memory(m), buffer(m.createBuffer(size, usage, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                         VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                             VMA_ALLOCATION_CREATE_MAPPED_BIT)) {
    if (!buffer.allocation_info.pMappedData) {
      memory.destroyBuffer(buffer);
      throw std::runtime_error("GPU transport mapped allocation unavailable");
    }
  }
  ~HostBuffer() { memory.destroyBuffer(buffer); }
  void upload(const void *data, size_t bytes) {
    std::memcpy(buffer.allocation_info.pMappedData, data, bytes);
    check(vmaFlushAllocation(memory.allocator(), buffer.allocation, 0, VK_WHOLE_SIZE),
          "GPU transport input flush failed");
  }
};
} // namespace

std::shared_ptr<const TransportGpuNativeSource>
LoadTransportGpuNativeSource(const std::filesystem::path &input,
                             const geometry::OpenVdbVolumeOptions &options) {
#if !CONTAINER_HAS_NANOVDB
  (void)input;
  (void)options;
  throw std::runtime_error("GPU transport native support was not enabled");
#else
  auto settings = options;
  settings.sparse = true;
  settings.temperatureGridName.clear();
  auto source = std::shared_ptr<TransportGpuNativeSource>(new TransportGpuNativeSource);
  source->volume_ = geometry::loadOpenVdbVolume(input, settings);
  auto &v = source->volume_;
  if (v.sparseDensity.empty())
    throw std::runtime_error("GPU transport native source is empty");
  const uint64_t bytes = uint64_t(v.sparseDensity.size()) * 4;
  if (bytes > options.maxInputBytes / 2)
    throw std::invalid_argument("GPU transport native validation workspace exceeds memory budget");
  struct alignas(32) Block {
    std::byte bytes[32];
  };
  std::vector<Block> aligned((bytes + 31) / 32);
  std::memcpy(aligned.data(), v.sparseDensity.data(), size_t(bytes));
  const auto &grid = *reinterpret_cast<const nanovdb::FloatGrid *>(aligned.data());
  double maximum = 0;
  const auto density = [&](float d) {
    if (!std::isfinite(d) || d < 0 || d > 1e6)
      throw std::invalid_argument("GPU transport native density invalid");
    (void)f32(d);
    maximum = std::max(maximum, double(d));
  };
  density(grid.tree().root().background());
  for (auto it = grid.tree().root().cbeginValueAll(); it; ++it)
    density(*it);
  const auto nodes = [&]<int level>() {
    auto *node = grid.tree().template getFirstNode<level>();
    for (uint32_t i = 0; i < grid.tree().nodeCount(level); ++i)
      for (auto it = node[i].cbeginValueAll(); it; ++it)
        density(*it);
  };
  nodes.operator()<2>();
  nodes.operator()<1>();
  nodes.operator()<0>();
  v.maxDensity = float(maximum);
  for (uint32_t corner = 0; corner < 8; ++corner) {
    nanovdb::Vec3d world;
    for (int k = 0; k < 3; ++k)
      world[k] =
          double((corner & (1u << k)) ? v.boundsMax[k] : v.boundsMin[k]) / v.sparseImportScale;
    const auto precise = grid.worldToIndex(world);
    const auto index =
        grid.worldToIndexF(nanovdb::Vec3f(float(world[0]), float(world[1]), float(world[2])));
    for (int k = 0; k < 3; ++k)
      if (!std::isfinite(index[k]) || spacing(index[k]) > 1.0 / 1024 ||
          std::abs(double(index[k]) - precise[k]) > 1.0 / 1024)
        throw std::invalid_argument("GPU transport native map has insufficient Float32 index "
                                    "precision; recenter the source");
  }
  return source;
#endif
}

struct OpenVdbTransportGpuReference::Impl {
  TransportGpuDevice context;
  VkPhysicalDeviceProperties properties{};
  VkCommandPool pool{};
  VkDescriptorSetLayout descriptors{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  explicit Impl(const TransportGpuDevice &d) : context(d) {}
  ~Impl() {
    const auto d = context.device;
    if (pipeline)
      vkDestroyPipeline(d, pipeline, nullptr);
    if (layout)
      vkDestroyPipelineLayout(d, layout, nullptr);
    if (descriptors)
      vkDestroyDescriptorSetLayout(d, descriptors, nullptr);
    if (pool)
      vkDestroyCommandPool(d, pool, nullptr);
  }
};
OpenVdbTransportGpuReference::OpenVdbTransportGpuReference(const TransportGpuDevice &d,
                                                           const std::filesystem::path &spirv) {
  if (!d.physicalDevice || !d.device || !d.queue || !d.memory)
    throw std::invalid_argument("GPU transport requires a live device, queue and allocator");
  auto owned = std::make_unique<Impl>(d);
  auto &p = *owned;
  vkGetPhysicalDeviceProperties(d.physicalDevice, &p.properties);
  const auto &l = p.properties.limits;
  if (l.maxPerStageDescriptorStorageBuffers < 6 || l.maxDescriptorSetStorageBuffers < 6 ||
      l.maxPerStageDescriptorUniformBuffers < 1 || l.maxUniformBufferRange < sizeof(Controls) ||
      l.maxComputeWorkGroupInvocations < 64 || l.maxComputeWorkGroupSize[0] < 64)
    throw std::runtime_error(
        "GPU transport requires six storage buffers, one UBO and 64 compute invocations");
  uint32_t familyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(d.physicalDevice, &familyCount, nullptr);
  std::vector<VkQueueFamilyProperties> families(familyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(d.physicalDevice, &familyCount, families.data());
  if (d.queueFamily >= families.size() ||
      !(families[d.queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT))
    throw std::invalid_argument("GPU transport queue family must support compute");
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = d.queueFamily;
  check(vkCreateCommandPool(d.device, &pool, nullptr, &p.pool),
        "GPU transport command pool creation failed");
  std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
  for (uint32_t i = 0; i < bindings.size(); ++i)
    bindings[i] = {i,
                   i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  di.bindingCount = uint32_t(bindings.size());
  di.pBindings = bindings.data();
  check(vkCreateDescriptorSetLayout(d.device, &di, nullptr, &p.descriptors),
        "GPU transport descriptor layout creation failed");
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &p.descriptors;
  check(vkCreatePipelineLayout(d.device, &pl, nullptr, &p.layout),
        "GPU transport pipeline layout creation failed");
  std::ifstream file(spirv, std::ios::binary | std::ios::ate);
  if (!file)
    throw std::invalid_argument("GPU transport SPIR-V file could not be opened");
  const auto end = file.tellg();
  if (end <= 0 || uint64_t(end) > 16 * 1024 * 1024 || uint64_t(end) % 4)
    throw std::invalid_argument("GPU transport SPIR-V size invalid");
  std::vector<uint32_t> code(size_t(end) / 4);
  file.seekg(0);
  file.read(reinterpret_cast<char *>(code.data()), std::streamsize(end));
  if (!file || code.front() != 0x07230203)
    throw std::invalid_argument("GPU transport SPIR-V invalid");
  VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  si.codeSize = code.size() * 4;
  si.pCode = code.data();
  VkShaderModule shader{};
  check(vkCreateShaderModule(d.device, &si, nullptr, &shader),
        "GPU transport shader creation failed");
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.layout = p.layout;
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              nullptr,
              0,
              VK_SHADER_STAGE_COMPUTE_BIT,
              shader,
              "computeMain",
              nullptr};
  const auto status =
      vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline);
  vkDestroyShaderModule(d.device, shader, nullptr);
  check(status, "GPU transport pipeline creation failed");
  impl_ = std::move(owned);
}
OpenVdbTransportGpuReference::~OpenVdbTransportGpuReference() = default;

TransportGpuReferenceResult OpenVdbTransportGpuReference::trace(
    const TransportScene &scene, std::span<const TransportRay> rays,
    const TransportGpuReferenceOptions &options,
    const std::array<std::shared_ptr<const TransportGpuNativeSource>, 4> &sources) {
  auto input = prepare(scene, rays, options, sources, impl_->properties.limits);
  auto &m = *impl_->context.memory;
  const VkDevice device = impl_->context.device;
  HostBuffer control(m, sizeof(Controls), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  HostBuffer mediaBuffer(m, sizeof(input.media), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  HostBuffer payloadBuffer(m, input.payload.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  HostBuffer lightBuffer(m, input.lights.size() * sizeof(Light),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  HostBuffer triangleBuffer(m, input.triangles.size() * sizeof(Triangle),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  HostBuffer rayBuffer(m, input.rays.size() * sizeof(Ray), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  HostBuffer resultBuffer(m, size_t(input.paths) * sizeof(PathResult),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  control.upload(&input.controls, sizeof(input.controls));
  mediaBuffer.upload(input.media.data(), sizeof(input.media));
  payloadBuffer.upload(input.payload.data(), input.payload.size() * 4);
  lightBuffer.upload(input.lights.data(), input.lights.size() * sizeof(Light));
  triangleBuffer.upload(input.triangles.data(), input.triangles.size() * sizeof(Triangle));
  rayBuffer.upload(input.rays.data(), input.rays.size() * sizeof(Ray));
  struct Submission {
    VkDevice device;
    VkCommandPool commandPool;
    VkDescriptorPool descriptors{};
    VkCommandBuffer command{};
    VkFence fence{};
    bool submitted = false, complete = false;
    ~Submission() {
      // On a timeout/error retain resources until this owned submission retires.
      // No queue/device idle is used, and ordinary success has already waited.
      if (submitted && !complete)
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
      if (fence)
        vkDestroyFence(device, fence, nullptr);
      if (command)
        vkFreeCommandBuffers(device, commandPool, 1, &command);
      if (descriptors)
        vkDestroyDescriptorPool(device, descriptors, nullptr);
    }
  } submission{device, impl_->pool};
  const std::array<VkDescriptorPoolSize, 2> sizes{
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 1;
  pool.poolSizeCount = 2;
  pool.pPoolSizes = sizes.data();
  check(vkCreateDescriptorPool(device, &pool, nullptr, &submission.descriptors),
        "GPU transport descriptor pool creation failed");
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = submission.descriptors;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &impl_->descriptors;
  VkDescriptorSet set{};
  check(vkAllocateDescriptorSets(device, &allocate, &set),
        "GPU transport descriptor allocation failed");
  const std::array<VkBuffer, 7> buffers{control.buffer.buffer,        mediaBuffer.buffer.buffer,
                                        payloadBuffer.buffer.buffer,  lightBuffer.buffer.buffer,
                                        triangleBuffer.buffer.buffer, rayBuffer.buffer.buffer,
                                        resultBuffer.buffer.buffer};
  const std::array<VkDeviceSize, 7> bytes{sizeof(Controls),
                                          sizeof(input.media),
                                          input.payload.size() * 4,
                                          input.lights.size() * sizeof(Light),
                                          input.triangles.size() * sizeof(Triangle),
                                          input.rays.size() * sizeof(Ray),
                                          size_t(input.paths) * sizeof(PathResult)};
  std::array<VkDescriptorBufferInfo, 7> infos{};
  std::array<VkWriteDescriptorSet, 7> writes{};
  for (uint32_t i = 0; i < 7; ++i) {
    infos[i] = {buffers[i], 0, bytes[i]};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 nullptr,
                 set,
                 i,
                 0,
                 1,
                 i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 nullptr,
                 &infos[i],
                 nullptr};
  }
  vkUpdateDescriptorSets(device, 7, writes.data(), 0, nullptr);
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = impl_->pool;
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  check(vkAllocateCommandBuffers(device, &ca, &submission.command),
        "GPU transport command allocation failed");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check(vkBeginCommandBuffer(submission.command, &begin), "GPU transport command begin failed");
  VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(submission.command, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
  vkCmdBindPipeline(submission.command, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->pipeline);
  vkCmdBindDescriptorSets(submission.command, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->layout, 0, 1,
                          &set, 0, nullptr);
  vkCmdDispatch(submission.command, (input.paths + 63) / 64, 1, 1);
  VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(submission.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
  check(vkEndCommandBuffer(submission.command), "GPU transport command end failed");
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  check(vkCreateFence(device, &fi, nullptr, &submission.fence),
        "GPU transport fence creation failed");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &submission.command;
  check(vkQueueSubmit(impl_->context.queue, 1, &submit, submission.fence),
        "GPU transport submit failed");
  submission.submitted = true;
  check(vkWaitForFences(device, 1, &submission.fence, VK_TRUE, 30'000'000'000ull),
        "GPU transport submission timed out or failed");
  submission.complete = true;
  check(vmaInvalidateAllocation(m.allocator(), resultBuffer.buffer.allocation, 0, VK_WHOLE_SIZE),
        "GPU transport result invalidate failed");
  const auto *paths =
      static_cast<const PathResult *>(resultBuffer.buffer.allocation_info.pMappedData);
  for (uint32_t i = 0; i < input.paths; ++i) {
    if (paths[i].diagnostics[3] & 1)
      throw std::runtime_error(
          "GPU transport per-path/total work budget exceeded; no partial result returned");
    if (paths[i].diagnostics[3] & 4)
      throw std::runtime_error(
          "GPU transport density exceeded its validated majorant; no partial result returned");
    if (paths[i].diagnostics[3])
      throw std::runtime_error(
          "GPU transport Float32 numeric range/progress failed at path " + std::to_string(i) +
          " (status " + std::to_string(paths[i].diagnostics[3]) + ", shadow proposals " +
          std::to_string(paths[i].counters[3]) + "); no partial result returned");
    for (float value : paths[i].radianceCosine)
      if (!std::isfinite(value))
        throw std::runtime_error("GPU transport returned nonfinite path values");
    for (int k = 0; k < 3; ++k)
      if (paths[i].radianceCosine[k] < 0)
        throw std::runtime_error("GPU transport returned negative radiance");
  }
  TransportGpuReferenceResult output;
  output.paths = input.paths;
  output.rays.reserve(rays.size());
  double cosine = 0;
  for (uint32_t ray = 0; ray < rays.size(); ++ray) {
    std::array<Moment, 3> moments;
    for (uint32_t sample = 0; sample < options.transport.samplesPerRay; ++sample) {
      const auto &p = paths[ray * options.transport.samplesPerRay + sample];
      output.work += p.counters[0];
      output.scatteringEvents += p.counters[1];
      output.nullCollisions += p.counters[2];
      output.shadowProposals += p.counters[3];
      output.truncatedPaths += p.diagnostics[0] & 1u;
      output.shadowRouletteTerminations += p.diagnostics[0] >> 1u;
      output.rouletteTerminations += p.diagnostics[1];
      output.phaseSamples += p.diagnostics[2];
      cosine += p.radianceCosine[3];
      for (int k = 0; k < 3; ++k)
        moments[k].push(p.radianceCosine[k], sample + 1);
    }
    TransportRayEstimate estimate;
    for (int k = 0; k < 3; ++k) {
      estimate.radiance[k] = moments[k].value();
      estimate.standardError[k] = moments[k].error(options.transport.samplesPerRay);
    }
    output.rays.push_back(estimate);
  }
  if (output.work > options.transport.maxWork)
    throw std::runtime_error("GPU transport aggregate work budget exceeded");
  output.meanScatteringCosine = output.phaseSamples ? cosine / double(output.phaseSamples) : 0;
  return output;
}
} // namespace container::renderer
