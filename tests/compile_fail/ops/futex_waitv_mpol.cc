// expected-error: FUTEX2_MPOL requires FUTEX2_NUMA
#include <liburing/ops/futex.hpp>
liburing::abi::FutexWaitv waiter;
auto bad = liburing::ops::PrepareFutexWaitv<liburing::abi::kFutex2SizeU32 | liburing::abi::kFutex2Mpol>(&waiter, 1U);
