#pragma once
#include <cstdint>
#include <liburing/sys/uring_sys.hpp>
#include <utility>
namespace liburing::sys {
class UniqueFd final {
  std::int32_t fd_{-1};
  explicit UniqueFd(std::int32_t fd) noexcept : fd_(fd) {}

 public:
  UniqueFd() noexcept = default;
  UniqueFd(const UniqueFd&) = delete ("fd ownership is unique");
  UniqueFd& operator=(const UniqueFd&) = delete ("fd ownership is unique");
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      [[maybe_unused]] const auto close_result = reset();
      fd_ = other.release();
    }
    return *this;
  }
  ~UniqueFd() noexcept { [[maybe_unused]] const auto close_result = reset(); }
  [[nodiscard]] static Result<UniqueFd> Adopt(std::int32_t fd) noexcept {
    if (fd < 0) return std::unexpected(UringErrorCode::kBadFd);
    return UniqueFd{fd};
  }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] std::int32_t get() const noexcept { return fd_; }
  [[nodiscard]] std::int32_t release() noexcept { return std::exchange(fd_, -1); }
  [[nodiscard]] Result<void> reset() noexcept {
    if (!valid()) return {};
    // Linux releases the descriptor even when close reports EINTR; retrying can close a reused fd.
    const auto result = ToResult(Syscall6(kSysClose, static_cast<std::uint64_t>(release())));
    if (!result) return std::unexpected(result.error());
    return {};
  }
};

/** Registration indices belong to one Linux task; reset and destruction must run on that issuer task. */
class RegisteredRingFd final {
  static constexpr std::uint32_t kEmpty = 0xffffffffU;
  std::uint32_t index_{kEmpty};
  std::int32_t task_{};
  RegisteredRingFd(std::uint32_t index, std::int32_t task) noexcept : index_(index), task_(task) {}

 public:
  RegisteredRingFd() noexcept = default;
  RegisteredRingFd(const RegisteredRingFd&) = delete ("ring registrations belong to one issuer task");
  RegisteredRingFd& operator=(const RegisteredRingFd&) = delete ("ring registrations belong to one issuer task");
  RegisteredRingFd(RegisteredRingFd&& other) noexcept : index_(std::exchange(other.index_, kEmpty)), task_(other.task_) {}
  RegisteredRingFd& operator=(RegisteredRingFd&& other) noexcept {
    if (this != &other) {
      if (!reset()) Panic("registered ring cleanup failed on issuer task");
      index_ = std::exchange(other.index_, kEmpty);
      task_ = other.task_;
    }
    return *this;
  }
  ~RegisteredRingFd() noexcept {
    if (!reset()) Panic("registered ring cleanup failed on issuer task");
  }
  [[nodiscard]] static Result<RegisteredRingFd> Adopt(std::uint32_t index) noexcept {
    if (index > 0x7fffffffU) return std::unexpected(UringErrorCode::kInvalid);
    return RegisteredRingFd{index, static_cast<std::int32_t>(Syscall6(kSysGettid))};
  }
  [[nodiscard]] static Result<RegisteredRingFd> Register(std::int32_t ring_fd) noexcept {
    abi::IoUringRsrcUpdate update{.offset = kEmpty, .data = static_cast<std::uint64_t>(static_cast<std::int64_t>(ring_fd))};
    const auto result = ToResult(SysIoUringRegister(ring_fd, abi::kRegisterRingFds, &update, 1));
    if (!result) return std::unexpected(result.error());
    if (*result != 1) return std::unexpected(UringErrorCode::kProtocolViolation);
    return RegisteredRingFd{update.offset, static_cast<std::int32_t>(Syscall6(kSysGettid))};
  }
  [[nodiscard]] bool valid() const noexcept { return index_ != kEmpty; }
  [[nodiscard]] std::uint32_t index() const noexcept { return index_; }
  [[nodiscard]] Result<void> reset() noexcept {
    if (!valid()) return {};
    if (Syscall6(kSysGettid) != task_) return std::unexpected(UringErrorCode::kBadRing);
    abi::IoUringRsrcUpdate update{.offset = index_};
    const auto result = ToResult(SysIoUringRegister(static_cast<std::int32_t>(index_),
                                                    abi::kUnregisterRingFds | abi::kRegisterUseRegisteredRing, &update, 1));
    if (!result) return std::unexpected(result.error());
    if (*result != 1) return std::unexpected(UringErrorCode::kProtocolViolation);
    index_ = kEmpty;
    return {};
  }
};
}  // namespace liburing::sys
