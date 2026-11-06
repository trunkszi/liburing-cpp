#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <liburing/core/operation_slab.hpp>

[[gnu::noinline]] bool RoundTrip(liburing::OperationSlab<std::uint64_t, 4>& slab, std::uint64_t& checksum) noexcept {
  const auto token = slab.TryAllocate(liburing::OpKind::kOneShot);
  if (!token || !slab.Arm(*token)) return false;
  *slab.Get(*token) = liburing::Generation(*token);
  if (!slab.Complete(*token, liburing::CqeClass::kTerminal)) return false;
  checksum += *slab.Get(*token);
  return slab.FinishDispatch(*token);
}

int main() {
  using Clock = std::chrono::steady_clock;
  liburing::OperationSlab<std::uint64_t, 4> slab(1);
  std::array<double, 8192> samples{};
  std::uint64_t checksum = 0;
  for (auto& sample : samples) {
    const auto start = Clock::now();
    if (!RoundTrip(slab, checksum)) return 1;
    sample = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  }
  std::sort(samples.begin(), samples.end());
  std::printf("slab roundtrip ns p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f checksum=%llu\n", samples[4096], samples[7372],
              samples[8110], samples[8183], static_cast<unsigned long long>(checksum));
  return checksum == 33558528ULL ? 0 : 4;
}
