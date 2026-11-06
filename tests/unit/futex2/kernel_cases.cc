#include <pthread.h>

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <liburing/futex2/futex2.hpp>
#include <thread>

#include "../../support/host_kernel.hpp"
using namespace liburing;
using namespace liburing::futex2;
namespace {
std::int64_t Now() noexcept {
  abi::KernelTimespec time{};
  if (sys::Syscall6(228, 1, sys::Address(&time)) != 0) return -1;
  return time.tv_sec * 1000000000 + time.tv_nsec;
}
abi::AbsoluteDeadline Deadline() noexcept {
  abi::KernelTimespec time{};
  if (sys::Syscall6(228, 1, sys::Address(&time)) != 0) std::abort();
  time.tv_sec += 10;
  return {time, abi::FutexClock::kMonotonic, 0};
}
bool Parked(std::int32_t tid, std::int64_t syscall) noexcept {
  if (tid <= 0) return false;
  char path[96];
  char line[1024];
  std::snprintf(path, sizeof(path), "/proc/%d/stat", tid);
  auto* file = std::fopen(path, "r");
  if (!file) return false;
  const bool read = std::fgets(line, sizeof(line), file) != nullptr;
  std::fclose(file);
  const auto* name_end = read ? std::strrchr(line, ')') : nullptr;
  if (!name_end || name_end[1] != ' ' || name_end[2] != 'S') return false;
  std::snprintf(path, sizeof(path), "/proc/%d/syscall", tid);
  file = std::fopen(path, "r");
  if (!file) return false;
  const bool syscall_read = std::fgets(line, sizeof(line), file) != nullptr;
  std::fclose(file);
  if (!syscall_read) return false;
  std::int64_t current = -1;
  const auto parsed = std::from_chars(line, line + std::strlen(line), current);
  return parsed.ec == std::errc{} && current == syscall;
}
bool AwaitPark(const std::atomic<std::int32_t>& tid, std::int64_t syscall) noexcept {
  const auto until = Now() + 3000000000;
  while (Now() < until) {
    if (Parked(tid.load(std::memory_order_acquire), syscall)) return true;
    std::this_thread::yield();
  }
  return false;
}
struct Join {
  std::thread worker;
  ~Join() {
    if (worker.joinable()) worker.join();
  }
};
void OnSignal(int) noexcept {}
}  // namespace
// NOLINTNEXTLINE(readability-identifier-naming) -- Entry point named by the generated CTest driver.
int u32_cross_thread_wake() noexcept {
  FutexWord32 word{};
  auto ref = MakeRef(word);
  if (!ref) return 1;
  std::atomic<std::int32_t> tid{};
  Result<WaitOutcome> outcome = std::unexpected(UringErrorCode::kProtocolViolation);
  const auto deadline = Deadline();
  Join thread{std::thread([&] {
    tid.store(static_cast<std::int32_t>(sys::Syscall6(186)), std::memory_order_release);
    outcome = Wait(*ref, 0, deadline);
  })};
  if (!AwaitPark(tid, sys::kSysFutexWait)) return 2;
  StoreRelease(word, 1);
  auto woken = Wake(*ref);
  thread.worker.join();
  return woken && *woken == 1 && outcome && *outcome == WaitOutcome::kWoken && LoadAcquire(word) == 1 ? 0 : 3;
}
// NOLINTNEXTLINE(readability-identifier-naming) -- Entry point named by the generated CTest driver.
int numa_node_writeback() noexcept {
  if (const int gate = test_support::SkipBelowKernel(6, 14); gate >= 0) return gate;
  for (const bool mpol : {false, true}) {
    FutexNumaWord32 word{7, abi::kFutexNoNode};
    auto ref = MakeNumaRef(word, FutexScope::kShared, mpol);
    if (!ref) return 1;
    auto result = Wait(*ref, 8);
    if (!result || *result != WaitOutcome::kValueChanged || word.node == abi::kFutexNoNode) return 2;
    auto wake = Wake(*ref, 0);
    if (!wake || *wake != 0) return 3;
  }
  return 0;
}
// NOLINTNEXTLINE(readability-identifier-naming) -- Entry point named by the generated CTest driver.
int requeue_transfers_waiters() noexcept {
  FutexWord32 source{};
  FutexWord32 target{};
  auto from = MakeRef(source);
  auto to = MakeRef(target);
  if (!from || !to) return 1;
  std::atomic<std::int32_t> first_tid{};
  std::atomic<std::int32_t> second_tid{};
  std::atomic<int> finished{};
  Result<WaitOutcome> first = std::unexpected(UringErrorCode::kProtocolViolation);
  Result<WaitOutcome> second = std::unexpected(UringErrorCode::kProtocolViolation);
  const auto deadline = Deadline();
  Join a{std::thread([&] {
    first_tid.store(static_cast<std::int32_t>(sys::Syscall6(186)), std::memory_order_release);
    first = Wait(*from, 0, deadline);
    finished.fetch_add(1, std::memory_order_release);
  })};
  Join b{std::thread([&] {
    second_tid.store(static_cast<std::int32_t>(sys::Syscall6(186)), std::memory_order_release);
    second = Wait(*from, 0, deadline);
    finished.fetch_add(1, std::memory_order_release);
  })};
  if (!AwaitPark(first_tid, sys::kSysFutexWait) || !AwaitPark(second_tid, sys::kSysFutexWait)) return 2;
  auto transferred = Requeue(*from, 0, *to, 1, 1);
  if (!transferred || *transferred != 2) return 3;
  const auto until = Now() + 3000000000;
  while (finished.load(std::memory_order_acquire) != 1 && Now() < until) std::this_thread::yield();
  if (finished.load(std::memory_order_acquire) != 1) return 4;
  auto woken = Wake(*to, 1);
  a.worker.join();
  b.worker.join();
  return woken && *woken == 1 && first && second && *first == WaitOutcome::kWoken && *second == WaitOutcome::kWoken ? 0 : 5;
}
// NOLINTNEXTLINE(readability-identifier-naming) -- Entry point named by the generated CTest driver.
int wait_any_woken_index() noexcept {
  FutexWord32 words[3]{};
  abi::FutexWaitv waiters[3]{};
  for (std::size_t i = 0; i < 3; ++i) {
    auto ref = MakeRef(words[i]);
    if (!ref) return 1;
    auto waiter = MakeWaitv(*ref, 0);
    if (!waiter) return 2;
    waiters[i] = *waiter;
  }
  std::atomic<std::int32_t> tid{};
  Result<WaitAnyOutcome> result = std::unexpected(UringErrorCode::kProtocolViolation);
  const auto deadline = Deadline();
  Join thread{std::thread([&] {
    tid.store(static_cast<std::int32_t>(sys::Syscall6(186)), std::memory_order_release);
    result = WaitAny(waiters, deadline);
  })};
  if (!AwaitPark(tid, sys::kSysFutexWaitv)) return 3;
  auto target = MakeRef(words[2]);
  if (!target) return 4;
  auto woken = Wake(*target);
  thread.worker.join();
  return woken && *woken == 1 && result && result->outcome == WaitOutcome::kWoken && result->index == 2 ? 0 : 5;
}
// NOLINTNEXTLINE(readability-identifier-naming) -- Entry point named by the generated CTest driver.
int wait_interrupted() noexcept {
  struct sigaction action{};
  struct sigaction old{};
  action.sa_handler = OnSignal;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, &old)) return 1;
  struct Restore {
    struct sigaction saved;
    ~Restore() { static_cast<void>(sigaction(SIGUSR1, &saved, nullptr)); }
  } restore{old};
  for (const bool vector : {false, true}) {
    FutexWord32 word{};
    auto ref = MakeRef(word);
    if (!ref) return 2;
    auto waiter = MakeWaitv(*ref, 0);
    if (!waiter) return 3;
    std::atomic<std::int32_t> tid{};
    WaitOutcome observed = WaitOutcome::kTimedOut;
    bool success = false;
    const auto deadline = Deadline();
    Join thread{std::thread([&] {
      tid.store(static_cast<std::int32_t>(sys::Syscall6(186)), std::memory_order_release);
      if (vector) {
        auto result = WaitAny(std::span<const abi::FutexWaitv>{&*waiter, 1}, deadline);
        if (result) {
          success = true;
          observed = result->outcome;
        }
      } else {
        auto result = Wait(*ref, 0, deadline);
        if (result) {
          success = true;
          observed = *result;
        }
      }
    })};
    if (!AwaitPark(tid, vector ? sys::kSysFutexWaitv : sys::kSysFutexWait)) return 4;
    if (pthread_kill(thread.worker.native_handle(), SIGUSR1)) return 5;
    thread.worker.join();
    if (!success || observed != WaitOutcome::kInterrupted) return 6;
  }
  return 0;
}
