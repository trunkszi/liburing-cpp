/** @file @brief Generational storage prevents stale completions from reaching reused payloads. */
#pragma once
#include <array>
#include <cstdint>
#include <liburing/core/op_header.hpp>
#include <optional>
#include <span>

namespace liburing {
template <class Payload, unsigned Log>
class OperationSlab {
  static_assert(Log >= 1 && Log <= 22, "slab Log must fit the token slot field");

 public:
  using Index = uarch::PoolIndex<Payload>;
  static constexpr std::size_t kCapacity = 1uz << Log;

 private:
  std::array<Payload, kCapacity> payloads_{};
  uarch::CacheAligned<std::array<OpHeader, kCapacity>> headers_{};
  std::array<std::uint32_t, kCapacity> next_{};
  std::uint32_t free_head_{};
  std::uint8_t subsystem_;

  constexpr void Recycle(std::uint32_t slot) noexcept {
    if (State(headers_.value[slot]) == OpState::kFree) {
      next_[slot] = free_head_;
      free_head_ = slot;
    }
  }

 public:
  // A seed permits restoring a drained slab without reusing an earlier generation.
  explicit constexpr OperationSlab(std::uint8_t subsystem, std::uint32_t generation_seed = 0) noexcept : subsystem_(subsystem) {
    const bool exhausted = generation_seed == std::numeric_limits<std::uint32_t>::max();
    for (std::size_t i = 1; i < kCapacity; ++i) {
      headers_.value[i] = OpHeader{}
                              .template with<^^OpHeaderSpec::generation>(generation_seed)
                              .template with<^^OpHeaderSpec::state>(exhausted ? OpState::kExhausted : OpState::kFree);
      next_[i] = i + 1 < kCapacity ? static_cast<std::uint32_t>(i + 1) : 0;
    }
    free_head_ = exhausted ? 0 : 1;
  }
  OperationSlab(const OperationSlab&) = delete;
  OperationSlab& operator=(const OperationSlab&) = delete;
  OperationSlab(OperationSlab&&) = delete;
  OperationSlab& operator=(OperationSlab&&) = delete;

  [[nodiscard]] constexpr bool Contains(UserDataToken token) const noexcept {
    const auto slot = Slot(token);
    // Bounds and sentinel checks must precede the header load, even on a branchless data path.
    if (slot == 0 || slot >= kCapacity || Subsystem(token) != subsystem_) return false;
    const auto h = headers_.value[slot];
    const auto state = State(h);
    return uarch::eager_all(GenerationMatches(token, h), state != OpState::kFree, state != OpState::kExhausted,
                            Member(token) < h.template get<^^OpHeaderSpec::member_count>());
  }
  [[nodiscard]] constexpr Payload* Get(UserDataToken token) noexcept {
    if (!Contains(token)) return nullptr;
    auto pool = uarch::Pool<Payload, Log>::try_new(std::span<Payload>(payloads_));
    return pool->get(*Index::try_new(Slot(token)));
  }
  [[nodiscard]] constexpr const Payload* Get(UserDataToken token) const noexcept {
    if (!Contains(token)) return nullptr;
    auto pool = uarch::Pool<const Payload, Log>::try_new(std::span<const Payload>(payloads_));
    return pool->get(*uarch::PoolIndex<const Payload>::try_new(Slot(token)));
  }
  [[nodiscard]] constexpr const OpHeader* Header(UserDataToken token) const noexcept {
    return Contains(token) ? &headers_.value[Slot(token)] : nullptr;
  }
  [[nodiscard]] constexpr std::optional<UserDataToken> TryAllocate(OpKind kind, std::uint8_t members = 1) noexcept {
    if (free_head_ == 0 || static_cast<unsigned>(kind) > 3 || members == 0 || members > 4 ||
        (kind != OpKind::kChain && members != 1))
      return std::nullopt;
    const auto slot = free_head_;
    free_head_ = next_[slot];
    const auto generation = Generation(headers_.value[slot]) + 1;
    headers_.value[slot] = MakeHeader(kind, generation, members).template with<^^OpHeaderSpec::state>(OpState::kReserved);
    return MakeToken(slot, 0, subsystem_, generation);
  }
  [[nodiscard]] constexpr bool Defer(UserDataToken token) noexcept {
    if (!Contains(token) || State(headers_.value[Slot(token)]) != OpState::kReserved) return false;
    auto& h = headers_.value[Slot(token)];
    h = h.template with<^^OpHeaderSpec::state>(OpState::kDeferred);
    return true;
  }
  [[nodiscard]] constexpr bool Arm(UserDataToken token) noexcept {
    if (!Contains(token)) return false;
    auto& h = headers_.value[Slot(token)];
    if (State(h) != OpState::kReserved && State(h) != OpState::kDeferred) return false;
    h = h.template with<^^OpHeaderSpec::state>(OpState::kArmed);
    return true;
  }
  [[nodiscard]] constexpr bool Complete(UserDataToken token, CqeClass event) noexcept {
    if (!Contains(token) || static_cast<unsigned>(event) > 2) return false;
    const auto slot = Slot(token);
    auto& h = headers_.value[slot];
    if ((State(h) != OpState::kArmed && State(h) != OpState::kAwaitNotif) || DispatchPins(h) == kMaxDispatchPins) return false;
    if (event == CqeClass::kNotification && Kind(h) != OpKind::kZeroCopy && Kind(h) != OpKind::kChain) return false;
    if (Kind(h) == OpKind::kChain) {
      const auto bit = static_cast<std::uint8_t>(1U << Member(token));
      const auto terminal = h.template get<^^OpHeaderSpec::member_terminal_seen>();
      const auto more = h.template get<^^OpHeaderSpec::member_more_seen>();
      const auto notif = h.template get<^^OpHeaderSpec::member_notif_seen>();
      if (event == CqeClass::kNotification) {
        if ((notif & bit) != 0 || ((terminal & bit) != 0 && (more & bit) == 0)) return false;
      } else if ((terminal & bit) != 0)
        return false;
    }
    h = Advance(Pin(h), event, Member(token));
    return true;
  }
  [[nodiscard]] constexpr bool FinishDispatch(UserDataToken token) noexcept {
    if (!Contains(token)) return false;
    auto& h = headers_.value[Slot(token)];
    if (DispatchPins(h) == 0) return false;
    h = Unpin(h);
    Recycle(Slot(token));
    return true;
  }
  [[nodiscard]] constexpr bool RequestCancel(UserDataToken token) noexcept {
    if (!Contains(token)) return false;
    auto& h = headers_.value[Slot(token)];
    h = h.template with<^^OpHeaderSpec::cancel_requested>(true);
    return true;
  }
  [[nodiscard]] constexpr bool CancelUnsubmitted(UserDataToken token) noexcept {
    if (!Contains(token)) return false;
    auto& h = headers_.value[Slot(token)];
    if (State(h) != OpState::kReserved && State(h) != OpState::kDeferred) return false;
    h = ReleaseRetired(h.template with<^^OpHeaderSpec::state>(OpState::kRetiring));
    Recycle(Slot(token));
    return true;
  }
};
}  // namespace liburing
