#include <liburing/core/proactor.hpp>
#include <liburing/ops/net.hpp>
using namespace liburing;
#define REQUIRE(condition)             \
  do {                                 \
    if (!(condition)) return __LINE__; \
  } while (false)
struct Sink {
  static constexpr std::uint8_t kSubsystemId = 1;
  std::size_t count{};
  ScriptedProactor* engine{};
  bool extension_okay{};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept { return {}; }
  void OnCompletions(std::span<const Delivery> deliveries) noexcept {
    count += deliveries.size();
    if (engine) {
      const auto copied = engine->CurrentCompletions();
      const auto* extended = engine->Extended(deliveries.front());
      const auto copy = deliveries.front();
      extension_okay = extended && extended->extra[0] == 0x12345678 && !engine->Extended(copy) && copied.size() == 1 &&
                       copied[0].extra[0] == 0x12345678 && copied[0].extra[1] == 0x87654321;
    }
  }
};
int ExtendedCompletion() {
  ScriptedProactor engine{std::in_place, 8U, 16U, RingMode::kFull128_32};
  engine.Backend().SetAutoCompleteNops(false);
  auto token = engine.Arm128(1, ops::PrepareNop128());
  REQUIRE(token);
  Sink sink;
  sink.engine = &engine;
  SingleDispatcher dispatcher{sink};
  engine.Backend().ScriptCqe32(*token, 0, 0, 0x12345678, 0x87654321);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(sink.extension_okay && engine.CurrentCompletions().empty() && engine.InFlight() == 0);
  return 0;
}
int ChainNotification() {
  ScriptedProactor engine{std::in_place, 8U, 16U};
  engine.Backend().SetAutoCompleteNops(false);
  auto token = engine.ArmChain(1, ops::PrepareSendZc(-1, nullptr, 0), ops::PrepareNop());
  REQUIRE(token);
  const auto second = ring::WithChainMember(*token, 1);
  engine.Backend().ScriptCqe(*token, 0, abi::kCqeNotif);
  engine.Backend().ScriptCqe(second, 0);
  engine.Backend().ScriptCqe(*token, 0, abi::kCqeMore);
  Sink sink;
  SingleDispatcher dispatcher{sink};
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(sink.count == 3 && engine.InFlight() == 0);
  auto next = engine.ArmOneShot(1, ops::PrepareNop());
  REQUIRE(next && Generation(*next) != Generation(*token));
  const auto spoof = MakeToken(Slot(*next), 0, 2, Generation(*next));
  REQUIRE(!engine.Contains(spoof));
  engine.Backend().ScriptCqe(*token, 0);
  engine.Backend().ScriptCqe(spoof, 0);
  engine.Backend().ScriptCqe(*next, 0);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(sink.count == 4 && engine.InFlight() == 0);
  return 0;
}
int ExhaustAndDrain() {
  BasicProactor<ScriptedRing, 6, 6> engine{std::in_place, 8U, 64U};
  for (std::size_t i = 1; i < decltype(engine)::kCapacity; ++i) REQUIRE(engine.ArmOneShot(1, ops::PrepareNop()));
  REQUIRE(engine.ArmOneShot(1, ops::PrepareNop()).error() == UringErrorCode::kSlabExhausted);
  Sink sink;
  SingleDispatcher dispatcher{sink};
  REQUIRE(engine.BeginDrain());
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.InFlight() == 0 && engine.DeferredCount() == 0);
  for (int i = 0; i < 7; ++i) engine.Backend().ScriptCqe(engine.InternalCancelToken(), 0);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.Stage() == DrainStage::kQuiescent && engine.Close());
  return 0;
}
int main() {
  if (auto result = ExtendedCompletion()) return result;
  if (auto result = ChainNotification()) return result;
  return ExhaustAndDrain();
}
