#pragma once
#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/sys/syscall.hpp>
namespace liburing::sys {
inline constexpr std::int64_t kSysIoUringSetup = 425;
inline constexpr std::int64_t kSysIoUringEnter = 426;
inline constexpr std::int64_t kSysIoUringRegister = 427;
[[nodiscard]] inline std::int64_t SysIoUringSetup(std::uint32_t entries, abi::IoUringParams* params) noexcept {
  return Syscall6(kSysIoUringSetup, entries, Address(params));
}
[[nodiscard]] inline std::int64_t SysIoUringEnter(std::int32_t fd, std::uint32_t submit, std::uint32_t complete,
                                                  std::uint32_t flags, const void* argument = nullptr,
                                                  std::uint64_t size = 0) noexcept {
  return Syscall6(kSysIoUringEnter, static_cast<std::uint64_t>(static_cast<std::int64_t>(fd)), submit, complete, flags,
                  Address(argument), size);
}
[[nodiscard]] inline std::int64_t SysIoUringRegister(std::int32_t fd, std::uint32_t opcode, const void* argument,
                                                     std::uint32_t count) noexcept {
  return Syscall6(kSysIoUringRegister, static_cast<std::uint64_t>(static_cast<std::int64_t>(fd)), opcode, Address(argument),
                  count);
}
}  // namespace liburing::sys
