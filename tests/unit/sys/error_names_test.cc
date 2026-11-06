#include <cerrno>
#include <liburing/error/uring_error.hpp>
using namespace liburing;
constexpr bool ErrorNames() {
  return static_cast<unsigned>(UringErrorCode::kTimerExpired) == 62 &&
         FromRawSyscall(-104).error().code() == UringErrorCode::kConnectionReset &&
         ErrorName(UringErrorCode::kTimerExpired) == "timer expired" &&
         ErrorName(UringErrorCode::kConnectionReset) == "connection reset" &&
         ErrorName(static_cast<UringErrorCode>(0)) == "unknown liburing error";
}
static_assert(ErrorNames());
static_assert(static_cast<unsigned>(UringErrorCode::kShuttingDown) == ESHUTDOWN);
static_assert(ErrorName(UringErrorCode::kShuttingDown) == "shutting down");
static_assert(ToResult(-ESHUTDOWN).error() == UringErrorCode::kShuttingDown);
static_assert(static_cast<unsigned>(UringErrorCode::kFileExists) == EEXIST);
static_assert(static_cast<unsigned>(UringErrorCode::kBrokenPipe) == EPIPE);
static_assert(static_cast<unsigned>(UringErrorCode::kBadRequestDescriptor) == EBADR);
static_assert(static_cast<unsigned>(UringErrorCode::kTimerExpired) == ETIME);
static_assert(static_cast<unsigned>(UringErrorCode::kConnectionReset) == ECONNRESET);
static_assert(static_cast<unsigned>(UringErrorCode::kNoBufferSpace) == ENOBUFS);
static_assert(static_cast<unsigned>(UringErrorCode::kTransportConnected) == EISCONN);
static_assert(static_cast<unsigned>(UringErrorCode::kTransportNotConnected) == ENOTCONN);
static_assert(static_cast<unsigned>(UringErrorCode::kConnectionRefused) == ECONNREFUSED);
static_assert(static_cast<unsigned>(UringErrorCode::kAlreadyInProgress) == EALREADY);
static_assert(static_cast<unsigned>(UringErrorCode::kOperationInProgress) == EINPROGRESS);

int main() { return ErrorNames() ? 0 : 1; }
