#include <liburing/core/op_header.hpp>
using namespace liburing;

constexpr bool zc_notif_before_primary_retires(std::uint32_t generation = 1) {
  auto h = MakeHeader(OpKind::kZeroCopy, generation);
  h = Advance(h, CqeClass::kNotification);
  if (State(h) != OpState::kAwaitNotif) return false;
  h = Advance(h, CqeClass::kMore);
  return State(h) == OpState::kFree;
}
constexpr bool zc_primary_without_more_retires(std::uint32_t generation = 1) {
  return State(Advance(MakeHeader(OpKind::kZeroCopy, generation), CqeClass::kTerminal)) == OpState::kFree;
}
constexpr bool zc_primary_more_waits(std::uint32_t generation = 1) {
  auto h = Advance(Pin(MakeHeader(OpKind::kZeroCopy, generation)), CqeClass::kMore);
  if (State(h) != OpState::kAwaitNotif) return false;
  h = Unpin(h);
  h = Advance(Pin(h), CqeClass::kNotification);
  if (State(h) != OpState::kRetiring) return false;
  return State(Unpin(h)) == OpState::kFree;
}
constexpr bool chain_member_countdown_retires(std::uint32_t generation = 1) {
  auto h = MakeHeader(OpKind::kChain, generation, 3);
  h = Advance(h, CqeClass::kTerminal, 0);
  if (State(h) != OpState::kArmed) return false;
  h = Advance(h, CqeClass::kTerminal, 1);
  if (State(h) != OpState::kArmed) return false;
  return State(Advance(h, CqeClass::kTerminal, 2)) == OpState::kFree;
}
constexpr bool multishot_more_retains_slot(std::uint32_t generation = 1) {
  auto h = Advance(MakeHeader(OpKind::kMultishot, generation), CqeClass::kMore);
  if (State(h) != OpState::kArmed) return false;
  h = Advance(Pin(h), CqeClass::kTerminal);
  if (State(h) != OpState::kRetiring) return false;
  h = Unpin(h);
  return State(h) == OpState::kFree && Generation(h) == generation;
}
constexpr bool generation_exhaustion_retires_slot() {
  auto h = Advance(Pin(MakeHeader(OpKind::kOneShot, 0xffffffffU)), CqeClass::kTerminal);
  return State(h) == OpState::kRetiring && State(Unpin(h)) == OpState::kExhausted;
}
static_assert(zc_notif_before_primary_retires());
static_assert(zc_primary_without_more_retires());
static_assert(zc_primary_more_waits());
static_assert(chain_member_countdown_retires());
static_assert(multishot_more_retains_slot());
static_assert(generation_exhaustion_retires_slot());
int main() {
  volatile std::uint32_t generation = 17;
  return zc_notif_before_primary_retires(generation) && zc_primary_without_more_retires(generation) &&
                 zc_primary_more_waits(generation) && chain_member_countdown_retires(generation) &&
                 multishot_more_retains_slot(generation) && generation_exhaustion_retires_slot()
             ? 0
             : 1;
}
