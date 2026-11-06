/** @file @brief Destructive control intents wait for an engine quiescent point. */
#pragma once
#include <cstdint>
#include <liburing/core/deferred_queue.hpp>
#include <liburing/core/user_data_token.hpp>
#include <optional>

namespace liburing {
enum class ControlKind : std::uint32_t { kDetach, kResize, kSyncCancel };
struct ControlCommand {
  OperationKey key;
  std::uint64_t argument;
  ControlKind kind;
  std::uint32_t reserved{};
};
static_assert(sizeof(ControlCommand) == 32 && uarch::NoPadding<ControlCommand>);
static_assert(std::is_trivially_copyable_v<ControlCommand> && std::is_standard_layout_v<ControlCommand>);
// The engine applies these records only at its quiescent point.
template <std::size_t MaxSlots = 64>
class ControlQueue {
  std::array<ControlCommand, MaxSlots> commands_{};
  DeferredQueue<MaxSlots> queue_{};

 public:
  [[nodiscard]] constexpr bool TryEnqueue(std::uint32_t slot, ControlCommand command) noexcept {
    if (!queue_.TryEnqueue(slot)) return false;
    commands_[slot] = command;
    return true;
  }
  [[nodiscard]] constexpr bool Empty() const noexcept { return queue_.Empty(); }
  [[nodiscard]] constexpr bool Remove(std::uint32_t slot) noexcept { return queue_.Remove(slot); }
  [[nodiscard]] constexpr std::optional<ControlCommand> PopFront() noexcept {
    const auto slot = queue_.PopFront();
    if (slot == kNullSlot) return std::nullopt;
    return commands_[slot];
  }
};
}  // namespace liburing
