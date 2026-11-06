#pragma once
#include <cstdint>
#include <cstdio>
#include <liburing/error/uring_error.hpp>
#include <source_location>
#include <string_view>
#if !defined(__linux__) || !defined(__x86_64__) || defined(__ILP32__)
#error "liburing syscall ABI requires Linux x86-64 LP64"
#endif
namespace liburing::sys {
inline constexpr std::int64_t kSysClose = 3;
inline constexpr std::int64_t kSysMmap = 9;
inline constexpr std::int64_t kSysMunmap = 11;
inline constexpr std::int64_t kSysMincore = 27;
inline constexpr long kSysNanosleep = 35;
inline constexpr std::int64_t kSysGetpid = 39;
inline constexpr std::int64_t kSysUname = 63;
inline constexpr std::int64_t kSysFcntl = 72;
inline constexpr std::int64_t kSysGettid = 186;
inline constexpr std::int64_t kSysMemfdCreate = 319;
inline constexpr long kSysStatx = 332;
[[noreturn, gnu::cold, gnu::noinline]] inline void Panic(
    std::string_view invariant, std::source_location location = std::source_location::current()) noexcept {
  std::fprintf(stderr, "liburing invariant at %s:%u (%s): %.*s\n", location.file_name(), location.line(),
               location.function_name(), static_cast<int>(invariant.size()), invariant.data());
  __builtin_trap();
}

[[nodiscard, gnu::always_inline]] inline std::int64_t Syscall6(std::int64_t number, std::uint64_t a0 = 0, std::uint64_t a1 = 0,
                                                               std::uint64_t a2 = 0, std::uint64_t a3 = 0, std::uint64_t a4 = 0,
                                                               std::uint64_t a5 = 0) noexcept {
  register std::uint64_t r10 asm("r10") = a3;
  register std::uint64_t r8 asm("r8") = a4;
  register std::uint64_t r9 asm("r9") = a5;
  std::int64_t result;
  asm volatile("syscall"
               : "=a"(result)
               : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
               : "rcx", "r11", "memory", "cc");
  return result;
}
[[nodiscard]] inline std::uint64_t Address(const void* pointer) noexcept { return reinterpret_cast<std::uint64_t>(pointer); }
}  // namespace liburing::sys
