// expected-error: subsystem operations cannot use IOSQE_IO_DRAIN
#include <liburing/ops/uring_cmd.hpp>
constexpr std::array<std::byte, 80> kPayload{};
constexpr auto kBad = liburing::ops::PrepareUringCmd128(3, 4, kPayload).WithSqeFlags<liburing::abi::kSqeIoDrain>();
