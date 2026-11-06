#pragma once
#include <atomic>
#include <concepts>
#include <cstdint>
#include <liburing/abi/uapi_futex.hpp>
#include <uarch/uarch.hpp>
namespace liburing::futex2 {
struct alignas(4)[[= uarch::layout::register_leaf]] FutexWord32 {
  std::uint32_t value{};
};
struct alignas(8) FutexNumaWord32 {
  std::uint32_t value{};
  // Mutable prevents .rodata placement so kernel kFutexNoNode writeback cannot fault with EFAULT.
  // Node selection is part of the futex key; coordinate initialization before publishing waiters.
  mutable std::uint32_t node{abi::kFutexNoNode};
};
static_assert(uarch::NoPadding<FutexWord32> && uarch::RegisterPassable<FutexWord32> && uarch::RegisterLeaf<FutexWord32>);
static_assert(uarch::NoPadding<FutexNumaWord32> && uarch::RegisterPassable<FutexNumaWord32> &&
              uarch::GprPair<FutexNumaWord32> && uarch::layout::sysv_classify<FutexNumaWord32>().registers == 1);
static_assert(sizeof(FutexWord32) == 4 && alignof(FutexWord32) == 4);
static_assert(sizeof(FutexNumaWord32) == 8 && alignof(FutexNumaWord32) == 8);
static_assert(uarch::reflect::byte_offset(^^FutexNumaWord32::value) == 0 &&
              uarch::reflect::byte_offset(^^FutexNumaWord32::node) == 4);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free && std::atomic_ref<std::uint32_t>::required_alignment <= 4);

template <class T>
concept SupportedWord = std::same_as<T, FutexWord32> || std::same_as<T, FutexNumaWord32>;
template <SupportedWord Word>
[[nodiscard, gnu::always_inline]] inline std::uint32_t LoadAcquire(Word& word) noexcept {
  return std::atomic_ref<std::uint32_t>{word.value}.load(std::memory_order_acquire);
}
template <SupportedWord Word>
[[gnu::always_inline]] inline void StoreRelease(Word& word, std::uint32_t value) noexcept {
  std::atomic_ref<std::uint32_t>{word.value}.store(value, std::memory_order_release);
}
template <SupportedWord Word>
[[nodiscard, gnu::always_inline]] inline bool CompareExchange(Word& word, std::uint32_t& expected,
                                                              std::uint32_t desired) noexcept {
  return std::atomic_ref<std::uint32_t>{word.value}.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                                                            std::memory_order_acquire);
}
template <SupportedWord Word>
[[nodiscard, gnu::always_inline]] inline std::uint32_t FetchAdd(Word& word, std::uint32_t delta) noexcept {
  return std::atomic_ref<std::uint32_t>{word.value}.fetch_add(delta, std::memory_order_acq_rel);
}

struct FutexWord8 {
  FutexWord8() = delete ("Linux kernel futex_flags_valid() currently only implements FUTEX2_SIZE_U32");
};
struct FutexWord16 {
  FutexWord16() = delete ("Linux kernel futex_flags_valid() currently only implements FUTEX2_SIZE_U32");
};
struct FutexWord64 {
  FutexWord64() = delete ("Linux kernel futex_flags_valid() currently only implements FUTEX2_SIZE_U32");
};
struct FutexNumaWord64 {
  FutexNumaWord64() = delete ("Linux kernel futex_flags_valid() currently only implements FUTEX2_SIZE_U32");
};
}  // namespace liburing::futex2
