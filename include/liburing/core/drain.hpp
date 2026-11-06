/** @file @brief Shutdown retains ownership until both kernel work and staged controls finish. */
#pragma once
#include <liburing/error/uring_error.hpp>
namespace liburing {
enum class DrainStage : std::uint8_t { kRunning, kStopping, kDraining, kQuiescent, kClosed };
static_assert(uarch::RegisterPassable<DrainStage>);
class DrainState {
  DrainStage stage_{DrainStage::kRunning};

 public:
  [[nodiscard]] constexpr DrainStage Stage() const noexcept { return stage_; }
  [[nodiscard]] constexpr Result<void> BeginDrain() noexcept {
    if (stage_ == DrainStage::kRunning) stage_ = DrainStage::kStopping;
    return {};
  }
  constexpr void StartDraining() noexcept {
    if (stage_ == DrainStage::kStopping) stage_ = DrainStage::kDraining;
  }
  constexpr void Observe(std::size_t in_flight, bool queues_empty) noexcept {
    if (stage_ == DrainStage::kDraining && in_flight == 0 && queues_empty) stage_ = DrainStage::kQuiescent;
  }
  [[nodiscard]] constexpr Result<void> Close() noexcept {
    if (stage_ != DrainStage::kQuiescent && stage_ != DrainStage::kClosed) return std::unexpected(UringErrorCode::kBusy);
    stage_ = DrainStage::kClosed;
    return {};
  }
};
}  // namespace liburing
