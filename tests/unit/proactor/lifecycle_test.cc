#include <array>
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
  std::size_t calls{};
  ScriptedProactor* engine{};
  bool reentrant_rejected{};
  bool head_released{true};
  bool stage_callback{};
  bool staged{};
  bool callback_syscall_free{true};
  UserDataToken cancel_target{};
  bool canceled{};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept { return caps::RequiredCaps::For<abi::kOpNop>(); }
  void OnCompletions(std::span<const Delivery> deliveries) noexcept {
    ++calls;
    count += deliveries.size();
    if (engine) {
      head_released &= engine->Backend().Cq().SharedHead() >= count;
      SingleDispatcher dispatcher{*this};
      auto nested = engine->Drive(DriveMode::kPoll, dispatcher);
      reentrant_rejected = !nested && nested.error() == UringErrorCode::kReentrantDrive;
      if (cancel_target != kNullToken && !canceled) {
        const auto before = engine->Backend().LastRegisterOpcode();
        canceled = engine->Cancel(cancel_target).has_value();
        callback_syscall_free &= engine->Backend().LastRegisterOpcode() == before;
      }
      if (stage_callback && !staged) {
        const auto head = engine->Backend().Sq().Head();
        staged = engine->ArmOneShot(1, ops::PrepareNop()).has_value();
        callback_syscall_free &= engine->Backend().Sq().Head() == head && engine->Backend().Cq().Available() == 0;
      }
    }
  }
};
int Burst() {
  ScriptedProactor engine{std::in_place, 8U, 64U};
  engine.Backend().SetAutoCompleteNops(false);
  auto token = engine.ArmMultishot(1, ops::PrepareRecvMultishot(-1, nullptr, 0, 0, 0));
  REQUIRE(token);
  Sink sink;
  sink.engine = &engine;
  SingleDispatcher dispatcher{sink};
  for (int i = 0; i < 40; ++i) engine.Backend().ScriptCqe(*token, i, abi::kCqeMore);
  engine.Backend().ScriptCqe(*token, 0);
  while (sink.count < 41) REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(sink.count == 41 && sink.calls == 3 && engine.InFlight() == 0);
  REQUIRE(sink.reentrant_rejected && sink.head_released);
  return 0;
}
int ZeroCopyCancel() {
  ScriptedProactor engine{std::in_place, 8U, 16U};
  engine.Backend().SetAutoCompleteNops(false);
  auto token = engine.ArmZeroCopy(1, ops::PrepareSendZc(-1, nullptr, 0));
  REQUIRE(token);
  Sink sink;
  SingleDispatcher dispatcher{sink};
  REQUIRE(engine.Cancel(*token));
  REQUIRE(engine.InFlight() == 1);
  engine.Backend().ScriptCqe(*token, 0, abi::kCqeNotif);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.InFlight() == 1);
  engine.Backend().ScriptCqe(*token, -125, abi::kCqeMore);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.InFlight() == 0 && sink.count == 2);
  engine.Backend().ScriptCqe(engine.InternalCancelToken(), 0);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  return 0;
}
int DeferredFifo() {
  ScriptedProactor engine{std::in_place, 2U, 16U};
  engine.Backend().SetAutoCompleteNops(false);
  engine.Backend().SetConsumeSqOnSubmit(false);
  auto a = engine.ArmOneShot(1, ops::PrepareNop());
  auto b = engine.ArmOneShot(1, ops::PrepareNop());
  auto c = engine.ArmOneShot(1, ops::PrepareNop());
  auto d = engine.ArmOneShot(1, ops::PrepareNop());
  REQUIRE(a && b && c && d && engine.DeferredCount() == 2);
  Sink sink;
  SingleDispatcher dispatcher{sink};
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  engine.Backend().AdvanceSqHead(2);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.DeferredCount() == 0);
  REQUIRE(engine.Backend().Sq().SlotAt(2).user_data == c->raw());
  REQUIRE(engine.Backend().Sq().SlotAt(3).user_data == d->raw());
  for (auto token : {*a, *b, *c, *d}) engine.Backend().ScriptCqe(token, 0);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.InFlight() == 0);
  return 0;
}
int DrainAndCallback() {
  ScriptedProactor engine{std::in_place, 8U, 16U};
  Sink sink;
  sink.engine = &engine;
  sink.stage_callback = true;
  SingleDispatcher dispatcher{sink};
  REQUIRE(engine.ArmOneShot(1, ops::PrepareNop()));
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(sink.staged && sink.callback_syscall_free && engine.InFlight() == 1);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.InFlight() == 0 && sink.count == 2);
  REQUIRE(engine.Stage() == DrainStage::kRunning);
  REQUIRE(engine.BeginDrain());
  REQUIRE(engine.Stage() == DrainStage::kStopping);
  REQUIRE(!engine.ArmOneShot(1, ops::PrepareNop()));
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.Stage() == DrainStage::kQuiescent);
  REQUIRE(engine.Close());
  REQUIRE(engine.Stage() == DrainStage::kClosed);
  return 0;
}
int MixedChain() {
  ScriptedProactor engine{std::in_place, 8U, 32U, RingMode::kMixed64_128_16_32};
  Sink sink;
  SingleDispatcher dispatcher{sink};
  for (int i = 0; i < 7; ++i) REQUIRE(engine.ArmOneShot(1, ops::PrepareNop()));
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  auto chain = engine.ArmChain(1, ops::PrepareNop128(), ops::PrepareNop());
  REQUIRE(chain);
  REQUIRE(engine.Backend().Sq().SlotAt(7).user_data == 0);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.InFlight() == 0 && sink.count == 9);
  return 0;
}
struct OtherSink {
  static constexpr std::uint8_t kSubsystemId = 2;
  std::size_t count{};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept { return {}; }
  void OnCompletions(std::span<const Delivery> deliveries) noexcept { count += deliveries.size(); }
};
int CallbackCancel() {
  ScriptedProactor engine{std::in_place, 2U, 16U};
  engine.Backend().SetAutoCompleteNops(false);
  engine.Backend().SetConsumeSqOnSubmit(false);
  auto a = engine.ArmOneShot(1, ops::PrepareNop());
  auto b = engine.ArmMultishot(2, ops::PrepareRecvMultishot(-1, nullptr, 0, 0, 0));
  REQUIRE(a && b);
  Sink sink;
  sink.engine = &engine;
  sink.cancel_target = *b;
  OtherSink other;
  StaticDispatcher dispatcher{sink, other};
  engine.Backend().ScriptCqe(*a, 0);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(sink.canceled && sink.callback_syscall_free && engine.InFlight() == 1);
  REQUIRE(engine.Backend().LastRegisterOpcode() == 0);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.Backend().LastRegisterOpcode() == abi::kRegisterSyncCancel);
  engine.Backend().ScriptCqe(*b, -125);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(other.count == 1 && engine.InFlight() == 0);
  return 0;
}
int main() {
  if (auto result = Burst()) return result;
  if (auto result = ZeroCopyCancel()) return result;
  if (auto result = DeferredFifo()) return result;
  if (auto result = DrainAndCallback()) return result;
  if (auto result = MixedChain()) return result;
  return CallbackCancel();
}
