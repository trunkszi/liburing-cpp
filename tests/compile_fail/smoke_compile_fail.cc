// expected-error: smoke compile fail triggered
#include <string_view>

#if defined(LIBURING_CARET_FAILURE_CONTROL)
static_assert(undeclared_value, "smoke compile fail triggered");
#else
static_assert(false, std::string_view("smoke compile fail triggered"));
#endif
