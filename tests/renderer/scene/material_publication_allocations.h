#pragma once

// Test-executable allocation replacements: include this header in exactly one
// translation unit, outside any namespace. Never enable denial around Google
// Test assertions, Vulkan/driver calls, or a noexcept operation known to allocate.
#include <gtest/gtest.h>
#include <tiny_gltf.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <utility>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace material_publication_test {
namespace detail {
struct AllocationState {
  std::size_t attempts{};
  bool deny{};
  AllocationState* previous{};
  std::size_t allowance{std::numeric_limits<std::size_t>::max()};
};
thread_local AllocationState* activeState = nullptr;

void recordAttempt() {
  if (!activeState) return;
  if (activeState->attempts != std::numeric_limits<std::size_t>::max())
    ++activeState->attempts;
  if (activeState->deny || activeState->attempts>activeState->allowance) throw std::bad_alloc();
}

void* allocate(std::size_t size) {
  recordAttempt();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}

void* allocateAligned(std::size_t size, std::size_t alignment) {
  recordAttempt();
#ifdef _WIN32
  void* memory = _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? 1 : size) != 0)
    memory = nullptr;
#endif
  if (!memory) throw std::bad_alloc();
  return memory;
}

void freeAligned(void* memory) noexcept {
#ifdef _WIN32
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}
} // namespace detail

// Counts attempted global new calls on this thread, including denied calls.
// Nested guards have independent counters and temporarily replace the outer
// policy. disable() restores the previous policy without allocation and keeps
// allocations() readable for assertions after denial has been disabled.
class AllocationGuard {
public:
  explicit AllocationGuard(bool deny,
      std::size_t successfulAllocationsBeforeDenial=std::numeric_limits<std::size_t>::max()) noexcept
      : state_{0, deny, detail::activeState,successfulAllocationsBeforeDenial} {
    detail::activeState = &state_;
  }
  ~AllocationGuard() { disable(); }
  AllocationGuard(const AllocationGuard&) = delete;
  AllocationGuard& operator=(const AllocationGuard&) = delete;
  AllocationGuard(AllocationGuard&&) = delete;
  AllocationGuard& operator=(AllocationGuard&&) = delete;

  [[nodiscard]] std::size_t allocations() const noexcept { return state_.attempts; }
  void disable() noexcept {
    if (!active_) return;
    // Removing an outer guard explicitly must not leave a dangling previous
    // pointer in a still-active nested guard.
    auto** link = &detail::activeState;
    while (*link && *link != &state_) link = &(*link)->previous;
    if (*link) *link = state_.previous;
    active_ = false;
  }
private:
  detail::AllocationState state_;
  bool active_{true};
};
} // namespace material_publication_test

void* operator new(std::size_t size) {
  return material_publication_test::detail::allocate(size);
}
void* operator new[](std::size_t size) {
  return material_publication_test::detail::allocate(size);
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

void* operator new(std::size_t size, std::align_val_t alignment) {
  return material_publication_test::detail::allocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return material_publication_test::detail::allocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* memory, std::align_val_t) noexcept {
  material_publication_test::detail::freeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
  material_publication_test::detail::freeAligned(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  material_publication_test::detail::freeAligned(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  material_publication_test::detail::freeAligned(memory);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new[](size); } catch (...) { return nullptr; }
}
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try { return ::operator new(size, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try { return ::operator new[](size, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
  material_publication_test::detail::freeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
  material_publication_test::detail::freeAligned(memory);
}

TEST(MaterialPublicationAllocations, CountsAndDeniesAllAllocationFormsWithoutAllocatingInTheGuard) {
  std::array<bool, 4> threw{};
  std::array<bool, 4> null{};
  material_publication_test::AllocationGuard guard(true);
  try { ::operator delete(::operator new(32)); } catch (const std::bad_alloc&) { threw[0] = true; }
  try { ::operator delete[](::operator new[](32)); } catch (const std::bad_alloc&) { threw[1] = true; }
  try { ::operator delete(::operator new(32, std::align_val_t{64}), std::align_val_t{64}); }
  catch (const std::bad_alloc&) { threw[2] = true; }
  try { ::operator delete[](::operator new[](32, std::align_val_t{64}), std::align_val_t{64}); }
  catch (const std::bad_alloc&) { threw[3] = true; }
  void* memory = ::operator new(32, std::nothrow);
  null[0] = memory == nullptr; ::operator delete(memory, std::nothrow);
  memory = ::operator new[](32, std::nothrow);
  null[1] = memory == nullptr; ::operator delete[](memory, std::nothrow);
  memory = ::operator new(32, std::align_val_t{64}, std::nothrow);
  null[2] = memory == nullptr; ::operator delete(memory, std::align_val_t{64}, std::nothrow);
  memory = ::operator new[](32, std::align_val_t{64}, std::nothrow);
  null[3] = memory == nullptr; ::operator delete[](memory, std::align_val_t{64}, std::nothrow);
  guard.disable();
  EXPECT_EQ(guard.allocations(), 8u);
  for (const bool denied : threw) EXPECT_TRUE(denied);
  for (const bool denied : null) EXPECT_TRUE(denied);
}

TEST(MaterialPublicationAllocations, NestedGuardRestoresOuterDenialAndKeepsItsOwnCount) {
  material_publication_test::AllocationGuard outer(true);
  material_publication_test::AllocationGuard inner(false);
  void* memory = ::operator new(32, std::align_val_t{64});
  const bool aligned = reinterpret_cast<std::uintptr_t>(memory) % 64 == 0;
  ::operator delete(memory, std::size_t{32}, std::align_val_t{64});
  inner.disable();
  bool denied = false;
  try { ::operator delete(::operator new(32)); } catch (const std::bad_alloc&) { denied = true; }
  outer.disable();
  EXPECT_TRUE(aligned); EXPECT_TRUE(denied);
  EXPECT_EQ(inner.allocations(), 1u); EXPECT_EQ(outer.allocations(), 1u);
}

TEST(MaterialPublicationAllocations, MaterialSwapExposesMsvcSentinelAllocationDespiteNoexceptMoves) {
  tinygltf::Material left, right;
  left.name = "left"; right.name = "right";
  left.values.emplace("sentinel", tinygltf::Parameter{});
  // MSVC map move construction allocates an empty-tree sentinel even though
  // TinyGLTF explicitly declares Material's defaulted move constructor noexcept.
  // Observe that attempt with allocation allowed: denying it would terminate.
  material_publication_test::AllocationGuard guard(false);
  std::swap(left, right);
  guard.disable();
  EXPECT_EQ(left.name, "right"); EXPECT_EQ(right.name, "left");
  EXPECT_TRUE(right.values.contains("sentinel")); EXPECT_TRUE(left.values.empty());
#ifdef _MSC_VER
  EXPECT_GT(guard.allocations(), 0u);
#endif
}
