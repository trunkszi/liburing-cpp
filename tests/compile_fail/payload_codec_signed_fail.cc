// expected-error: (constraints not satisfied|no matching function)
#include <liburing/core/payload_codec.hpp>
struct InvalidPayload {
  std::int32_t signed_value;
};
struct TooWide {
  std::uint32_t first;
  std::uint32_t second;
};
static_assert(liburing::PayloadEncodable<TooWide>, "payload exceeds 56 bits");
int main() { return static_cast<int>(liburing::EncodePayload(InvalidPayload{})); }
