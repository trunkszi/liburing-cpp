// expected-error: FUTEX2_MPOL requires FUTEX2_NUMA
#include <liburing/ops/futex.hpp>
std::uint32_t word;
auto bad = liburing::ops::PrepareFutexWait<liburing::abi::kFutex2SizeU32 | liburing::abi::kFutex2Mpol>(&word, 1U);
