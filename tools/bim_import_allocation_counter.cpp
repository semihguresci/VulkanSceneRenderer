#include <cstddef>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace {

thread_local size_t allocationCount;
thread_local size_t allocatedBytes;

template <typename Allocate>
void *allocate(size_t bytes, Allocate &&tryAllocate) {
  for (;;) {
    if (auto *memory = tryAllocate(bytes == 0 ? 1 : bytes)) {
      ++allocationCount;
      allocatedBytes += bytes;
      return memory;
    }
    const auto handler = std::get_new_handler();
    if (!handler)
      throw std::bad_alloc{};
    handler();
  }
}

void *allocateAligned(size_t bytes, std::align_val_t alignment) {
  return allocate(bytes, [alignment](size_t size) -> void * {
#ifdef _WIN32
    return _aligned_malloc(size, static_cast<size_t>(alignment));
#else
    void *memory = nullptr;
    if (posix_memalign(&memory, static_cast<size_t>(alignment), size) != 0)
      return nullptr;
    return memory;
#endif
  });
}

void freeAligned(void *memory) noexcept {
#ifdef _WIN32
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}

} // namespace

// Linked only into BimImportAllocationBenchmark. This counts requests on the
// synchronous preparation thread through replaceable C++ allocation functions.
void readBimImportAllocationCounters(size_t &count, size_t &bytes) noexcept {
  count = allocationCount;
  bytes = allocatedBytes;
}

void *operator new(size_t bytes) { return allocate(bytes, std::malloc); }
void *operator new[](size_t bytes) { return allocate(bytes, std::malloc); }
void *operator new(size_t bytes, std::align_val_t alignment) {
  return allocateAligned(bytes, alignment);
}
void *operator new[](size_t bytes, std::align_val_t alignment) {
  return allocateAligned(bytes, alignment);
}

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, size_t) noexcept { std::free(memory); }
void operator delete(void *memory, std::align_val_t) noexcept { freeAligned(memory); }
void operator delete[](void *memory, std::align_val_t) noexcept { freeAligned(memory); }
void operator delete(void *memory, size_t, std::align_val_t) noexcept { freeAligned(memory); }
void operator delete[](void *memory, size_t, std::align_val_t) noexcept { freeAligned(memory); }
