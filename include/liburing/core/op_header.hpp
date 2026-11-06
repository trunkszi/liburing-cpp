/** @file @brief Slots retain kernel ownership through every CQE and pinned callback. */
#pragma once
#include <array>
#include <cstdint>
#include <liburing/core/completion.hpp>
#include <limits>

namespace liburing {
enum class OpState : std::uint8_t { kFree, kReserved, kDeferred, kArmed, kRetiring, kExhausted, kAwaitNotif };
enum class OpKind : std::uint8_t { kOneShot, kMultishot, kZeroCopy, kChain };
enum class CqeClass : std::uint8_t { kTerminal, kMore, kNotification };
struct OpHeaderSpec {
  using Word = std::uint64_t;
  OpState state[[= uarch::bits::Field{0, 3}]];
  OpKind kind[[= uarch::bits::Field{3, 2}]];
  bool primary_done[[= uarch::bits::Flag{5}]];
  bool notif_expected[[= uarch::bits::Flag{6}]];
  bool notif_done[[= uarch::bits::Flag{7}]];
  bool cancel_requested[[= uarch::bits::Flag{8}]];
  std::uint8_t pending_cqes[[= uarch::bits::Field{9, 3}]];
  std::uint8_t dispatch_pins[[= uarch::bits::Field{12, 5}]];
  std::uint8_t member_count[[= uarch::bits::Field{17, 3}]];
  std::uint8_t member_terminal_seen[[= uarch::bits::Field{20, 4}]];
  std::uint8_t member_more_seen[[= uarch::bits::Field{24, 4}]];
  std::uint8_t member_notif_seen[[= uarch::bits::Field{28, 4}]];
  std::uint32_t generation[[= uarch::bits::Field{32, 32}]];
};
using OpHeader = uarch::BitWord<OpHeaderSpec>;
static_assert(sizeof(OpHeader) == 8 && uarch::NoPadding<OpHeader> && uarch::RegisterPassable<OpHeader>);

[[nodiscard]] constexpr auto State(OpHeader h) noexcept { return h.get<^^OpHeaderSpec::state>(); }
[[nodiscard]] constexpr auto Kind(OpHeader h) noexcept { return h.get<^^OpHeaderSpec::kind>(); }
[[nodiscard]] constexpr auto Generation(OpHeader h) noexcept { return h.get<^^OpHeaderSpec::generation>(); }
[[nodiscard]] constexpr bool GenerationMatches(UserDataToken token, OpHeader h) noexcept {
  return ((token.raw() ^ h.raw()) >> 32) == 0;
}

[[nodiscard]] consteval auto MakeTransitionTable() {
  std::array<std::array<std::array<OpState, 3>, 7>, 4> table{};
  for (unsigned kind = 0; kind < 4; ++kind) {
    for (unsigned state = 0; state < 7; ++state) {
      for (unsigned event = 0; event < 3; ++event) table[kind][state][event] = static_cast<OpState>(state);
    }
    table[kind][3][0] = OpState::kRetiring;
    table[kind][3][1] = OpState::kArmed;
  }
  return table;
}
inline constexpr auto kNext = MakeTransitionTable();

[[nodiscard]] constexpr OpHeader MakeHeader(OpKind kind, std::uint32_t generation, std::uint8_t members = 1) noexcept {
  return OpHeader{}
      .with<^^OpHeaderSpec::state>(OpState::kArmed)
      .with<^^OpHeaderSpec::kind>(kind)
      .with<^^OpHeaderSpec::generation>(generation)
      .with<^^OpHeaderSpec::pending_cqes>(members)
      .with<^^OpHeaderSpec::member_count>(members);
}
inline constexpr std::uint8_t kMaxDispatchPins = 31;
[[nodiscard]] constexpr auto DispatchPins(OpHeader h) noexcept { return h.get<^^OpHeaderSpec::dispatch_pins>(); }
[[nodiscard]] constexpr OpHeader Pin(OpHeader h) noexcept {
  const auto pins = DispatchPins(h);
  return h.with<^^OpHeaderSpec::dispatch_pins>(pins + static_cast<unsigned>(pins != kMaxDispatchPins));
}
[[nodiscard]] constexpr OpHeader ReleaseRetired(OpHeader h) noexcept {
  const bool release = uarch::eager_all(State(h) == OpState::kRetiring, DispatchPins(h) == 0);
  const auto terminal = Generation(h) == std::numeric_limits<std::uint32_t>::max() ? OpState::kExhausted : OpState::kFree;
  return h.with<^^OpHeaderSpec::state>(release ? terminal : State(h));
}
[[nodiscard]] constexpr OpHeader Unpin(OpHeader h) noexcept {
  const auto pins = DispatchPins(h);
  if (pins == 0) return h;
  return ReleaseRetired(h.with<^^OpHeaderSpec::dispatch_pins>(pins - 1U));
}
struct ProtocolViewSpec {
  using Word = std::uint64_t;
  std::uint8_t flags[[= uarch::bits::Field{5, 3}]];
  std::uint8_t pending[[= uarch::bits::Field{9, 3}]];
};
struct ProtocolIndexSpec {
  using Word = std::uint8_t;
  std::uint8_t flags[[= uarch::bits::Field{0, 3}]];
  std::uint8_t pending[[= uarch::bits::Field{3, 3}]];
};
inline constexpr auto kProtocolMask = OpHeader{}
                                          .with<^^OpHeaderSpec::state>(static_cast<OpState>(7))
                                          .with<^^OpHeaderSpec::primary_done>(true)
                                          .with<^^OpHeaderSpec::notif_expected>(true)
                                          .with<^^OpHeaderSpec::notif_done>(true)
                                          .with<^^OpHeaderSpec::pending_cqes>(7U)
                                          .raw();

inline constexpr auto kPrimaryMask = OpHeader{}.with<^^OpHeaderSpec::primary_done>(true).raw();
inline constexpr auto kExpectedMask = OpHeader{}.with<^^OpHeaderSpec::notif_expected>(true).raw();
inline constexpr auto kNotificationMask = OpHeader{}.with<^^OpHeaderSpec::notif_done>(true).raw();
inline constexpr auto kPendingUnit = OpHeader{}.with<^^OpHeaderSpec::pending_cqes>(1U).raw();
inline constexpr auto kPendingMask = OpHeader{}.with<^^OpHeaderSpec::pending_cqes>(7U).raw();
static_assert(kExpectedMask == 2 * kPrimaryMask && kNotificationMask == 4 * kPrimaryMask);
[[nodiscard]] consteval auto MakeProtocolTable() {
  std::array<std::array<std::array<std::uint16_t, 3>, 64>, 4> table{};
  for (unsigned kind = 0; kind < 4; ++kind) {
    for (unsigned index = 0; index < 64; ++index) {
      for (unsigned event = 0; event < 3; ++event) {
        auto bits = (index & 7U) * kPrimaryMask + (index >> 3) * kPendingUnit;
        auto next = kNext[kind][static_cast<unsigned>(OpState::kArmed)][event];
        if (kind == static_cast<unsigned>(OpKind::kZeroCopy)) {
          bits |= event == 2 ? kNotificationMask : kPrimaryMask;
          bits |= event != 0 ? kExpectedMask : 0;
          const bool done = (bits & kPrimaryMask) != 0 && (((bits & kExpectedMask) != 0) == ((bits & kNotificationMask) != 0));
          next = done ? OpState::kRetiring : OpState::kAwaitNotif;
        }
        table[kind][index][event] = static_cast<std::uint16_t>(bits | static_cast<unsigned>(next));
      }
    }
  }
  return table;
}
// Precompute flag/counter bookkeeping too, so each CQE uses one small table lookup.
inline constexpr auto kProtocolNext = MakeProtocolTable();
static_assert(sizeof(kProtocolNext) == 1536);
[[nodiscard]] constexpr OpHeader AdvanceChain(OpHeader h, CqeClass event, std::uint8_t member) noexcept {
  if (member >= 4 || member >= h.get<^^OpHeaderSpec::member_count>()) return h;
  const auto bit = static_cast<std::uint8_t>(1U << member);
  const auto previous = h.get<^^OpHeaderSpec::member_terminal_seen>();
  const auto more =
      static_cast<std::uint8_t>(h.get<^^OpHeaderSpec::member_more_seen>() | (event == CqeClass::kMore ? bit : 0U));
  const auto notif =
      static_cast<std::uint8_t>(h.get<^^OpHeaderSpec::member_notif_seen>() | (event == CqeClass::kNotification ? bit : 0U));
  const bool terminal = uarch::eager_any(event == CqeClass::kTerminal, (more & notif & bit) != 0);
  const auto seen = static_cast<std::uint8_t>(previous | (terminal ? bit : 0U));
  const auto pending =
      static_cast<std::uint8_t>(h.get<^^OpHeaderSpec::pending_cqes>() - static_cast<std::uint8_t>(seen != previous));
  const bool expected = (more | notif) != 0;
  const bool owed = (more & ~notif) != 0;
  const bool notif_done = uarch::eager_all(expected, !owed);
  const auto next = pending != 0 ? OpState::kArmed : (owed ? OpState::kAwaitNotif : OpState::kRetiring);
  constexpr auto mask = OpHeader{}
                            .with<^^OpHeaderSpec::member_more_seen>(15U)
                            .with<^^OpHeaderSpec::member_notif_seen>(15U)
                            .with<^^OpHeaderSpec::member_terminal_seen>(15U)
                            .with<^^OpHeaderSpec::pending_cqes>(7U)
                            .with<^^OpHeaderSpec::notif_expected>(true)
                            .with<^^OpHeaderSpec::notif_done>(true)
                            .with<^^OpHeaderSpec::state>(static_cast<OpState>(7))
                            .raw();
  const auto bits = OpHeader{}
                        .with<^^OpHeaderSpec::member_more_seen>(more)
                        .with<^^OpHeaderSpec::member_notif_seen>(notif)
                        .with<^^OpHeaderSpec::member_terminal_seen>(seen)
                        .with<^^OpHeaderSpec::pending_cqes>(pending)
                        .with<^^OpHeaderSpec::notif_expected>(expected)
                        .with<^^OpHeaderSpec::notif_done>(notif_done)
                        .with<^^OpHeaderSpec::state>(next)
                        .raw();
  h = OpHeader::from_raw((h.raw() & ~mask) | bits);
  return ReleaseRetired(h);
}
[[nodiscard, gnu::always_inline]] constexpr OpHeader Advance(OpHeader h, CqeClass event, std::uint8_t member = 0) noexcept {
  if ((State(h) != OpState::kArmed && State(h) != OpState::kAwaitNotif) || static_cast<unsigned>(event) >= 3) return h;
  if (Kind(h) == OpKind::kChain) return AdvanceChain(h, event, member);
  const auto view = uarch::BitWord<ProtocolViewSpec>::from_raw(h.raw());
  const auto index = uarch::BitWord<ProtocolIndexSpec>{}
                         .with<^^ProtocolIndexSpec::flags>(view.get<^^ProtocolViewSpec::flags>())
                         .with<^^ProtocolIndexSpec::pending>(view.get<^^ProtocolViewSpec::pending>())
                         .raw();
  const auto low = kProtocolNext[static_cast<unsigned>(Kind(h))][index][static_cast<unsigned>(event)];
  return ReleaseRetired(OpHeader::from_raw((h.raw() & ~kProtocolMask) | low));
}
[[nodiscard]] constexpr CqeClass ClassifyCqe(std::uint32_t flags) noexcept {
  // Notification takes precedence when the kernel also sets MORE on a notification CQE.
  return (flags & kCqeFlagNotif) != 0 ? CqeClass::kNotification
                                      : ((flags & kCqeFlagMore) != 0 ? CqeClass::kMore : CqeClass::kTerminal);
}
}  // namespace liburing
