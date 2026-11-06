#include <sched.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <liburing/futex2/futex2.hpp>
#include <optional>
using namespace liburing::futex2;
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kSamples = 4096;
constexpr std::uint32_t kBatch = 128;
void Report(const char* operation, std::array<std::int64_t, kSamples>& samples) {
  std::sort(samples.begin(), samples.end());
  auto quantile = [&](std::size_t index) { return static_cast<double>(samples[index]) / kBatch; };
  std::printf("%s,%.3f,%.3f,%.3f,%.3f\n", operation, quantile(kSamples / 2), quantile(kSamples * 90 / 100),
              quantile(kSamples * 99 / 100), quantile(kSamples * 999 / 1000));
}
}  // namespace
int main() {
  cpu_set_t allowed{};
  if (sched_getaffinity(0, sizeof(allowed), &allowed)) return 1;
  std::optional<std::size_t> core;
  for (std::size_t cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &allowed)) {
      core = cpu;
      break;
    }
  if (!core) return 2;
  cpu_set_t selected{};
  CPU_ZERO(&selected);
  CPU_SET(*core, &selected);
  if (sched_setaffinity(0, sizeof(selected), &selected)) return 3;
  struct Restore {
    cpu_set_t mask;
    ~Restore() { static_cast<void>(sched_setaffinity(0, sizeof(mask), &mask)); }
  } restore{allowed};
  FutexWord32 word{};
  std::array<std::int64_t, kSamples> samples{};
  for (std::uint32_t i = 0; i < 4096; ++i) static_cast<void>(FetchAdd(word, 1));
  std::uint64_t checksum = 0;
  for (auto& sample : samples) {
    auto start = Clock::now();
    for (std::uint32_t i = 0; i < kBatch; ++i) checksum += FetchAdd(word, 1);
    sample = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
  }
  constexpr auto count = static_cast<std::uint64_t>(kSamples) * kBatch;
  if (checksum != count * 4096 + count * (count - 1) / 2) return 4;
  std::printf("operation,p50_batch_mean_ns,p90_batch_mean_ns,p99_batch_mean_ns,p999_batch_mean_ns\n");
  Report("fetch_add", samples);
  auto ref = MakeRef(word);
  if (!ref) return 5;
  for (auto& sample : samples) {
    auto start = Clock::now();
    for (std::uint32_t i = 0; i < kBatch; ++i) {
      auto result = Wake(*ref, 0);
      if (!result || *result != 0) return 6;
    }
    sample = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
  }
  Report("wake_no_waiters", samples);
  return 0;
}
