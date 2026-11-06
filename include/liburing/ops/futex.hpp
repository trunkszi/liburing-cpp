/** @file @brief io_uring futex2 flags occupy fd; NUMA node storage must remain writable. */
#pragma once
#include <liburing/abi/uapi_futex.hpp>
#include <liburing/ops/core.hpp>
namespace liburing::ops {
struct alignas(8) FutexNumaWord32 {
  std::uint32_t value{};
  std::uint32_t node{abi::kFutexNoNode};
};
static_assert(sizeof(FutexNumaWord32) == 8 && uarch::NoPadding<FutexNumaWord32> && uarch::RegisterPassable<FutexNumaWord32>);
namespace detail {
template <std::uint32_t kFlags>
concept ValidFutexFlags = (kFlags & abi::kFutex2SizeMask) == abi::kFutex2SizeU32 &&
                          (kFlags & ~(abi::kFutex2SizeMask | abi::kFutex2Numa | abi::kFutex2Mpol | abi::kFutex2Private)) == 0 &&
                          ((kFlags & abi::kFutex2Mpol) == 0 || (kFlags & abi::kFutex2Numa) != 0);
template <std::uint32_t kFlags>
consteval std::uint32_t FutexNeeds() noexcept {
  return caps::kFutexU32 | ((kFlags & abi::kFutex2Numa) != 0 ? caps::kFutexNuma : 0U) |
         ((kFlags & abi::kFutex2Mpol) != 0 ? caps::kFutexMpol : 0U);
}
template <class Word>
concept UnsupportedFutexWord =
    std::same_as<std::remove_cv_t<Word>, std::uint8_t> || std::same_as<std::remove_cv_t<Word>, std::uint16_t> ||
    std::same_as<std::remove_cv_t<Word>, std::uint64_t>;
}  // namespace detail
template <std::uint32_t kFlags = abi::kFutex2SizeU32>
using FutexWait = detail::Operation<abi::kOpFutexWait, OpKind::kOneShot, 1, false, detail::FutexNeeds<kFlags>()>;
template <std::uint32_t kFlags = abi::kFutex2SizeU32>
using FutexWake = detail::Operation<abi::kOpFutexWake, OpKind::kOneShot, 1, false, detail::FutexNeeds<kFlags>()>;
template <std::uint32_t kFlags = abi::kFutex2SizeU32>
using FutexWaitv = detail::Operation<abi::kOpFutexWaitv, OpKind::kOneShot, 1, false, detail::FutexNeeds<kFlags>()>;
template <std::uint32_t kFlags = abi::kFutex2SizeU32>
  requires detail::ValidFutexFlags<kFlags>
[[nodiscard]] constexpr FutexWait<kFlags> PrepareFutexWait(UserAddress address, std::uint32_t value,
                                                           std::uint32_t mask = 0xffffffffU) noexcept {
  return FutexWait<kFlags>{
      {.fd = static_cast<std::int32_t>(kFlags), .offset = value, .address = address.Raw(), .address3 = mask}};
}
template <std::uint32_t kFlags = abi::kFutex2SizeU32>
  requires detail::ValidFutexFlags<kFlags>
[[nodiscard]] constexpr FutexWake<kFlags> PrepareFutexWake(UserAddress address, std::uint32_t count,
                                                           std::uint32_t mask = 0xffffffffU) noexcept {
  return FutexWake<kFlags>{
      {.fd = static_cast<std::int32_t>(kFlags), .offset = count, .address = address.Raw(), .address3 = mask}};
}
template <std::uint32_t kFlags = abi::kFutex2SizeU32>
  requires detail::ValidFutexFlags<kFlags>
[[nodiscard]] constexpr FutexWaitv<kFlags> PrepareFutexWaitv(UserAddress waiters, std::uint32_t count) noexcept {
  return FutexWaitv<kFlags>{{.fd = 0, .address = waiters.Raw(), .len = count}};
}

template <std::uint32_t kFlags = abi::kFutex2SizeU32, class Word>
  requires(std::same_as<std::remove_cv_t<Word>, std::uint32_t> && detail::ValidFutexFlags<kFlags> &&
           (kFlags & abi::kFutex2Numa) == 0)
[[nodiscard]] inline FutexWait<kFlags> PrepareFutexWait(Word* address, std::uint32_t value,
                                                        std::uint32_t mask = 0xffffffffU) noexcept {
  return PrepareFutexWait<kFlags>(UserAddress{address}, value, mask);
}
template <std::uint32_t kFlags = abi::kFutex2SizeU32 | abi::kFutex2Numa>
  requires detail::ValidFutexFlags<kFlags>
[[nodiscard]] inline FutexWait<kFlags> PrepareFutexWait(FutexNumaWord32* address, std::uint32_t value,
                                                        std::uint32_t mask = 0xffffffffU) noexcept {
  return PrepareFutexWait<kFlags>(UserAddress{address}, value, mask);
}
template <std::uint32_t kFlags = abi::kFutex2SizeU32, class Word>
  requires detail::UnsupportedFutexWord<Word>
auto PrepareFutexWait(Word*, std::uint32_t, std::uint32_t = 0xffffffffU) noexcept =
    delete ("Linux kernel futex2 currently only implements FUTEX2_SIZE_U32");
template <std::uint32_t kFlags>
  requires((kFlags & abi::kFutex2Mpol) != 0 && (kFlags & abi::kFutex2Numa) == 0)
auto PrepareFutexWait(UserAddress, std::uint32_t,
                      std::uint32_t = 0xffffffffU) noexcept = delete ("FUTEX2_MPOL requires FUTEX2_NUMA");
template <std::uint32_t kFlags>
  requires((kFlags & abi::kFutex2SizeMask) != abi::kFutex2SizeU32 &&
           ((kFlags & abi::kFutex2Mpol) == 0 || (kFlags & abi::kFutex2Numa) != 0))
auto PrepareFutexWait(UserAddress, std::uint32_t, std::uint32_t = 0xffffffffU) noexcept =
    delete ("Linux kernel futex2 currently only implements FUTEX2_SIZE_U32");
template <std::uint32_t kFlags, class Word>
  requires(std::same_as<std::remove_cv_t<Word>, std::uint32_t> && detail::ValidFutexFlags<kFlags> &&
           (kFlags & abi::kFutex2Numa) != 0)
auto PrepareFutexWait(Word*, std::uint32_t,
                      std::uint32_t = 0xffffffffU) noexcept = delete ("NUMA requires writable FutexNumaWord32 storage");
template <std::uint32_t kFlags = abi::kFutex2SizeU32 | abi::kFutex2Numa>
auto PrepareFutexWait(const FutexNumaWord32*, std::uint32_t,
                      std::uint32_t = 0xffffffffU) noexcept = delete ("NUMA requires writable FutexNumaWord32 storage");

template <std::uint32_t kFlags = abi::kFutex2SizeU32, class Word>
  requires(std::same_as<std::remove_cv_t<Word>, std::uint32_t> && detail::ValidFutexFlags<kFlags> &&
           (kFlags & abi::kFutex2Numa) == 0)
[[nodiscard]] inline FutexWake<kFlags> PrepareFutexWake(Word* address, std::uint32_t count,
                                                        std::uint32_t mask = 0xffffffffU) noexcept {
  return PrepareFutexWake<kFlags>(UserAddress{address}, count, mask);
}
template <std::uint32_t kFlags = abi::kFutex2SizeU32 | abi::kFutex2Numa>
  requires detail::ValidFutexFlags<kFlags>
[[nodiscard]] inline FutexWake<kFlags> PrepareFutexWake(FutexNumaWord32* address, std::uint32_t count,
                                                        std::uint32_t mask = 0xffffffffU) noexcept {
  return PrepareFutexWake<kFlags>(UserAddress{address}, count, mask);
}
template <std::uint32_t kFlags = abi::kFutex2SizeU32, class Word>
  requires detail::UnsupportedFutexWord<Word>
auto PrepareFutexWake(Word*, std::uint32_t, std::uint32_t = 0xffffffffU) noexcept =
    delete ("Linux kernel futex2 currently only implements FUTEX2_SIZE_U32");
template <std::uint32_t kFlags>
  requires((kFlags & abi::kFutex2Mpol) != 0 && (kFlags & abi::kFutex2Numa) == 0)
auto PrepareFutexWake(UserAddress, std::uint32_t,
                      std::uint32_t = 0xffffffffU) noexcept = delete ("FUTEX2_MPOL requires FUTEX2_NUMA");
template <std::uint32_t kFlags>
  requires((kFlags & abi::kFutex2SizeMask) != abi::kFutex2SizeU32 &&
           ((kFlags & abi::kFutex2Mpol) == 0 || (kFlags & abi::kFutex2Numa) != 0))
auto PrepareFutexWake(UserAddress, std::uint32_t, std::uint32_t = 0xffffffffU) noexcept =
    delete ("Linux kernel futex2 currently only implements FUTEX2_SIZE_U32");
template <std::uint32_t kFlags, class Word>
  requires(std::same_as<std::remove_cv_t<Word>, std::uint32_t> && detail::ValidFutexFlags<kFlags> &&
           (kFlags & abi::kFutex2Numa) != 0)
auto PrepareFutexWake(Word*, std::uint32_t,
                      std::uint32_t = 0xffffffffU) noexcept = delete ("NUMA requires writable FutexNumaWord32 storage");
template <std::uint32_t kFlags = abi::kFutex2SizeU32 | abi::kFutex2Numa>
auto PrepareFutexWake(const FutexNumaWord32*, std::uint32_t,
                      std::uint32_t = 0xffffffffU) noexcept = delete ("NUMA requires writable FutexNumaWord32 storage");
template <std::uint32_t kFlags>
  requires((kFlags & abi::kFutex2Mpol) != 0 && (kFlags & abi::kFutex2Numa) == 0)
auto PrepareFutexWaitv(UserAddress, std::uint32_t) noexcept = delete ("FUTEX2_MPOL requires FUTEX2_NUMA");
}  // namespace liburing::ops
