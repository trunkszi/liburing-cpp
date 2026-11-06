#pragma once
#include <cstdint>
#include <expected>
namespace liburing::abi {
struct KernelTimespec {
  std::int64_t tv_sec{};
  std::int64_t tv_nsec{};
};
enum class FutexClock : std::int32_t { kRealtime = 0, kMonotonic = 1 };
struct AbsoluteDeadline {
  KernelTimespec time{};
  FutexClock clock{FutexClock::kMonotonic};
  std::uint32_t reserved{};
  [[nodiscard]] constexpr bool valid() const noexcept {
    return reserved == 0 && time.tv_sec >= 0 && time.tv_nsec >= 0 && time.tv_nsec < 1000000000 &&
           (clock == FutexClock::kMonotonic || clock == FutexClock::kRealtime);
  }
};
}  // namespace liburing::abi
