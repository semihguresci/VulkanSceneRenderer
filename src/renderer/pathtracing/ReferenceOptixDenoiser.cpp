#include <Container/renderer/pathtracing/ReferenceOptixDenoiser.h>

#include <Windows.h>
#include <cuda.h>
#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

static_assert(OPTIX_VERSION == 90100, "This backend requires the pinned OptiX 9.1.0 headers");

namespace container::renderer {
namespace {

void checkCuda(CUresult result, const char* operation) {
  if (result != CUDA_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed (CUDA " + std::to_string(result) + ")");
}

void checkOptix(OptixResult result, const char* operation) {
  if (result != OPTIX_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed (OptiX " + std::to_string(result) + ")");
}

// Resolve only the stable driver entry points. An OptiX-enabled executable can
// start on machines without an NVIDIA driver or the CUDA Toolkit.
#define REFERENCE_CUDA_FUNCTIONS(X) \
  X(cuInit) X(cuDeviceGetCount) X(cuDeviceGet) X(cuDeviceGetUuid) X(cuDeviceGetAttribute) \
  X(cuDevicePrimaryCtxRetain) X(cuDevicePrimaryCtxRelease) X(cuCtxPushCurrent) X(cuCtxPopCurrent) \
  X(cuStreamCreate) X(cuStreamDestroy) X(cuStreamSynchronize) \
  X(cuEventCreate) X(cuEventDestroy) X(cuEventRecord) X(cuEventElapsedTime) \
  X(cuMemAlloc) X(cuMemFree) X(cuMemcpyHtoD) X(cuMemcpyDtoH)
#define CUDA_STRING_INNER(name) #name
#define CUDA_STRING(name) CUDA_STRING_INNER(name)

class Driver {
 public:
  Driver() {
    module = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) throw std::runtime_error("NVIDIA CUDA driver nvcuda.dll is unavailable");
    try {
#define LOAD_CUDA(name) \
      p_##name = reinterpret_cast<decltype(p_##name)>(GetProcAddress(module, CUDA_STRING(name))); \
      if (!p_##name) throw std::runtime_error("NVIDIA CUDA driver is missing " CUDA_STRING(name));
      REFERENCE_CUDA_FUNCTIONS(LOAD_CUDA)
#undef LOAD_CUDA
      checkCuda(p_cuInit(0), "CUDA driver initialization");
      checkOptix(optixInitWithHandle(&optixModule), "OptiX 9.1 driver initialization (requires NVIDIA R590 or newer)");
    } catch (...) {
      if (optixModule) optixUninitWithHandle(optixModule);
      FreeLibrary(module);
      throw;
    }
  }
  ~Driver() {
    if (optixModule) optixUninitWithHandle(optixModule);
    FreeLibrary(module);
  }

#define DECLARE_CUDA(name) decltype(&::name) p_##name{};
  REFERENCE_CUDA_FUNCTIONS(DECLARE_CUDA)
#undef DECLARE_CUDA
 private:
  HMODULE module{};
  void* optixModule{};
};

#undef CUDA_STRING
#undef CUDA_STRING_INNER
#undef REFERENCE_CUDA_FUNCTIONS

Driver& driver() {
  static Driver instance;
  return instance;
}

struct CurrentContext {
  const Driver& cuda;
  explicit CurrentContext(const Driver& cuda, CUcontext context) : cuda(cuda) {
    checkCuda(cuda.p_cuCtxPushCurrent(context), "CUDA context activation");
  }
  ~CurrentContext() {
    CUcontext popped{};
    cuda.p_cuCtxPopCurrent(&popped);
  }
};

} // namespace

struct ReferenceOptixDenoiser::Impl {
  Driver& cuda = driver();
  CUdevice device{};
  CUcontext context{};
  CUstream stream{};
  CUevent start{}, end{};
  OptixDeviceContext optix{};
  OptixDenoiser denoiser{};
  CUdeviceptr state{}, scratch{}, intensity{};
  std::array<CUdeviceptr, 4> pixels{};
  size_t imageBytes{};
  uint32_t width{}, height{};
  OptixDenoiserSizes sizes{};

  ~Impl() {
    if (!context) return;
    if (cuda.p_cuCtxPushCurrent(context) == CUDA_SUCCESS) {
      if (stream) cuda.p_cuStreamSynchronize(stream);
      if (denoiser) optixDenoiserDestroy(denoiser);
      if (optix) optixDeviceContextDestroy(optix);
      for (auto ptr : pixels) if (ptr) cuda.p_cuMemFree(ptr);
      if (intensity) cuda.p_cuMemFree(intensity);
      if (scratch) cuda.p_cuMemFree(scratch);
      if (state) cuda.p_cuMemFree(state);
      if (end) cuda.p_cuEventDestroy(end);
      if (start) cuda.p_cuEventDestroy(start);
      if (stream) cuda.p_cuStreamDestroy(stream);
      CUcontext popped{};
      cuda.p_cuCtxPopCurrent(&popped);
    }
    cuda.p_cuDevicePrimaryCtxRelease(device);
  }

  void create(const std::array<uint8_t, 16>& uuid, uint32_t w, uint32_t h) {
    constexpr uint32_t pixelBytes = sizeof(float) * 4;
    if (!w || !h || w > std::numeric_limits<uint32_t>::max() / pixelBytes ||
        size_t(w) * pixelBytes > std::numeric_limits<size_t>::max() / h)
      throw std::invalid_argument("invalid OptiX denoiser extent");
    width = w;
    height = h;
    imageBytes = size_t(width) * height * pixelBytes;

    int deviceCount{};
    checkCuda(cuda.p_cuDeviceGetCount(&deviceCount), "CUDA adapter enumeration");
    bool matched = false;
    for (int i = 0; i < deviceCount; ++i) {
      CUuuid candidate{};
      checkCuda(cuda.p_cuDeviceGet(&device, i), "CUDA adapter query");
      checkCuda(cuda.p_cuDeviceGetUuid(&candidate, device), "CUDA adapter UUID query");
      if (!std::equal(uuid.begin(), uuid.end(), reinterpret_cast<const uint8_t*>(candidate.bytes))) continue;
      matched = true;
      break;
    }
    if (!matched) throw std::runtime_error("no CUDA adapter matches the Vulkan physical device UUID");
    int major{}, minor{};
    checkCuda(cuda.p_cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device), "CUDA architecture query");
    checkCuda(cuda.p_cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device), "CUDA architecture query");
    if (major < 7 || (major == 7 && minor < 5))
      throw std::runtime_error("OptiX 9.1 requires a Turing (compute capability 7.5) or newer NVIDIA GPU");
    checkCuda(cuda.p_cuDevicePrimaryCtxRetain(&context, device), "CUDA primary context acquisition");
    const CurrentContext current(cuda, context);
    // Order the denoiser after the driver's synchronous host uploads on the
    // legacy default stream, including the pageable-host staging case.
    checkCuda(cuda.p_cuStreamCreate(&stream, CU_STREAM_DEFAULT), "CUDA denoiser stream creation");
    checkCuda(cuda.p_cuEventCreate(&start, CU_EVENT_DEFAULT), "CUDA start timestamp allocation");
    checkCuda(cuda.p_cuEventCreate(&end, CU_EVENT_DEFAULT), "CUDA end timestamp allocation");
    const OptixDeviceContextOptions contextOptions{};
    checkOptix(optixDeviceContextCreate(context, &contextOptions, &optix), "OptiX device context creation");
    const OptixDenoiserOptions options{.guideAlbedo = 1, .guideNormal = 1};
    checkOptix(optixDenoiserCreate(optix, OPTIX_DENOISER_MODEL_KIND_HDR, &options, &denoiser),
        "OptiX guided HDR denoiser creation");
    checkOptix(optixDenoiserComputeMemoryResources(denoiser, width, height, &sizes), "OptiX working memory query");
    checkCuda(cuda.p_cuMemAlloc(&state, sizes.stateSizeInBytes), "CUDA denoiser state allocation");
    checkCuda(cuda.p_cuMemAlloc(&scratch, sizes.withoutOverlapScratchSizeInBytes), "CUDA scratch allocation");
    checkCuda(cuda.p_cuMemAlloc(&intensity, sizeof(float)), "CUDA intensity allocation");
    for (size_t i = 0; i < pixels.size(); ++i)
      checkCuda(cuda.p_cuMemAlloc(&pixels[i], i == 2 ? imageBytes / 2 : imageBytes), "CUDA denoiser image allocation");
    checkOptix(optixDenoiserSetup(denoiser, stream, width, height, state, sizes.stateSizeInBytes,
        scratch, sizes.withoutOverlapScratchSizeInBytes), "OptiX denoiser setup");
    checkCuda(cuda.p_cuStreamSynchronize(stream), "CUDA denoiser setup synchronization");
  }

