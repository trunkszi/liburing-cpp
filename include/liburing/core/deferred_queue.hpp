/** @file @brief Stable slot identities retain submission intents across SQ backpressure. */
#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <uarch/uarch.hpp>

namespace liburing {
inline constexpr std::uint32_t kNullSlot = 0;
template <std::size_t MaxSlots = 64>
class DeferredQueue {
  static_assert(MaxSlots >= 2 && MaxSlots <= std::numeric_limits<std::uint32_t>::max());
  static constexpr std::uint32_t kNotQueued = 0;
  static constexpr auto kTailSentinel = std::numeric_limits<std::uint32_t>::max();
  struct Link {
    std::uint32_t previous{};
    std::uint32_t next{};
  };
  static_assert(uarch::NoPadding<Link> && sizeof(Link) == 8);
  uarch::CacheAligned<std::array<Link, MaxSlots>> links_{};
  std::uint32_t head_{};
  std::uint32_t tail_{};
  std::uint32_t size_{};

 public:
  constexpr DeferredQueue() noexcept = default;
  DeferredQueue(const DeferredQueue&) = delete;
  DeferredQueue& operator=(const DeferredQueue&) = delete;
  [[nodiscard]] constexpr bool Empty() const noexcept { return head_ == kNullSlot; }
  [[nodiscard]] constexpr auto Size() const noexcept { return size_; }
  [[nodiscard]] constexpr auto Front() const noexcept { return head_; }
  [[nodiscard]] constexpr bool Contains(std::uint32_t slot) const noexcept {
    return slot != kNullSlot && slot < MaxSlots && links_.value[slot].next != kNotQueued;
  }
  [[nodiscard]] constexpr bool TryEnqueue(std::uint32_t slot) noexcept {
    if (slot == kNullSlot || slot >= MaxSlots || Contains(slot)) return false;
    links_.value[slot] = {tail_, kTailSentinel};
    if (tail_ != kNullSlot)
      links_.value[tail_].next = slot;
    else
      head_ = slot;
    tail_ = slot;
    ++size_;
    return true;
  }
  [[nodiscard]] constexpr bool Remove(std::uint32_t slot) noexcept {
    if (!Contains(slot)) return false;
    const auto link = links_.value[slot];
    const std::uint32_t next_slot = link.next == kTailSentinel ? kNullSlot : link.next;
    if (link.previous != kNullSlot)
      links_.value[link.previous].next = link.next;
    else
      head_ = next_slot;
    if (next_slot != kNullSlot)
      links_.value[next_slot].previous = link.previous;
    else
      tail_ = link.previous;
    links_.value[slot] = {};
    --size_;
    return true;
  }
  [[nodiscard]] constexpr std::uint32_t PopFront() noexcept {
    const auto slot = head_;
    if (slot != kNullSlot) static_cast<void>(Remove(slot));
    return slot;
  }
};
}  // namespace liburing
