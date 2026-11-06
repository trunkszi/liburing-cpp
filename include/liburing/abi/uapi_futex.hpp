// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
#pragma once
#include <cstdint>
namespace liburing::abi {
inline constexpr std::uint32_t kFutex2SizeU8 = 0x00;
inline constexpr std::uint32_t kFutex2SizeU16 = 0x01;
inline constexpr std::uint32_t kFutex2SizeU32 = 0x02;
inline constexpr std::uint32_t kFutex2SizeU64 = 0x03;
inline constexpr std::uint32_t kFutex2SizeMask = 0x03;
inline constexpr std::uint32_t kFutex2Numa = 0x04;
inline constexpr std::uint32_t kFutex2Mpol = 0x08;
inline constexpr std::uint32_t kFutex2Private = 0x80;
inline constexpr std::uint32_t kFutexNoNode = 0xffffffffU;
inline constexpr std::uint32_t kFutexWaitvMax = 128;
struct FutexWaitv {
  std::uint64_t val{};
  std::uint64_t uaddr{};
  std::uint32_t flags{};
  std::uint32_t reserved{};
};
}  // namespace liburing::abi
