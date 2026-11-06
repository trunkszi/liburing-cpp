// expected-error: subsystem operations cannot use IOSQE_IO_DRAIN
#include <liburing/ops/core.hpp>
constexpr auto kBad = liburing::ops::PrepareNop().WithSqeFlags<liburing::abi::kSqeIoDrain>();
