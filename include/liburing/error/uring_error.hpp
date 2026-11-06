#pragma once
#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <uarch/uarch.hpp>

namespace liburing {
enum class UringErrorCode : std::uint8_t {
  kPermissionDenied = 1,
  kNoEntry = 2,
  kInterrupted = 4,
  kIo = 5,
  kBadFd = 9,
  kAgain = 11,
  kNoMemory = 12,
  kAccessDenied = 13,
  kFault = 14,
  kBusy = 16,
  kInvalid = 22,
  kTooManyFiles = 24,
  kNoSpace = 28,
  kNotImplemented = 38,
  kOverflow = 75,
  kNotSupported = 95,
  kTimedOut = 110,
  kCanceled = 125,
  kFileExists = 17,
  kBrokenPipe = 32,
  kBadRequestDescriptor = 53,
  kTimerExpired = 62,
  kConnectionReset = 104,
  kNoBufferSpace = 105,
  kTransportConnected = 106,
  kTransportNotConnected = 107,
  kShuttingDown = 108,
  kConnectionRefused = 111,
  kAlreadyInProgress = 114,
  kOperationInProgress = 115,
  kBadRing = 192,
  kUnsupportedKernelVersion,
  kReentrantDrive,
  kSqRingFull,
  kSlabExhausted,
  kGenerationExhausted,
  kInvalidToken,
  kProtocolViolation,
  kDrainTimeout,
  kUnmappedErrno
};
static_assert(uarch::RegisterPassable<UringErrorCode>);
template <class T>
using Result = std::expected<T, UringErrorCode>;

// The syscall errno range exceeds a byte; retain its number before narrowing to a domain code.
struct SyscallError {
  std::uint16_t number;
  [[nodiscard]] constexpr UringErrorCode code() const noexcept {
    return number > 0 && number <= 133 ? static_cast<UringErrorCode>(number) : UringErrorCode::kUnmappedErrno;
  }
};
static_assert(uarch::NoPadding<SyscallError> && uarch::RegisterPassable<SyscallError>);
using SyscallResult = std::expected<std::int64_t, SyscallError>;
[[nodiscard]] constexpr SyscallResult FromRawSyscall(std::int64_t raw) noexcept {
  if (raw < 0 && raw >= -4095) return std::unexpected(SyscallError{static_cast<std::uint16_t>(-raw)});
  return raw;
}
[[nodiscard]] constexpr Result<std::int64_t> ToResult(std::int64_t raw) noexcept {
  auto value = FromRawSyscall(raw);
  if (!value) return std::unexpected(value.error().code());
  return *value;
}
inline constexpr std::array<std::string_view, 134> kLinuxErrnoNames{
    "reserved Linux errno 0",
    "EPERM",
    "ENOENT",
    "ESRCH",
    "EINTR",
    "EIO",
    "ENXIO",
    "E2BIG",
    "ENOEXEC",
    "EBADF",
    "ECHILD",
    "EAGAIN",
    "ENOMEM",
    "EACCES",
    "EFAULT",
    "ENOTBLK",
    "EBUSY",
    "EEXIST",
    "EXDEV",
    "ENODEV",
    "ENOTDIR",
    "EISDIR",
    "EINVAL",
    "ENFILE",
    "EMFILE",
    "ENOTTY",
    "ETXTBSY",
    "EFBIG",
    "ENOSPC",
    "ESPIPE",
    "EROFS",
    "EMLINK",
    "EPIPE",
    "EDOM",
    "ERANGE",
    "EDEADLOCK",
    "ENAMETOOLONG",
    "ENOLCK",
    "ENOSYS",
    "ENOTEMPTY",
    "ELOOP",
    "reserved Linux errno 41",
    "ENOMSG",
    "EIDRM",
    "ECHRNG",
    "EL2NSYNC",
    "EL3HLT",
    "EL3RST",
    "ELNRNG",
    "EUNATCH",
    "ENOCSI",
    "EL2HLT",
    "EBADE",
    "EBADR",
    "EXFULL",
    "ENOANO",
    "EBADRQC",
    "EBADSLT",
    "reserved Linux errno 58",
    "EBFONT",
    "ENOSTR",
    "ENODATA",
    "ETIME",
    "ENOSR",
    "ENONET",
    "ENOPKG",
    "EREMOTE",
    "ENOLINK",
    "EADV",
    "ESRMNT",
    "ECOMM",
    "EPROTO",
    "EMULTIHOP",
    "EDOTDOT",
    "EBADMSG",
    "EOVERFLOW",
    "ENOTUNIQ",
    "EBADFD",
    "EREMCHG",
    "ELIBACC",
    "ELIBBAD",
    "ELIBSCN",
    "ELIBMAX",
    "ELIBEXEC",
    "EILSEQ",
    "ERESTART",
    "ESTRPIPE",
    "EUSERS",
    "ENOTSOCK",
    "EDESTADDRREQ",
    "EMSGSIZE",
    "EPROTOTYPE",
    "ENOPROTOOPT",
    "EPROTONOSUPPORT",
    "ESOCKTNOSUPPORT",
    "ENOTSUP",
    "EPFNOSUPPORT",
    "EAFNOSUPPORT",
    "EADDRINUSE",
    "EADDRNOTAVAIL",
    "ENETDOWN",
    "ENETUNREACH",
    "ENETRESET",
    "ECONNABORTED",
    "ECONNRESET",
    "ENOBUFS",
    "EISCONN",
    "ENOTCONN",
    "ESHUTDOWN",
    "ETOOMANYREFS",
    "ETIMEDOUT",
    "ECONNREFUSED",
    "EHOSTDOWN",
    "EHOSTUNREACH",
    "EALREADY",
    "EINPROGRESS",
    "ESTALE",
    "EUCLEAN",
    "ENOTNAM",
    "ENAVAIL",
    "EISNAM",
    "EREMOTEIO",
    "EDQUOT",
    "ENOMEDIUM",
    "EMEDIUMTYPE",
    "ECANCELED",
    "ENOKEY",
    "EKEYEXPIRED",
    "EKEYREVOKED",
    "EKEYREJECTED",
    "EOWNERDEAD",
    "ENOTRECOVERABLE",
    "ERFKILL",
    "EHWPOISON",
};
[[nodiscard]] constexpr std::string_view ErrorName(UringErrorCode code) noexcept {
  switch (code) {
    case UringErrorCode::kPermissionDenied:
      return "permission denied";
    case UringErrorCode::kNoEntry:
      return "no such entry";
    case UringErrorCode::kInterrupted:
      return "interrupted";
    case UringErrorCode::kIo:
      return "I/O error";
    case UringErrorCode::kBadFd:
      return "bad file descriptor";
    case UringErrorCode::kAgain:
      return "try again";
    case UringErrorCode::kNoMemory:
      return "out of memory";
    case UringErrorCode::kAccessDenied:
      return "access denied";
    case UringErrorCode::kFault:
      return "bad address";
    case UringErrorCode::kBusy:
      return "resource busy";
    case UringErrorCode::kInvalid:
      return "invalid argument";
    case UringErrorCode::kTooManyFiles:
      return "too many files";
    case UringErrorCode::kNoSpace:
      return "no space left";
    case UringErrorCode::kNotImplemented:
      return "not implemented";
    case UringErrorCode::kOverflow:
      return "value overflow";
    case UringErrorCode::kNotSupported:
      return "not supported";
    case UringErrorCode::kTimedOut:
      return "timed out";
    case UringErrorCode::kCanceled:
      return "canceled";
    case UringErrorCode::kFileExists:
      return "file exists";
    case UringErrorCode::kBrokenPipe:
      return "broken pipe";
    case UringErrorCode::kBadRequestDescriptor:
      return "bad request descriptor";
    case UringErrorCode::kTimerExpired:
      return "timer expired";
    case UringErrorCode::kConnectionReset:
      return "connection reset";
    case UringErrorCode::kNoBufferSpace:
      return "no buffer space";
    case UringErrorCode::kTransportConnected:
      return "transport connected";
    case UringErrorCode::kTransportNotConnected:
      return "transport not connected";
    case UringErrorCode::kShuttingDown:
      return "shutting down";
    case UringErrorCode::kConnectionRefused:
      return "connection refused";
    case UringErrorCode::kAlreadyInProgress:
      return "already in progress";
    case UringErrorCode::kOperationInProgress:
      return "operation in progress";
    case UringErrorCode::kBadRing:
      return "invalid ring state";
    case UringErrorCode::kUnsupportedKernelVersion:
      return "unsupported kernel version";
    case UringErrorCode::kReentrantDrive:
      return "reentrant proactor drive";
    case UringErrorCode::kSqRingFull:
      return "submission queue full";
    case UringErrorCode::kSlabExhausted:
      return "operation slab exhausted";
    case UringErrorCode::kGenerationExhausted:
      return "operation generation exhausted";
    case UringErrorCode::kInvalidToken:
      return "invalid operation token";
    case UringErrorCode::kProtocolViolation:
      return "completion protocol violation";
    case UringErrorCode::kDrainTimeout:
      return "drain deadline elapsed";
    case UringErrorCode::kUnmappedErrno:
      return "errno outside byte domain; inspect SyscallError";
  }
  const auto number = static_cast<unsigned>(code);
  if (number > 0 && number < kLinuxErrnoNames.size()) return kLinuxErrnoNames[number];
  return "unknown liburing error";
}
class UringErrorCategory final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override { return "liburing"; }
  [[nodiscard]] std::string message(int value) const override {
    if (value > 0 && value <= 133) return std::generic_category().message(value);
    if (value < 192 || value > static_cast<int>(UringErrorCode::kUnmappedErrno)) return "unknown liburing error";
    return std::string(ErrorName(static_cast<UringErrorCode>(value)));
  }
};
[[nodiscard]] inline const std::error_category& UringCategory() noexcept {
  static UringErrorCategory category;
  return category;
}
[[nodiscard]] inline std::error_code make_error_code(UringErrorCode code) noexcept {
  return {static_cast<int>(code), UringCategory()};
}
}  // namespace liburing
namespace std {
template <>
struct is_error_code_enum<liburing::UringErrorCode> : true_type {};
}  // namespace std
