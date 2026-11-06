#include <array>
#include <liburing/core/dispatcher.hpp>
using namespace liburing;
struct Log {
  std::array<int, 8> order{};
  std::size_t count{};
};
template <std::uint8_t Id>
struct Sink {
  static constexpr auto kSubsystemId = Id;
  Log* log;
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept { return caps::RequiredCaps::For<abi::kOpNop>(); }
  void OnCompletions(std::span<const Delivery> batch) noexcept {
    log->order[log->count++] = Id * 100 + static_cast<int>(batch.size());
  }
};
int main() {
  Log log;
  Sink<1> a{&log};
  Sink<2> b{&log};
  StaticDispatcher dispatch{a, b};
  std::array<Delivery, 5> deliveries{};
  constexpr std::array<std::uint8_t, 5> ids{1, 1, 2, 1, 2};
  for (std::size_t i = 0; i < ids.size(); ++i) deliveries[i].completion.token = MakeToken(1, 0, ids[i], 1);
  if (!dispatch.DispatchBatch(deliveries)) return 1;
  if (log.count != 4 || log.order[0] != 102 || log.order[1] != 201 || log.order[2] != 101 || log.order[3] != 201) return 2;
  DynamicDispatcher dynamic;
  if (!dynamic.Register(Describe(a)) || !dynamic.Register(Describe(b))) return 3;
  if (dynamic.Register(Describe(a)).error() != UringErrorCode::kBusy) return 4;
  log.count = 0;
  if (!dynamic.DispatchBatch(deliveries) || log.count != 4) return 5;
  if (!dynamic.Detach(2)) return 6;
  if (dynamic.DispatchBatch(deliveries).error() != UringErrorCode::kNoEntry) return 7;
  SubsystemDescriptor stateless{3, {}, {[](void*, std::span<const Delivery>) noexcept {}, nullptr}};
  if (!dynamic.Register(stateless)) return 9;
  SingleDispatcher single{a};
  log.count = 0;
  if (!single.DispatchBatch(std::span<const Delivery>{deliveries.data(), 2}) || log.order[0] != 102) return 8;
  constexpr auto required = decltype(dispatch)::RequiredCaps();
  static_assert(required.opcodes[0].test(abi::kOpNop));
  static_assert(plugins::Subsystem<Sink<1>>);
  static_assert(!UniqueSubsystemIds<Sink<1>, Sink<1>>());
  return 0;
}
