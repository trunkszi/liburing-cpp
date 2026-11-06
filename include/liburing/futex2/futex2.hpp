#pragma once
#include <cstdint>
#include <liburing/abi/time.hpp>
#include <liburing/futex2/futex_flags.hpp>
#include <liburing/sys/futex_sys.hpp>
#include <limits>
#include <optional>
#include <span>
namespace liburing::futex2::sync {
inline constexpr std::uint32_t kFutexBitsetMatchAny = 0xffffffffU;
enum class WaitOutcome : std::uint8_t { kWoken, kValueChanged, kTimedOut, kInterrupted };
static_assert(uarch::RegisterPassable<WaitOutcome>);
struct WaitAnyOutcome {
  std::uint32_t index{};
  WaitOutcome outcome{WaitOutcome::kWoken};
  std::uint8_t reserved[3]{};
};
static_assert(sizeof(WaitAnyOutcome) == 8 && uarch::NoPadding<WaitAnyOutcome> && uarch::RegisterPassable<WaitAnyOutcome> &&
              uarch::GprPair<WaitAnyOutcome>);
namespace detail {
[[nodiscard]] constexpr std::optional<WaitOutcome> WaitStatus(std::int64_t raw) noexcept {
  if (raw == 0) return WaitOutcome::kWoken;
  if (raw == -static_cast<long>(UringErrorCode::kAgain)) return WaitOutcome::kValueChanged;
  if (raw == -static_cast<long>(UringErrorCode::kTimedOut)) return WaitOutcome::kTimedOut;
  if (raw == -static_cast<long>(UringErrorCode::kInterrupted)) return WaitOutcome::kInterrupted;
  return std::nullopt;
}
[[nodiscard]] constexpr UringErrorCode Failure(std::int64_t raw) noexcept {
  auto result = FromRawSyscall(raw);
  return result ? UringErrorCode::kProtocolViolation : result.error().code();
}
}  // namespace detail
[[nodiscard]] inline Result<std::uint32_t> Wake(FutexRef target, std::int32_t count = 1,
                                                std::uint32_t mask = kFutexBitsetMatchAny) noexcept {
  auto valid = ValidateRef(target);
  if (!valid) return std::unexpected(valid.error());
  if (count < 0 || mask == 0) return std::unexpected(UringErrorCode::kInvalid);
  const auto raw = sys::SysFutexWake(reinterpret_cast<void*>(target.uaddr), mask, count, target.flags);
  auto result = ToResult(raw);
  if (!result) return std::unexpected(result.error());
  if (*result < 0 || *result > count) return std::unexpected(UringErrorCode::kProtocolViolation);
  return static_cast<std::uint32_t>(*result);
}
// A wake only schedules a waiter; callers recheck their predicate with an acquire load.
[[nodiscard]] inline Result<WaitOutcome> Wait(FutexRef target, std::uint32_t expected,
                                              std::optional<abi::AbsoluteDeadline> deadline = std::nullopt,
                                              std::uint32_t mask = kFutexBitsetMatchAny) noexcept {
  auto valid = ValidateRef(target);
  if (!valid) return std::unexpected(valid.error());
  if (mask == 0 || (deadline && !deadline->valid())) return std::unexpected(UringErrorCode::kInvalid);
  const auto raw =
      sys::SysFutexWait(reinterpret_cast<void*>(target.uaddr), expected, mask, target.flags,
                        deadline ? &deadline->time : nullptr, deadline ? deadline->clock : abi::FutexClock::kMonotonic);
  auto outcome = detail::WaitStatus(raw);
  if (outcome) return *outcome;
  return std::unexpected(detail::Failure(raw));
}
[[nodiscard]] inline Result<std::uint32_t> Requeue(FutexRef source, std::uint32_t expected_source, FutexRef target,
                                                   std::int32_t nr_wake, std::int32_t nr_requeue) noexcept {
  auto from = ValidateRef(source);
  if (!from) return std::unexpected(from.error());
  auto to = ValidateRef(target);
  if (!to) return std::unexpected(to.error());
  if (nr_wake < 0 || nr_requeue < 0) return std::unexpected(UringErrorCode::kInvalid);
  abi::FutexWaitv pair[2]{{expected_source, source.uaddr, source.flags, 0}, {0, target.uaddr, target.flags, 0}};
  auto result = ToResult(sys::SysFutexRequeue(pair, 0, nr_wake, nr_requeue));
  if (!result) return std::unexpected(result.error());
  const auto maximum = static_cast<std::uint64_t>(nr_wake) + static_cast<std::uint64_t>(nr_requeue);
  if (*result < 0 || static_cast<std::uint64_t>(*result) > maximum) return std::unexpected(UringErrorCode::kProtocolViolation);
  return static_cast<std::uint32_t>(*result);
}
// The caller keeps the waiter array immutable and its referenced words alive until the wait returns.
[[nodiscard]] inline Result<WaitAnyOutcome> WaitAny(std::span<const abi::FutexWaitv> waiters,
                                                    std::optional<abi::AbsoluteDeadline> deadline = std::nullopt) noexcept {
  if (waiters.empty() || waiters.size() > abi::kFutexWaitvMax || !waiters.data() || (deadline && !deadline->valid()))
    return std::unexpected(UringErrorCode::kInvalid);
  for (const auto& waiter : waiters) {
    auto valid = ValidateRef(FutexRef{waiter.uaddr, waiter.flags, waiter.reserved});
    if (!valid) return std::unexpected(valid.error());
    if (waiter.val > std::numeric_limits<std::uint32_t>::max()) return std::unexpected(UringErrorCode::kInvalid);
  }
  const auto raw =
      sys::SysFutexWaitv(waiters.data(), static_cast<std::uint32_t>(waiters.size()), 0, deadline ? &deadline->time : nullptr,
                         deadline ? deadline->clock : abi::FutexClock::kMonotonic);
  if (raw >= 0 && static_cast<std::uint64_t>(raw) < waiters.size())
    return WaitAnyOutcome{static_cast<std::uint32_t>(raw), WaitOutcome::kWoken, {}};
  auto status = detail::WaitStatus(raw);
  if (status) return WaitAnyOutcome{0, *status, {}};
  return std::unexpected(detail::Failure(raw));
}
}  // namespace liburing::futex2::sync
namespace liburing::futex2 {
using sync::kFutexBitsetMatchAny;
using sync::Requeue;
using sync::Wait;
using sync::WaitAny;
using sync::WaitAnyOutcome;
using sync::WaitOutcome;
using sync::Wake;
}  // namespace liburing::futex2
