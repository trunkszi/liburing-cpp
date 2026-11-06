#include <liburing/core/payload_codec.hpp>
#include <liburing/core/proactor.hpp>
#include <liburing/core/route_dispatcher.hpp>
using namespace liburing;
struct Header {
  std::uint8_t kind;
  std::uint16_t flags;
  std::uint32_t len;
};
struct Small {
  std::uint8_t first;
  std::uint8_t second;
};
static_assert(DecodePayload<Small>(0x0102)->first == 1);
static_assert(!DecodePayload<Small>(0x00ab000000000102ULL));
struct Signed {
  std::int8_t value;
};
struct TooWide {
  std::uint64_t value;
};
static_assert(PayloadEncodable<Header> && !PayloadEncodable<Signed> && !PayloadEncodable<TooWide>);
struct BoolField {
  bool value;
};
struct FloatField {
  float value;
};
struct PointerField {
  void* value;
};
static_assert(!PayloadEncodable<BoolField> && !PayloadEncodable<FloatField> && !PayloadEncodable<PointerField>);
constexpr Header kHeader{0xff, 0xffff, 0xffffffff};
static_assert(EncodePayload(Header{0x12, 0x3456, 0x789abcde}) == 0x00123456789abcdeULL);
static_assert(EncodePayload(kHeader) == 0x00ffffffffffffffULL);
static_assert(DecodePayload<Header>(EncodePayload(kHeader))->len == 0xffffffffU);
static_assert(!DecodePayload<Header>(0x0100000000000000ULL));
enum class Routes : std::uint8_t { kFile = 1, kTimer = 2 };
enum class BadRoutes : std::uint8_t { kReserved = 0 };
enum class Alias : std::uint8_t { kOne = 1, kAlias = 1 };
static_assert(SubsystemsEnum<Routes> && !SubsystemsEnum<BadRoutes> && !SubsystemsEnum<Alias>);
static_assert(RouteFromSubsystemId<Routes>(2) == Routes::kTimer && !RouteFromSubsystemId<Routes>(9));
int main() {
  auto engine = Proactor::Builder().Entries(8).BuildScripted();
  if (!engine) return 1;
  unsigned state = 0;
  auto route = MakeRouteDispatcher<Routes>(state, [](Routes, unsigned& count, const Delivery&) noexcept { ++count; });
  if (!engine->Arm(1, ops::PrepareNop()) || !engine->Arm(2, ops::PrepareNop())) return 2;
  if (!engine->Tick(route) || state != 2) return 3;
  auto fn = MakeFnDispatcher([&](const Delivery&) noexcept -> Result<void> {
    ++state;
    return {};
  });
  if (!engine->Arm(1, ops::PrepareNop()) || !engine->Tick(fn) || state != 3) return 4;
  if (!engine->Shutdown()) return 5;
  unsigned routed = 0;
  auto isolate = MakeRouteDispatcher<Routes>(routed, [](Routes, unsigned& value, const Delivery&) noexcept { ++value; });
  std::array<Delivery, 3> mixed{};
  mixed[0].completion.token = MakeToken(1, 0, 9, 1);
  mixed[1].completion.token = MakeToken(2, 0, 1, 1);
  mixed[2].completion.token = MakeToken(3, 0, 2, 1);
  auto isolated = isolate.DispatchBatch(mixed);
  if (isolated || isolated.error() != UringErrorCode::kNoEntry || routed != 2 || isolate.UnroutedCount() != 1) return 6;
  auto failure =
      MakeRouteDispatcher<Routes>(routed, [](Routes route, unsigned& value, const Delivery&) noexcept -> Result<void> {
        if (route == Routes::kFile) return std::unexpected(UringErrorCode::kBusy);
        ++value;
        return {};
      });
  auto failed = failure.DispatchBatch(std::span<const Delivery>{mixed}.subspan(1));
  if (failed || failed.error() != UringErrorCode::kBusy || routed != 3) return 7;
  return OperationPayload::New(OperationPayload::kMaxRaw + 1) ? 8 : 0;
}
