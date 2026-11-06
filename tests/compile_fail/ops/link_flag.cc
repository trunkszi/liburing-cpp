// expected-error: linked chains must use ChainShape
#include <liburing/ops/uring_cmd.hpp>
#ifdef T4_HARDLINK
constexpr auto kFlag = liburing::abi::kSqeIoHardlink;
#else
constexpr auto kFlag = liburing::abi::kSqeIoLink;
#endif
#ifdef T4_WIDE
constexpr auto kBad = liburing::ops::UringCmd128{}.WithSqeFlags<kFlag>();
#else
constexpr auto kBad = liburing::ops::PrepareNop().WithSqeFlags<kFlag>();
#endif
