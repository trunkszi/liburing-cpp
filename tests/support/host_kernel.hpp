#pragma once

#include <cstdint>
#include <cstdio>
#include <liburing/caps/kernel_caps.hpp>
#include <liburing/sys/syscall.hpp>

namespace liburing::test_support {

// -1: host meets the requirement. 0: older kernel, caller skips. 1: uname probe failed.
[[nodiscard]] inline int SkipBelowKernel(std::uint32_t major, std::uint32_t minor) noexcept {
  const auto version = caps::VerifyKernelVersion(major, minor);
  if (version) return -1;
  if (version.error() != UringErrorCode::kUnsupportedKernelVersion) {
    std::fprintf(stderr, "kernel version probe failed: %u\n", static_cast<unsigned>(version.error()));
    return 1;
  }
  struct Utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
  } name{};
  const char* release = sys::Syscall6(sys::kSysUname, sys::Address(&name)) >= 0 ? name.release : "unknown";
  std::fprintf(stderr, "skip: uname -r %s is below %u.%u\n", release, major, minor);
  return 0;
}

}  // namespace liburing::test_support
