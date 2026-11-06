/** @file @brief Dispatch preserves arrival order and merges only adjacent runs. */
#pragma once
#include <array>
#include <liburing/core/subsystem.hpp>
#include <meta>
#include <tuple>
#include <utility>
namespace liburing {
template <plugins::Subsystem... Subsystems>
[[nodiscard]] consteval bool UniqueSubsystemIds() noexcept {
  std::array<bool, 256> seen{};
  for (auto id : std::array<std::uint8_t, sizeof...(Subsystems)>{Subsystems::kSubsystemId...}) {
    if (seen[id] || id == kInternalSubsystemId || id == kMailboxSubsystemId) return false;
    seen[id] = true;
  }
  return true;
}
template <plugins::Subsystem S>
class SingleDispatcher {
  static_assert(S::kSubsystemId != kInternalSubsystemId && S::kSubsystemId != kMailboxSubsystemId, "reserved subsystem ID");
  S& subsystem_;
  std::size_t unrouted_count_{};

 public:
  explicit SingleDispatcher(S& subsystem) noexcept : subsystem_(subsystem) {}
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept { return S::RequiredCaps(); }
  [[nodiscard]] constexpr bool CanRoute(std::uint8_t id) const noexcept { return id == S::kSubsystemId; }
  [[nodiscard]] constexpr std::size_t UnroutedCount() const noexcept { return unrouted_count_; }
  [[nodiscard, gnu::always_inline]] Result<void> DispatchBatch(std::span<const Delivery> deliveries) noexcept {
    bool had_unrouted = false;
    while (!deliveries.empty()) {
      const auto id = Subsystem(deliveries.front().completion.token);
      std::size_t count = 1;
      while (count < deliveries.size() && Subsystem(deliveries[count].completion.token) == id) ++count;
      if (CanRoute(id))
        plugins::Deliver(subsystem_, deliveries.first(count));
      else {
        unrouted_count_ += count;
        had_unrouted = true;
      }
      deliveries = deliveries.subspan(count);
    }
    if (had_unrouted) return std::unexpected(UringErrorCode::kNoEntry);
    return {};
  }
};
template <plugins::Subsystem... Subsystems>
class StaticDispatcher {
  static_assert(sizeof...(Subsystems) > 0 && UniqueSubsystemIds<Subsystems...>(),
                "subsystem IDs must be unique and nonreserved");
  std::tuple<Subsystems*...> subsystems_;
  std::size_t unrouted_count_{};
  template <std::size_t... Indices>
  [[nodiscard]] static consteval auto IndicesFor(std::index_sequence<Indices...>) noexcept {
    return std::array<std::size_t, sizeof...(Indices)>{Indices...};
  }

 public:
  explicit StaticDispatcher(Subsystems&... subsystems) noexcept : subsystems_(&subsystems...) {}
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept {
    caps::RequiredCaps required{};
    template for (constexpr auto index : std::define_static_array(IndicesFor(std::index_sequence_for<Subsystems...>{}))) {
      using S = std::tuple_element_t<index, std::tuple<Subsystems...>>;
      required = MergeCaps(required, S::RequiredCaps());
    }
    return required;
  }
  [[nodiscard]] constexpr bool CanRoute(std::uint8_t id) const noexcept { return ((id == Subsystems::kSubsystemId) || ...); }
  [[nodiscard]] constexpr std::size_t UnroutedCount() const noexcept { return unrouted_count_; }
  [[nodiscard]] Result<void> DispatchBatch(std::span<const Delivery> deliveries) noexcept {
    bool had_unrouted = false;
    while (!deliveries.empty()) {
      const auto id = Subsystem(deliveries.front().completion.token);
      std::size_t count = 1;
      while (count < deliveries.size() && Subsystem(deliveries[count].completion.token) == id) ++count;
      bool found = false;
      template for (constexpr auto index : std::define_static_array(IndicesFor(std::index_sequence_for<Subsystems...>{}))) {
        using S = std::tuple_element_t<index, std::tuple<Subsystems...>>;
        if (id == S::kSubsystemId) {
          plugins::Deliver(*std::get<index>(subsystems_), deliveries.first(count));
          found = true;
        }
      }
      if (!found) {
        unrouted_count_ += count;
        had_unrouted = true;
      }
      deliveries = deliveries.subspan(count);
    }
    if (had_unrouted) return std::unexpected(UringErrorCode::kNoEntry);
    return {};
  }
};
class DynamicDispatcher {
  std::array<CompletionDelegate, 256> delegates_{};
  std::size_t unrouted_count_{};
  std::array<caps::RequiredCaps, 256> requirements_{};

 public:
  [[nodiscard]] Result<void> Register(const SubsystemDescriptor& descriptor) noexcept {
    const auto id = descriptor.subsystem_id;
    if (id == kInternalSubsystemId || id == kMailboxSubsystemId || !descriptor.completion.invoke)
      return std::unexpected(UringErrorCode::kInvalid);
    if (delegates_[id].invoke) return std::unexpected(UringErrorCode::kBusy);
    delegates_[id] = descriptor.completion;
    requirements_[id] = descriptor.required_caps;
    return {};
  }
  [[nodiscard]] Result<void> Detach(std::uint8_t id) noexcept {
    if (!delegates_[id].invoke) return std::unexpected(UringErrorCode::kNoEntry);
    delegates_[id] = {};
    requirements_[id] = {};
    return {};
  }
  [[nodiscard]] caps::RequiredCaps RequiredCaps() const noexcept {
    caps::RequiredCaps required{};
    for (std::size_t i = 0; i < delegates_.size(); ++i)
      if (delegates_[i].invoke) required = MergeCaps(required, requirements_[i]);
    return required;
  }
  [[nodiscard]] constexpr bool CanRoute(std::uint8_t id) const noexcept { return delegates_[id].invoke != nullptr; }
  [[nodiscard]] constexpr std::size_t UnroutedCount() const noexcept { return unrouted_count_; }
  [[nodiscard]] Result<void> DispatchBatch(std::span<const Delivery> deliveries) noexcept {
    bool had_unrouted = false;
    while (!deliveries.empty()) {
      const auto id = Subsystem(deliveries.front().completion.token);
      const auto delegate = delegates_[id];
      std::size_t count = 1;
      while (count < deliveries.size() && Subsystem(deliveries[count].completion.token) == id) ++count;
      if (delegate.invoke)
        delegate.invoke(delegate.context, deliveries.first(count));
      else {
        unrouted_count_ += count;
        had_unrouted = true;
      }
      deliveries = deliveries.subspan(count);
    }
    if (had_unrouted) return std::unexpected(UringErrorCode::kNoEntry);
    return {};
  }
};
}  // namespace liburing
