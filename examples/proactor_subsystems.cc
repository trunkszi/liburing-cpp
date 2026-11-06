#include <array>
#include <chrono>
#include <cstdio>
#include <liburing/liburing.hpp>

using namespace liburing;
namespace {
enum class Routes : std::uint8_t { kCompute = 1, kTimer = 2 };
struct Work {
  std::uint8_t kind;
  std::uint16_t sequence;
};
struct Worker {
  std::array<unsigned, 2> counts{};
  std::array<std::uint64_t, 2> payloads{EncodePayload(Work{1, 42}), EncodePayload(Work{2, 43})};
  bool valid{true};
};
}  // namespace
int main() {
  if (!caps::VerifyKernelVersion(6, 16).has_value()) {
    std::fputs("skip: host kernel is below 6.16\n", stderr);
    return 0;
  }
  auto engine = Proactor::Builder().Entries(16).Build();
  if (!engine) return 1;
  Worker worker;
  auto dispatcher = MakeRouteDispatcher<Routes>(worker, [](Routes route, Worker& state, const Delivery& delivery) noexcept {
    const auto index = static_cast<std::size_t>(route) - 1;
    const auto work = DecodePayload<Work>(state.payloads[index]);
    state.valid &= work && work->sequence == 42 + index &&
                   (route == Routes::kTimer ? delivery.completion.result == -static_cast<int>(UringErrorCode::kTimerExpired)
                                            : delivery.completion.result == 0);
    ++state.counts[index];
  });
  engine->BindDispatcher(dispatcher);
  abi::KernelTimespec timeout{0, 1000000};
  const bool armed =
      engine->Arm(1, ops::PrepareNop()).has_value() && engine->Arm(2, ops::PrepareTimeout(&timeout, 0)).has_value();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool success = armed;
  while (success && engine->InFlight() != 0 && std::chrono::steady_clock::now() < deadline) {
    auto completed = engine->WaitCompletions(1, std::chrono::milliseconds(50), dispatcher);
    success = completed || completed.error() == UringErrorCode::kTimedOut;
  }
  success &= engine->InFlight() == 0 && worker.valid && worker.counts == std::array<unsigned, 2>{1, 1};
  const auto shutdown = engine->ShutdownWith(dispatcher);
  if (!success || !shutdown) return 2;
  std::puts("one thread: compute + timer routed; caller-held reflected payloads verified");
}
