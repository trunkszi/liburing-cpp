// expected-error: tracked operations require CQEs
#include <liburing/ops/uring_cmd.hpp>
constexpr std::array<std::byte, 80> kPayload{};
constexpr auto kBad = liburing::ops::PrepareUringCmd128(3, 4, kPayload).WithSqeFlags<liburing::abi::kSqeCqeSkipSuccess>();
