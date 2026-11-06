#include <cstdint>
#include <liburing/caps/kernel_caps.hpp>

#include "../../support/host_kernel.hpp"
using namespace liburing;
using namespace liburing::caps;
struct Denied : NativeProbeBackend {
  static std::int64_t Setup(std::uint32_t, abi::IoUringParams*) noexcept { return -1; }
};
struct Old : Denied {
  static Result<void> VerifyVersion() noexcept { return std::unexpected(UringErrorCode::kUnsupportedKernelVersion); }
};
struct RegistrationFailure : NativeProbeBackend {
  inline static std::int32_t last_fd = -1;
  static std::int64_t Setup(std::uint32_t n, abi::IoUringParams* params) noexcept {
    const auto result = NativeProbeBackend::Setup(n, params);
    if (result >= 0) last_fd = static_cast<std::int32_t>(result);
    return result;
  }
  static std::int64_t Register(std::int32_t, std::uint32_t, void*, std::uint32_t) noexcept { return -12; }
};
struct WithoutQuery : NativeProbeBackend {
  static std::int64_t Register(std::int32_t fd, std::uint32_t op, void* data, std::uint32_t n) noexcept {
    if (op == abi::kRegisterQuery) return -22;
    return NativeProbeBackend::Register(fd, op, data, n);
  }
};
struct WithoutNuma : NativeProbeBackend {
  static std::int64_t Wake(void* word, std::uint32_t flags) noexcept {
    if (flags & abi::kFutex2Numa) return -22;
    return NativeProbeBackend::Wake(word, flags);
  }
};
int main() {
  RequiredCaps required{};
  auto old = KernelCaps::Probe<Old>(required);
  if (old || old.error() != UringErrorCode::kUnsupportedKernelVersion) return 2;
  if (const int gate = test_support::SkipBelowKernel(6, 16); gate >= 0) return gate;
  auto denied = KernelCaps::Probe<Denied>(required);
  if (denied || denied.error() != UringErrorCode::kPermissionDenied) return 1;
  auto failure = KernelCaps::Probe<RegistrationFailure>(required);
  if (failure || failure.error() != UringErrorCode::kNoMemory || RegistrationFailure::last_fd < 0) return 3;
  if (sys::Syscall6(72, static_cast<std::uint64_t>(RegistrationFailure::last_fd), 1) != -9) return 4;
  required.require_query = true;
  auto missing_query = KernelCaps::Probe<WithoutQuery>(required);
  if (missing_query || missing_query.error() != UringErrorCode::kNotSupported) return 5;
  required.require_query = false;
  auto baseline = KernelCaps::Probe<WithoutQuery>(required);
  if (!baseline || baseline->query_available || baseline->tier() != KernelTier::kBaseline616) return 6;
  required.required_futex = kFutexU32 | kFutexNuma;
  auto numa = KernelCaps::Probe<WithoutNuma>(required);
  if (numa || numa.error() != UringErrorCode::kInvalid) return 7;
  required.required_futex = kFutexMpol;
  if (KernelCaps::Probe(required).error() != UringErrorCode::kInvalid) return 8;
  return 0;
}
