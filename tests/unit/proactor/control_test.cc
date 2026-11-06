#include <liburing/core/proactor.hpp>
using namespace liburing;
struct Sink {
  static constexpr std::uint8_t kSubsystemId = 7;
  std::size_t count{};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept { return {}; }
  void OnCompletions(std::span<const Delivery> batch) noexcept { count += batch.size(); }
};
int ResizeControl() {
  ScriptedProactor engine{std::in_place, 8U, 16U};
  Sink sink;
  SingleDispatcher dispatcher{sink};
  if (!engine.ArmOneShot(7, ops::PrepareNop())) return 20;
  ControlCommand command{{kNullToken, engine.Instance()}, 16, ControlKind::kResize, 0};
  if (!engine.EnqueueControl(19, command)) return 21;
  if (!engine.Drive(DriveMode::kPoll, dispatcher)) return 22;
  if (engine.Backend().Sq().RingEntries() != 8 || engine.InFlight() != 0) return 23;
  if (!engine.Drive(DriveMode::kPoll, dispatcher)) return 24;
  if (engine.Backend().Sq().RingEntries() != 16) return 25;
  if (!engine.ArmOneShot(7, ops::PrepareNop()) || !engine.Drive(DriveMode::kPoll, dispatcher)) return 26;
  return engine.InFlight() == 0 && sink.count == 2 ? 0 : 27;
}
int main() {
  ScriptedProactor engine{std::in_place, 2U, 16U};
  engine.Backend().SetAutoCompleteNops(false);
  engine.Backend().SetConsumeSqOnSubmit(false);
  Sink sink;
  DynamicDispatcher dispatcher;
  if (!dispatcher.Register(Describe(sink))) return 1;
  auto a = engine.ArmOneShot(7, ops::PrepareNop());
  auto b = engine.ArmOneShot(7, ops::PrepareNop());
  auto deferred = engine.ArmOneShot(7, ops::PrepareNop());
  if (!a || !b || !deferred) return 2;
  if (!engine.Cancel(*deferred) || engine.Contains(*deferred) || engine.DeferredCount() != 0) return 3;
  if (!engine.BeginDrain() || !engine.Drive(DriveMode::kPoll, dispatcher)) return 4;
  if (engine.Stage() != DrainStage::kDraining || engine.InFlight() != 2) return 5;
  if (engine.Backend().LastRegisterOpcode() != abi::kRegisterSyncCancel) return 6;
  engine.Backend().ScriptCqe(*a, -125);
  engine.Backend().ScriptCqe(*b, -125);
  if (!engine.Drive(DriveMode::kPoll, dispatcher) || engine.Stage() != DrainStage::kQuiescent) return 7;
  if (!engine.Close()) return 8;

  ScriptedProactor second{std::in_place, 8U, 16U};
  second.Backend().SetAutoCompleteNops(false);
  auto token = second.ArmOneShot(7, ops::PrepareNop());
  if (!token) return 9;
  if (second.Cancel(OperationKey{*token, engine.Instance()}).error() != UringErrorCode::kInvalidToken) return 10;
  ControlCommand detach{{*token, second.Instance()}, 7, ControlKind::kDetach, 0};
  if (!second.EnqueueControl(17, detach)) return 11;
  if (!second.Drive(DriveMode::kPoll, dispatcher)) return 12;
  const bool issued_cancel = second.Backend().Sq().Tail() == 2;
  second.Backend().ScriptCqe(*token, -125);
  if (issued_cancel) second.Backend().ScriptCqe(second.InternalCancelToken(), 0);
  if (!second.Drive(DriveMode::kPoll, dispatcher) || !second.Drive(DriveMode::kPoll, dispatcher)) return 13;
  if (!issued_cancel) return 14;
  Delivery delivery{{*token, second.Instance()}, {*token, 0, 0}};
  if (dispatcher.DispatchBatch({&delivery, 1}).error() != UringErrorCode::kNoEntry) return 15;
  return ResizeControl();
}
