// expected-error: tracked operations require CQEs
#include <liburing/ops/core.hpp>
constexpr auto kBad =
    liburing::ops::PrepareNop().WithToken(liburing::MakeToken(1, 0, 7, 1)).WithSqeFlags<liburing::abi::kSqeCqeSkipSuccess>();
