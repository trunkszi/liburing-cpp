// expected-error: fd ownership is unique
#include <liburing/sys/unique_fd.hpp>
void Copy(const liburing::sys::UniqueFd& fd) { auto copy = fd; }
