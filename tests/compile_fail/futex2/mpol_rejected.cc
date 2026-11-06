// expected-error: MPOL requires NUMA
#include <liburing/futex2/futex_flags.hpp>
constexpr auto kBad = liburing::futex2::Futex2Flags<liburing::futex2::FutexScope::kShared,
                                                    static_cast<liburing::futex2::FutexLocality>(liburing::abi::kFutex2Mpol)>();
