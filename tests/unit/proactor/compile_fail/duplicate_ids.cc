// expected-error: subsystem IDs must be unique and nonreserved
#include <liburing/core/dispatcher.hpp>
struct Sink {
  static constexpr std::uint8_t kSubsystemId = 1;
  static consteval liburing::caps::RequiredCaps RequiredCaps() noexcept { return {}; }
  void OnCompletions(std::span<const liburing::Delivery>) noexcept {}
};
int main() {
  Sink a;
  Sink b;
  liburing::StaticDispatcher dispatcher{a, b};
}
