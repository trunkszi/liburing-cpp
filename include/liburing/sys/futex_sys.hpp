#pragma once
#include <cstdint>
#include <liburing/abi/time.hpp>
#include <liburing/abi/uapi_futex.hpp>
#include <liburing/sys/syscall.hpp>
namespace liburing::sys {
inline constexpr std::int64_t kSysFutexWaitv = 449;
inline constexpr std::int64_t kSysFutexWake = 454;
inline constexpr std::int64_t kSysFutexWait = 455;
inline constexpr std::int64_t kSysFutexRequeue = 456;
[[nodiscard]] inline std::int64_t SysFutexWake(void* address, std::uint64_t mask, std::int32_t count,
                                               std::uint32_t flags) noexcept {
  return Syscall6(kSysFutexWake, Address(address), mask, static_cast<std::uint64_t>(static_cast<std::int64_t>(count)), flags);
}
[[nodiscard]] inline std::int64_t SysFutexWait(void* address, std::uint64_t value, std::uint64_t mask, std::uint32_t flags,
                                               const abi::KernelTimespec* timeout = nullptr,
                                               abi::FutexClock clock = abi::FutexClock::kMonotonic) noexcept {
  return Syscall6(kSysFutexWait, Address(address), value, mask, flags, Address(timeout), static_cast<std::uint64_t>(clock));
}
[[nodiscard]] inline std::int64_t SysFutexWaitv(const abi::FutexWaitv* waiters, std::uint32_t count, std::uint32_t flags,
                                                const abi::KernelTimespec* timeout = nullptr,
                                                abi::FutexClock clock = abi::FutexClock::kMonotonic) noexcept {
  return Syscall6(kSysFutexWaitv, Address(waiters), count, flags, Address(timeout), static_cast<std::uint64_t>(clock));
}
[[nodiscard]] inline std::int64_t SysFutexRequeue(const abi::FutexWaitv* pair, std::uint32_t flags, std::int32_t wake,
                                                  std::int32_t requeue) noexcept {
  return Syscall6(kSysFutexRequeue, Address(pair), flags, static_cast<std::uint64_t>(static_cast<std::int64_t>(wake)),
                  static_cast<std::uint64_t>(static_cast<std::int64_t>(requeue)));
}
}  // namespace liburing::sys
