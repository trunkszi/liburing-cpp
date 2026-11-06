// expected-error: field value overflow
#include <liburing/core/user_data_token.hpp>
constexpr auto kBad = liburing::UserDataToken{}.with_const<^^liburing::TokenSpec::slot, 0x400000U>();
