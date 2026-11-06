#include <array>
#include <liburing/core/proactor.hpp>
using namespace liburing;
template <std::uint8_t Id>
struct Sink {
  static constexpr auto kSubsystemId = Id;
  std::size_t count{};
  std::size_t calls{};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept { return {}; }
  void OnCompletions(std::span<const Delivery> deliveries) noexcept {
    ++calls;
    count += deliveries.size();
  }
};
template <class Dispatcher>
int CheckRuns(Dispatcher& dispatcher, Sink<1>& sink) {
  std::array<Delivery, 6> batch{};
  constexpr std::array<std::uint8_t, 6> ids{9, 1, 1, 9, 1, 9};
  for (std::size_t i = 0; i < ids.size(); ++i) batch[i].completion.token = MakeToken(1, 0, ids[i], 1);
  auto result = dispatcher.DispatchBatch(batch);
  if (result || result.error() != UringErrorCode::kNoEntry || sink.count != 3 || sink.calls != 2 ||
      dispatcher.UnroutedCount() != 3)
    return 1;
  if (!dispatcher.CanRoute(1) || dispatcher.CanRoute(9)) return 2;
  return 0;
}
int main() {
  Sink<1> a;
  SingleDispatcher single{a};
  if (CheckRuns(single, a)) return 1;
  Sink<1> b;
  StaticDispatcher statically_bound{b};
  if (CheckRuns(statically_bound, b)) return 2;
  Sink<1> c;
  DynamicDispatcher dynamically_bound;
  if (!dynamically_bound.Register(Describe(c)) || CheckRuns(dynamically_bound, c)) return 3;
  ScriptedProactor engine{std::in_place, 8U, 16U};
  auto stray = engine.Arm(9, ops::PrepareNop());
  auto valid = engine.Arm(1, ops::PrepareNop());
  if (!stray || !valid) return 4;
  engine.BindDispatcher(statically_bound);
  if (engine.Arm(9, ops::PrepareNop()).error() != UringErrorCode::kNoEntry) return 5;
  auto drove = engine.Drive(DriveMode::kPoll, statically_bound);
  if (drove || drove.error() != UringErrorCode::kNoEntry || b.count != 4 || engine.InFlight() != 0) return 6;
  return engine.DrainBlocking(statically_bound) ? 0 : 7;
}
