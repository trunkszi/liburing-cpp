#include <cerrno>
#include <cstdint>
#include <liburing/error/uring_error.hpp>
#include <liburing/sys/futex_sys.hpp>
#include <liburing/sys/syscall.hpp>
#include <liburing/sys/uring_sys.hpp>
#include <limits>
using namespace liburing;
static_assert(sizeof(UringErrorCode) == 1);
static_assert(FromRawSyscall(-4095).error().number == 4095);
static_assert(FromRawSyscall(-9).error().code() == UringErrorCode::kBadFd);
static_assert(FromRawSyscall(-4096).value() == -4096);
static_assert(FromRawSyscall(42).value() == 42);
int main() {
  errno = 177;
  if (sys::Syscall6(liburing::sys::kSysGetpid) <= 0 || errno != 177) return 1;
  const auto bad = sys::SysIoUringEnter(-1, 0, 0, 0);
  if (bad != -9 || errno != 177) return 2;
  const auto six = sys::Syscall6(999999, 1, 2, 3, 4, 5, 6);
  if (six != -38 || errno != 177) return 3;
  std::uint32_t word = 0;
  if (sys::SysFutexWake(&word, 0xffffffffU, 0, abi::kFutex2SizeU32 | abi::kFutex2Private) != 0) return 4;
  const auto flags = abi::kFutex2SizeU32 | abi::kFutex2Private;
  if (sys::SysFutexWait(&word, 1, 0xffffffffU, flags) != -11) return 5;
  abi::FutexWaitv waiter{.val = 1, .uaddr = sys::Address(&word), .flags = flags};
  if (sys::SysFutexWaitv(&waiter, 1, 0) != -11) return 6;
  std::uint32_t target = 0;
  abi::FutexWaitv pair[2]{{.uaddr = sys::Address(&word), .flags = flags}, {.uaddr = sys::Address(&target), .flags = flags}};
  if (sys::SysFutexRequeue(pair, 0, 0, 0) != 0) return 7;
  if (errno != 177) return 8;
  for (std::int64_t number = 1; number <= 4095; ++number) {
    const auto value = FromRawSyscall(-number);
    if (value || value.error().number != number) return 9;
  }
  return 0;
}
