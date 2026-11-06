#include <pthread.h>

#include <algorithm>
#include <liburing/core/proactor.hpp>
#include <liburing/ops/net.hpp>
#include <liburing/ops/uring_cmd.hpp>

#include "../../support/host_kernel.hpp"
using namespace liburing;
struct Sink {
  static constexpr std::uint8_t kSubsystemId = 1;
  std::size_t count{};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept { return {}; }
  void OnCompletions(std::span<const Delivery> batch) noexcept { count += batch.size(); }
};
int ActiveCapacity() {
  static_assert(sizeof(detail::SubmissionIntent) == 16 && uarch::NoPadding<detail::SubmissionIntent>);
  static_assert(ScriptedProactor::kCapacity == 2048);
  BasicProactor<ScriptedRing, 11, 2> engine{std::in_place, 8U, 64U};
  std::array<UserDataToken, 2047> tokens{};
  Sink sink;
  SingleDispatcher dispatcher{sink};
  for (auto& token : tokens) {
    if (engine.Backend().Sq().FreeSlots() == 0 && !engine.Drive(DriveMode::kPoll, dispatcher)) return 1;
    auto armed = engine.Arm(1, ops::PrepareRecv(-1, nullptr, 0));
    if (!armed) return 2;
    token = *armed;
  }
  if (!engine.Drive(DriveMode::kPoll, dispatcher)) return 3;
  if (engine.InFlight() != 2047 || engine.DeferredCount() != 0 || engine.Backend().Sq().Head() != engine.Backend().Sq().Tail())
    return 3;
  if (engine.Arm(1, ops::PrepareNop()).error() != UringErrorCode::kSlabExhausted) return 4;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    engine.Backend().ScriptCqe(tokens[i], 0);
    if ((i + 1) % 16 == 0 && !engine.Drive(DriveMode::kPoll, dispatcher)) return 5;
  }
  if (!engine.Drive(DriveMode::kPoll, dispatcher) || engine.InFlight() != 0 || sink.count != 2047) return 6;
  return engine.DrainBlocking(dispatcher) ? 0 : 7;
}
int DeferredCapacity() {
  ScriptedProactor engine{std::in_place, 8U, 16U};
  std::array<UserDataToken, 263> tokens{};
  Sink sink;
  SingleDispatcher dispatcher{sink};
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    auto token = engine.Arm(1, ops::PrepareRecv(-1, nullptr, 0));
    if (!token) return 10;
    tokens[i] = *token;
    if (i == 199 && engine.InFlight() != 200) return 11;
  }
  if (engine.DeferredCount() != 255 || engine.Arm(1, ops::PrepareNop()).error() != UringErrorCode::kSqRingFull) return 12;
  for (std::size_t i = 8; i < tokens.size(); ++i)
    if (!engine.Cancel(tokens[i])) return 13;
  if (engine.DeferredCount() != 0 || engine.InFlight() != 8) return 14;
  for (std::size_t i = 8; i < tokens.size(); ++i) {
    auto token = engine.Arm(1, ops::PrepareNop());
    if (!token) return 15;
  }
  for (std::size_t i = 0; i < 8; ++i) engine.Backend().ScriptCqe(tokens[i], 0);
  while (engine.InFlight() != 0)
    if (!engine.Drive(DriveMode::kPoll, dispatcher)) return 16;
  // Refill every deferred pack after Replay has released the entire pool.
  for (std::size_t i = 0; i < tokens.size(); ++i)
    if (!engine.Arm(1, ops::PrepareNop())) return 17;
  while (engine.InFlight() != 0)
    if (!engine.Drive(DriveMode::kPoll, dispatcher)) return 18;
  return engine.DrainBlocking(dispatcher) ? 0 : 19;
}
int DeferredRollback() {
  BasicProactor<ScriptedRing, 4, 2> engine{std::in_place, 2U, 16U};
  std::array<UserDataToken, 5> tokens{};
  Sink sink;
  SingleDispatcher dispatcher{sink};
  for (auto& token : tokens) {
    auto armed = engine.Arm(1, ops::PrepareNop());
    if (!armed) return 30;
    token = *armed;
  }
  for (int i = 0; i < 32; ++i)
    if (engine.Arm(1, ops::PrepareNop()).error() != UringErrorCode::kSqRingFull) return 31;
  for (std::size_t i = 2; i < tokens.size(); ++i)
    if (!engine.Cancel(tokens[i])) return 32;
  for (int i = 0; i < 3; ++i)
    if (!engine.Arm(1, ops::PrepareNop())) return 33;
  while (engine.InFlight() != 0)
    if (!engine.Drive(DriveMode::kPoll, dispatcher)) return 34;
  return engine.DrainBlocking(dispatcher) ? 0 : 35;
}
int DeferredWideChain() {
  BasicProactor<ScriptedRing, 4, 2> engine{std::in_place, 4U, 16U, RingMode::kMixed64_128_16_32};
  Sink sink;
  SingleDispatcher dispatcher{sink};
  for (int i = 0; i < 3; ++i)
    if (!engine.Arm(1, ops::PrepareNop())) return 40;
  std::array<std::byte, 80> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>(i);
  auto chain = engine.ArmChain(1, ops::PrepareUringCmd128(-1, 123, bytes), ops::PrepareNop());
  if (!chain || engine.DeferredCount() != 1) return 41;
  if (!engine.Drive(DriveMode::kPoll, dispatcher) || engine.DeferredCount() != 0) return 42;
  const auto* encoded = reinterpret_cast<const abi::FlatSqe128*>(&engine.Backend().Sq().SlotAt(4));
  if (engine.Backend().Sq().SlotAt(3).user_data != 0 || !std::equal(bytes.begin(), bytes.end(), encoded->command().begin()))
    return 43;
  std::array<UserDataToken, 3> queued{};
  for (auto& token : queued) {
    auto armed = engine.Arm(1, ops::PrepareNop());
    if (!armed) return 44;
    token = *armed;
  }
  for (auto token : queued)
    if (!engine.Cancel(token)) return 45;
  engine.Backend().ScriptCqe(*chain, 0);
  if (!engine.Drive(DriveMode::kPoll, dispatcher) || engine.InFlight() != 0) return 46;
  return engine.DrainBlocking(dispatcher) ? 0 : 47;
}
int DrainTimeout() {
  ScriptedProactor engine{std::in_place, 8U, 16U};
  auto token = engine.Arm(1, ops::PrepareRecv(-1, nullptr, 0));
  if (!token) return 20;
  Sink sink;
  SingleDispatcher dispatcher{sink};
  if (engine.DrainBlocking(dispatcher, 0).error() != UringErrorCode::kDrainTimeout || engine.InFlight() != 1) return 21;
  engine.Backend().ScriptCqe(*token, -125);
  engine.Backend().ScriptCqe(engine.InternalCancelToken(), 0);
  if (!engine.DrainBlocking(dispatcher) || engine.Stage() != DrainStage::kClosed) return 22;
  return 0;
}
struct SmallStackResult {
  int status{1};
};
void* SmallStackWorker(void* context) noexcept {
  auto& result = *static_cast<SmallStackResult*>(context);
  Sink sink;
  SingleDispatcher dispatcher{sink};
  if (const int gate = test_support::SkipBelowKernel(6, 16); gate > 0) {
    result.status = 51;
    return nullptr;
  } else if (gate < 0) {
    auto created = Proactor::Create(64);
    if (!created) {
      result.status = 51;
      return nullptr;
    }
    auto& engine = *created;
    if (!engine.Arm(1, ops::PrepareNop())) {
      result.status = 52;
      return nullptr;
    }
    while (engine.InFlight() != 0) {
      if (!engine.Drive(DriveMode::kWait, dispatcher)) sys::Panic("small-stack kernel drive failed");
    }
    if (sink.count != 1 || !engine.DrainBlocking(dispatcher)) {
      result.status = 53;
      return nullptr;
    }
  }
  sink.count = 0;
  {
    ScriptedProactor engine{std::in_place, 8U, 16U};
    if (!engine.Arm(1, ops::PrepareNop())) {
      result.status = 54;
      return nullptr;
    }
    if (!engine.Drive(DriveMode::kPoll, dispatcher)) sys::Panic("small-stack scripted drive failed");
    if (sink.count != 1 || !engine.DrainBlocking(dispatcher)) {
      result.status = 55;
      return nullptr;
    }
  }
  result.status = 0;
  return nullptr;
}
int SmallStack() {
  static_assert(sizeof(Proactor) <= 8192);
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes)) return 56;
  const auto configured = pthread_attr_setstacksize(&attributes, 131072);
  if (configured) {
    static_cast<void>(pthread_attr_destroy(&attributes));
    return 57;
  }
  SmallStackResult result;
  pthread_t worker;
  const auto started = pthread_create(&worker, &attributes, SmallStackWorker, &result);
  const auto destroyed = pthread_attr_destroy(&attributes);
  if (started) return 58;
  if (pthread_join(worker, nullptr) || destroyed) return 59;
  return result.status;
}
int main() {
  if (auto result = ActiveCapacity()) return result;
  if (auto result = DeferredCapacity()) return result;
  if (auto result = DeferredRollback()) return result;
  if (auto result = DeferredWideChain()) return result;
  if (auto result = DrainTimeout()) return result;
  return SmallStack();
}
