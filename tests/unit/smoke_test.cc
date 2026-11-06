#include <cstdint>
#include <limits>
#include <meta>
#include <uarch/uarch.hpp>

struct SmokeRecord {
  std::uint64_t first;
  std::uint64_t second;
};
static_assert(uarch::NoPadding<SmokeRecord> && uarch::RegisterPassable<SmokeRecord>);
static_assert(uarch::GprPair<SmokeRecord>);
constexpr auto kMembers =
    std::define_static_array(std::meta::nonstatic_data_members_of(^^SmokeRecord, std::meta::access_context::current()));
static_assert(kMembers.size() == 2);

struct SmokeBits {
  using Word = std::uint64_t;
  std::uint32_t slot[[= uarch::bits::Field{0, 32}]];
  std::uint32_t generation[[= uarch::bits::Field{32, 32}]];
};
using SmokeWord = uarch::BitWord<SmokeBits>;
static_assert(uarch::NoPadding<SmokeWord> && uarch::RegisterPassable<SmokeWord>);

constexpr SmokeWord Pack(std::uint32_t slot, std::uint32_t generation) noexcept {
  SmokeWord word{};
  word = word.with<^^SmokeBits::slot>(slot);
  return word.with<^^SmokeBits::generation>(generation);
}
static_assert(Pack(7, 3).raw() == 0x0000000300000007ULL);
static_assert(Pack(7, 3).wrapping_add_top_field<^^SmokeBits::generation>(1U).raw() == 0x0000000400000007ULL);

int main() {
#if defined(LIBURING_UBSAN_FAILURE_CONTROL)
  volatile int input = std::numeric_limits<int>::max();
  return input + 1;
#else
  volatile std::uint32_t input = 7;
  return Pack(input, 3).raw() == 0x0000000300000007ULL ? 0 : 1;
#endif
}
