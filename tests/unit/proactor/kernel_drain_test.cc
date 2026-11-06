#include <liburing/core/proactor.hpp>

#include "../../support/host_kernel.hpp"
using namespace liburing;
struct Sink {
  static constexpr std::uint8_t kSubsystemId = 1;
  std::size_t count{};
  bool canceled{true};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept {
    return caps::RequiredCaps::For<abi::kOpTimeout, abi::kOpAsyncCancel>();
  }
  void OnCompletions(std::span<const Delivery> deliveries) noexcept {
    for (const auto& delivery : deliveries) {
      ++count;
      canceled &= delivery.completion.result == -static_cast<int>(UringErrorCode::kCanceled);
    }
  }
};
int main() {
  if (const int gate = test_support::SkipBelowKernel(6, 16); gate >= 0) return gate;
  auto created = Proactor::Create(2, RingMode::kStandard64_16, Sink::RequiredCaps());
  if (!created) return 1;
  auto& engine = *created;
  abi::KernelTimespec timeout{60, 0};
  if (!engine.ArmOneShot(1, ops::PrepareTimeout(&timeout, 0)) || !engine.ArmOneShot(1, ops::PrepareTimeout(&timeout, 0)))
    return 2;
  if (!engine.BeginDrain()) return 3;
  if (engine.ArmOneShot(1, ops::PrepareNop()).error() != UringErrorCode::kShuttingDown) return 4;
  Sink sink;
  SingleDispatcher dispatcher{sink};
  for (std::size_t step = 0; engine.Stage() != DrainStage::kQuiescent && step < 100000; ++step)
    if (!engine.Drive(DriveMode::kPoll, dispatcher)) return 5;
  if (engine.InFlight() != 0 || sink.count != 2 || !sink.canceled || engine.Stage() != DrainStage::kQuiescent) return 6;
  const auto fd = engine.Backend().Fd();
  ControlCommand resize{{kNullToken, engine.Instance()}, 4, ControlKind::kResize, 0};
  if (!engine.EnqueueControl(31, resize)) return 8;
  auto resized = engine.Drive(DriveMode::kPoll, dispatcher);
  if (resized || resized.error() != UringErrorCode::kNotSupported || engine.Backend().Fd() != fd) return 9;
  return engine.Close() ? 0 : 7;
}
