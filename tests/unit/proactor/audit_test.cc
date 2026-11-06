#include <cstdio>
#include <liburing/core/proactor.hpp>
using namespace liburing;
#define REQUIRE(condition)                                \
  do {                                                    \
    if (!(condition)) {                                   \
      std::fprintf(stderr, "failed line %d\n", __LINE__); \
      return __LINE__;                                    \
    }                                                     \
  } while (false)
struct AuditSink {
  static constexpr std::uint8_t kSubsystemId = 1;
  std::size_t count{};
  std::size_t canceled{};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept { return {}; }
  void OnCompletions(std::span<const Delivery> values) noexcept {
    count += values.size();
    for (const auto& value : values) canceled += value.completion.result == -125;
  }
};
int DeferredGeometry() {
  ScriptedProactor engine{std::in_place, 8U, 32U};
  AuditSink sink;
  SingleDispatcher dispatcher{sink};
  for (int i = 0; i < 9; ++i) REQUIRE(engine.Arm(1, ops::PrepareNop()));
  const auto invalid = engine.Arm128(1, ops::PrepareNop128());
  REQUIRE(!invalid && invalid.error() == UringErrorCode::kNotSupported);
  REQUIRE(engine.InFlight() == 9 && engine.DeferredCount() == 1);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.InFlight() == 0 && sink.count == 9);
  ScriptedProactor tiny{std::in_place, 2U, 16U, RingMode::kMixed64_128_16_32};
  REQUIRE(tiny.Arm(1, ops::PrepareNop()) && tiny.Arm(1, ops::PrepareNop()));
  REQUIRE(tiny.Arm(1, ops::PrepareNop()));
  const auto oversized = tiny.ArmChain(1, ops::PrepareNop128(), ops::PrepareNop());
  REQUIRE(!oversized && oversized.error() == UringErrorCode::kInvalid);
  REQUIRE(tiny.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(tiny.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(tiny.DrainBlocking(dispatcher));
  return 0;
}
int DeferredReplayEvicts() {
  ScriptedProactor engine{std::in_place, 4U, 32U, RingMode::kMixed64_128_16_32};
  AuditSink sink;
  SingleDispatcher dispatcher{sink};
  for (int i = 0; i < 7; ++i) REQUIRE(engine.Arm(1, ops::PrepareNop()));
  auto invalid = engine.ArmChain(1, ops::PrepareNop128(), ops::PrepareNop128());
  REQUIRE(!invalid && invalid.error() == UringErrorCode::kInvalid);
  auto chain = engine.ArmChain(1, ops::PrepareNop128(), ops::PrepareNop());
  REQUIRE(chain && engine.DeferredCount() == 4);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.DeferredCount() == 1 && engine.Contains(*chain));
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.Contains(*chain) && engine.DeferredCount() == 0);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.InFlight() == 0 && sink.count == 9);
  return 0;
}
int DrainUnrouted() {
  ScriptedProactor engine{std::in_place, 8U, 32U};
  AuditSink sink;
  SingleDispatcher dispatcher{sink};
  REQUIRE(engine.Arm(9, ops::PrepareNop()) && engine.Arm(1, ops::PrepareNop()));
  REQUIRE(engine.DrainBlocking(dispatcher));
  REQUIRE(engine.Stage() == DrainStage::kClosed && engine.InFlight() == 0 && dispatcher.UnroutedCount() == 1);
  return 0;
}
int DetachReattach() {
  ScriptedProactor engine{std::in_place, 8U, 32U};
  AuditSink sink;
  DynamicDispatcher dispatcher;
  REQUIRE(dispatcher.Register(Describe(sink)));
  engine.BindDispatcher(dispatcher);
  REQUIRE(engine.EnqueueControl(1, {{kNullToken, engine.Instance()}, 1, ControlKind::kDetach, 0}));
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(!dispatcher.CanRoute(1));
  REQUIRE(dispatcher.Register(Describe(sink)));
  engine.BindDispatcher(dispatcher);
  REQUIRE(engine.Arm(1, ops::PrepareNop()));
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher) && engine.InFlight() == 0);
  return 0;
}
int CancelDeduplicates() {
  ScriptedProactor engine{std::in_place, 8U, 32U};
  engine.Backend().SetAutoCompleteNops(false);
  AuditSink sink;
  SingleDispatcher dispatcher{sink};
  auto token = engine.Arm(1, ops::PrepareNop());
  REQUIRE(token && engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.Cancel(*token) && engine.Drive(DriveMode::kPoll, dispatcher));
  const auto tail = engine.Backend().Sq().Tail();
  engine.Backend().ScriptCqe(engine.InternalCancelToken(), -2);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.Cancel(*token) && engine.Cancel(*token));
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.Backend().Sq().Tail() == tail + 1 && engine.BeginDrain());
  engine.Backend().ScriptCqe(*token, -125);
  engine.Backend().ScriptCqe(MakeToken(0, 0, 0, 1), 0);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.Backend().Sq().Tail() == tail + 1 && engine.Stage() == DrainStage::kDraining);
  engine.Backend().ScriptCqe(engine.InternalCancelToken(), 0);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher));
  REQUIRE(engine.Stage() == DrainStage::kQuiescent && engine.Close());
  return 0;
}
struct FaultRing {
  ScriptedRing ring;
  explicit FaultRing(std::uint32_t sq, std::uint32_t cq) : ring(sq, cq) {}
  [[nodiscard]] auto& Sq() noexcept { return ring.Sq(); }
  [[nodiscard]] const auto& Sq() const noexcept { return ring.Sq(); }
  [[nodiscard]] auto& Cq() noexcept { return ring.Cq(); }
  [[nodiscard]] auto CqMode() const noexcept { return ring.CqMode(); }
  [[nodiscard]] auto Submit() noexcept { return ring.Submit(); }
  [[nodiscard]] auto SubmitAndWait(std::uint32_t count) noexcept { return ring.SubmitAndWait(count); }
  [[nodiscard]] static Result<std::int64_t> Register(std::uint32_t, const void*, std::uint32_t) noexcept {
    return std::unexpected(UringErrorCode::kNotSupported);
  }
};
int MaintenanceProgress() {
  BasicProactor<FaultRing> engine{std::in_place, 2U, 16U};
  auto& ring = engine.Backend().ring;
  ring.SetAutoCompleteNops(false);
  ring.SetConsumeSqOnSubmit(false);
  AuditSink sink;
  DynamicDispatcher dispatcher;
  REQUIRE(dispatcher.Register(Describe(sink)));
  auto target = engine.Arm(1, ops::PrepareNop());
  auto good = engine.Arm(1, ops::PrepareNop());
  REQUIRE(target && good && engine.Cancel(*target));
  ring.ScriptCqe(*good, 0);
  auto failed = engine.Drive(DriveMode::kPoll, dispatcher);
  REQUIRE(!failed && failed.error() == UringErrorCode::kNotSupported);
  REQUIRE(sink.count == 1 && engine.InFlight() == 1);
  ring.ScriptCqe(*target, -125);
  REQUIRE(engine.Drive(DriveMode::kPoll, dispatcher) && engine.InFlight() == 0);
  ScriptedProactor controlled{std::in_place, 8U, 16U};
  REQUIRE(controlled.Arm(1, ops::PrepareNop()));
  REQUIRE(controlled.EnqueueControl(1, {{kNullToken, controlled.Instance()}, 2, ControlKind::kDetach, 0}));
  auto control_error = controlled.Drive(DriveMode::kPoll, dispatcher);
  REQUIRE(!control_error && control_error.error() == UringErrorCode::kNoEntry);
  REQUIRE(controlled.InFlight() == 0 && sink.count == 3);
  return 0;
}
struct ChangingRing {
  ScriptedRing initial{1, 16, RingMode::kFull128_32};
  ScriptedRing changed{1, 16};
  bool switch_mode{};
  [[nodiscard]] auto& Sq() noexcept { return switch_mode ? changed.Sq() : initial.Sq(); }
  [[nodiscard]] const auto& Sq() const noexcept { return switch_mode ? changed.Sq() : initial.Sq(); }
  [[nodiscard]] auto& Cq() noexcept { return changed.Cq(); }
  [[nodiscard]] auto CqMode() const noexcept { return changed.CqMode(); }
  [[nodiscard]] auto Submit() noexcept { return changed.Submit(); }
  [[nodiscard]] auto SubmitAndWait(std::uint32_t count) noexcept { return changed.SubmitAndWait(count); }
  [[nodiscard]] auto Register(std::uint32_t op, const void* arg, std::uint32_t count) noexcept {
    return changed.Register(op, arg, count);
  }
};
int SyntheticEviction() {
  BasicProactor<ChangingRing> engine{std::in_place};
  auto first = engine.Arm(1, ops::PrepareNop());
  auto failed = engine.Arm(1, ops::PrepareNop128());
  REQUIRE(first && failed && engine.DeferredCount() == 1);
  engine.Backend().switch_mode = true;
  engine.Backend().changed.ScriptCqe(*first, 0);
  AuditSink sink;
  SingleDispatcher dispatcher{sink};
  auto result = engine.Drive(DriveMode::kPoll, dispatcher);
  REQUIRE(!result && result.error() == UringErrorCode::kNotSupported);
  REQUIRE(sink.count == 2 && sink.canceled == 1 && engine.InFlight() == 0 && !engine.Contains(*failed));
  return 0;
}
int main() {
  if (auto result = DeferredGeometry()) return result;
  if (auto result = DeferredReplayEvicts()) return result;
  if (auto result = DrainUnrouted()) return result;
  if (auto result = DetachReattach()) return result;
  if (auto result = CancelDeduplicates()) return result;
  if (auto result = MaintenanceProgress()) return result;
  return SyntheticEviction();
}
