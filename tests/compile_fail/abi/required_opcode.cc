// expected-error: required opcode outside UAPI
#include <liburing/caps/required_caps.hpp>
constexpr auto kBad = liburing::caps::RequiredCaps::For<65>();
