// expected-error: Linux kernel futex2 currently only implements FUTEX2_SIZE_U32
#include <liburing/ops/futex.hpp>
std::uint64_t word;
auto bad = liburing::ops::PrepareFutexWait(&word, 1U);
