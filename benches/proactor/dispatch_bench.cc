#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <liburing/core/dispatcher.hpp>
struct Sink {
  static constexpr std::uint8_t kSubsystemId = 1;
  std::uint64_t sum{};
  [[nodiscard]] static consteval liburing::caps::RequiredCaps RequiredCaps() noexcept { return {}; }
  void OnCompletions(std::span<const liburing::Delivery> deliveries) noexcept {
    for (const auto& delivery : deliveries) sum += static_cast<std::uint32_t>(delivery.completion.result);
  }
};
int main() {
  Sink sink;
  liburing::SingleDispatcher dispatcher{sink};
  std::array<liburing::Delivery, 16> batch{};
  for (auto& delivery : batch) {
    delivery.completion.result = 1;
    delivery.completion.token = liburing::MakeToken(1, 0, 1, 1);
  }
  std::array<double, 1024> samples{};
  for (auto& sample : samples) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 128; ++i) {
      asm volatile("" : : "r"(batch.data()) : "memory");
      if (!dispatcher.DispatchBatch(batch)) return 1;
      asm volatile("" : "+r"(sink.sum) : : "memory");
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    sample = std::chrono::duration<double, std::nano>(elapsed).count() / 2048;
  }
  std::sort(samples.begin(), samples.end());
  std::printf("ns/delivery p50=%.3f p90=%.3f p99=%.3f p99.9=%.3f checksum=%llu\n", samples[512], samples[921], samples[1013],
              samples[1022], static_cast<unsigned long long>(sink.sum));
  return sink.sum == 2097152 ? 0 : 2;
}
