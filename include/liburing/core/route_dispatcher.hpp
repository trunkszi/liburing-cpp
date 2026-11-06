#pragma once
#include <array>
#include <functional>
#include <liburing/core/completion.hpp>
#include <liburing/error/uring_error.hpp>
#include <meta>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
namespace liburing {
template <class E>
[[nodiscard]] consteval bool ValidSubsystemsEnum() {
  if constexpr (std::is_enum_v<E>) {
    if constexpr (std::is_same_v<std::underlying_type_t<E>, std::uint8_t>) {
      std::array<uarch::Bitmap64, 4> seen{};
      static constexpr auto values = std::define_static_array(std::meta::enumerators_of(^^E));
      if (values.empty()) return false;
      template for (constexpr auto value : values) {
        constexpr auto id = static_cast<std::uint8_t>([:value:]);
        if (id == 0 || id == 255 || seen[id >> 6].test(id & 63)) return false;
        seen[id >> 6] = seen[id >> 6].set(id & 63);
      }
      return true;
    }
  }
  return false;
}
template <class E>
concept SubsystemsEnum = ValidSubsystemsEnum<E>();
template <SubsystemsEnum E>
[[nodiscard]] constexpr std::optional<E> RouteFromSubsystemId(std::uint8_t id) noexcept {
  template for (constexpr auto value : std::define_static_array(std::meta::enumerators_of(^^E))) {
    if (id == static_cast<std::uint8_t>([:value:])) return [:value:];
  }
  return std::nullopt;
}
namespace detail {
template <class Fn, class... Args>
[[nodiscard]] Result<void> InvokeRoute(Fn& fn, Args&&... args) noexcept {
  if constexpr (std::is_void_v<std::invoke_result_t<Fn&, Args...>>) {
    std::invoke(fn, std::forward<Args>(args)...);
    return {};
  } else
    return std::invoke(fn, std::forward<Args>(args)...);
}
}  // namespace detail
template <SubsystemsEnum E, class State, class Fn>
class RouteDispatcher {
  State* state_;
  Fn fn_;
  std::size_t unrouted_count_{};

 public:
  RouteDispatcher(State& state, Fn fn) noexcept : state_(&state), fn_(std::move(fn)) {}
  [[nodiscard]] constexpr bool IsRoutable(std::uint8_t id) const noexcept { return RouteFromSubsystemId<E>(id).has_value(); }
  [[nodiscard]] constexpr bool CanRoute(std::uint8_t id) const noexcept { return IsRoutable(id); }
  [[nodiscard]] constexpr std::size_t UnroutedCount() const noexcept { return unrouted_count_; }
  [[nodiscard]] Result<std::uint32_t> DispatchBatch(std::span<const Delivery> deliveries) noexcept {
    std::optional<UringErrorCode> error;
    std::uint32_t count = 0;
    for (const auto& delivery : deliveries) {
      auto route = RouteFromSubsystemId<E>(Subsystem(delivery.completion.token));
      if (!route) {
        ++unrouted_count_;
        if (!error) error = UringErrorCode::kNoEntry;
        continue;
      }
      auto result = detail::InvokeRoute(fn_, *route, *state_, delivery);
      if (result)
        ++count;
      else if (!error)
        error = result.error();
    }
    if (error) return std::unexpected(*error);
    return count;
  }
};
template <SubsystemsEnum E, class State, class Fn>
[[nodiscard]] auto MakeRouteDispatcher(State& state, Fn&& fn) noexcept {
  return RouteDispatcher<E, State, std::decay_t<Fn>>{state, std::forward<Fn>(fn)};
}
template <class Fn>
class FnDispatcher {
  Fn fn_;
  std::size_t unrouted_count_{};

 public:
  explicit FnDispatcher(Fn fn) noexcept : fn_(std::move(fn)) {}
  [[nodiscard]] constexpr bool IsRoutable(std::uint8_t id) const noexcept { return id != 0 && id != 255; }
  [[nodiscard]] constexpr bool CanRoute(std::uint8_t id) const noexcept { return IsRoutable(id); }
  [[nodiscard]] constexpr std::size_t UnroutedCount() const noexcept { return unrouted_count_; }
  [[nodiscard]] Result<std::uint32_t> DispatchBatch(std::span<const Delivery> deliveries) noexcept {
    std::optional<UringErrorCode> error;
    std::uint32_t count = 0;
    for (const auto& delivery : deliveries) {
      if (!IsRoutable(Subsystem(delivery.completion.token))) {
        ++unrouted_count_;
        if (!error) error = UringErrorCode::kNoEntry;
        continue;
      }
      auto result = detail::InvokeRoute(fn_, delivery);
      if (result)
        ++count;
      else if (!error)
        error = result.error();
    }
    if (error) return std::unexpected(*error);
    return count;
  }
};
template <class Fn>
[[nodiscard]] auto MakeFnDispatcher(Fn&& fn) noexcept {
  return FnDispatcher<std::decay_t<Fn>>{std::forward<Fn>(fn)};
}
}  // namespace liburing
