// expected-error: untracked operations require kNullToken
#include <liburing/ops/core.hpp>
constexpr auto kBad = liburing::ops::PreparePadNop().WithToken(liburing::MakeToken(1, 0, 7, 1));