  [[nodiscard]] OptixImage2D image(CUdeviceptr data) const {
    constexpr uint32_t pixelBytes = sizeof(float) * 4;
    return {data, width, height, width * pixelBytes, pixelBytes, OPTIX_PIXEL_FORMAT_FLOAT4};
  }
};

ReferenceOptixDenoiser::ReferenceOptixDenoiser(const std::array<uint8_t, 16>& uuid, uint32_t width, uint32_t height)
    : impl_(std::make_unique<Impl>()) {
  impl_->create(uuid, width, height);
}

ReferenceOptixDenoiser::~ReferenceOptixDenoiser() = default;

double ReferenceOptixDenoiser::denoise(std::span<const float> color, std::span<const float> albedo,
    std::span<const float> normal, std::span<float> output) {
  auto& s = *impl_;
  if (color.size_bytes() != s.imageBytes || albedo.size_bytes() != s.imageBytes ||
      normal.size_bytes() != s.imageBytes / 2 || output.size_bytes() != s.imageBytes)
    throw std::invalid_argument("OptiX denoiser image size differs from the configured extent");
  const CurrentContext current(s.cuda, s.context);
  for (const auto& [i, data] : {std::pair{0u, color}, std::pair{1u, albedo}, std::pair{2u, normal}})
    checkCuda(s.cuda.p_cuMemcpyHtoD(s.pixels[i], data.data(), data.size_bytes()), "CUDA guide or radiance upload");
  constexpr uint32_t normalBytes = sizeof(float) * 2;
  const OptixImage2D normalImage{s.pixels[2], s.width, s.height, s.width * normalBytes,
      normalBytes, OPTIX_PIXEL_FORMAT_FLOAT2};
  const OptixDenoiserGuideLayer guides{.albedo = s.image(s.pixels[1]), .normal = normalImage};
  const OptixDenoiserLayer layer{.input = s.image(s.pixels[0]), .output = s.image(s.pixels[3])};
  const OptixDenoiserParams params{.hdrIntensity = s.intensity};
  checkCuda(s.cuda.p_cuEventRecord(s.start, s.stream), "CUDA start timestamp recording");
  checkOptix(optixDenoiserComputeIntensity(s.denoiser, s.stream, &layer.input, s.intensity,
      s.scratch, s.sizes.withoutOverlapScratchSizeInBytes), "OptiX HDR intensity estimation");
  checkOptix(optixDenoiserInvoke(s.denoiser, s.stream, &params, s.state, s.sizes.stateSizeInBytes,
      &guides, &layer, 1, 0, 0, s.scratch, s.sizes.withoutOverlapScratchSizeInBytes), "OptiX denoiser dispatch");
  checkCuda(s.cuda.p_cuEventRecord(s.end, s.stream), "CUDA end timestamp recording");
  checkCuda(s.cuda.p_cuStreamSynchronize(s.stream), "CUDA denoiser synchronization");
  checkCuda(s.cuda.p_cuMemcpyDtoH(output.data(), s.pixels[3], s.imageBytes), "CUDA denoiser output download");
  float elapsedMs{};
  checkCuda(s.cuda.p_cuEventElapsedTime(&elapsedMs, s.start, s.end), "CUDA denoiser elapsed time");
  return elapsedMs;
}

size_t ReferenceOptixDenoiser::allocatedBytes() const noexcept {
  const auto& s = *impl_;
  return s.imageBytes * s.pixels.size() - s.imageBytes / 2 + s.sizes.stateSizeInBytes +
      s.sizes.withoutOverlapScratchSizeInBytes + sizeof(float);
}

} // namespace container::renderer
