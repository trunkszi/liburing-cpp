// expected-error: padding rejected
#include <cstdint>
#include <uarch/uarch.hpp>
struct Bad {
  std::uint8_t tag;
  std::uint64_t value;
};
static_assert(uarch::NoPadding<Bad>, "padding rejected");
