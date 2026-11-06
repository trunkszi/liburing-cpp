#include <array>
#include <liburing/futex2/futex2.hpp>
#include <limits>
using namespace liburing;
using namespace liburing::futex2;
int main() {
  FutexWord32 word{7};
  auto ref = MakeRef(word);
  if (!ref) return 1;
  auto changed = Wait(*ref, 8);
  if (!changed || *changed != WaitOutcome::kValueChanged) return 2;
  abi::AbsoluteDeadline expired{};
  auto timed = Wait(*ref, 7, expired);
  if (!timed || *timed != WaitOutcome::kTimedOut) return 3;
  expired.clock = abi::FutexClock::kRealtime;
  auto realtime = Wait(*ref, 7, expired);
  if (!realtime || *realtime != WaitOutcome::kTimedOut) return 4;
  if (Wait(*ref, 7, std::nullopt, 0).error() != UringErrorCode::kInvalid) return 5;
  if (Wake(*ref, -1).error() != UringErrorCode::kInvalid) return 6;
  if (Wake(*ref, 1, 0).error() != UringErrorCode::kInvalid) return 7;
  expired.time.tv_nsec = 1000000000;
  if (Wait(*ref, 8, expired).error() != UringErrorCode::kInvalid) return 8;
  auto waiter = MakeWaitv(*ref, 8);
  if (!waiter) return 9;
  std::array<abi::FutexWaitv, 1> one{*waiter};
  auto any = WaitAny(one);
  if (!any || any->outcome != WaitOutcome::kValueChanged) return 10;
  if (WaitAny({}).error() != UringErrorCode::kInvalid) return 11;
  one[0].reserved = 1;
  if (WaitAny(one).error() != UringErrorCode::kInvalid) return 12;
  one[0] = *waiter;
  one[0].val = std::uint64_t{1} << 32;
  if (WaitAny(one).error() != UringErrorCode::kInvalid) return 13;
  FutexWord32 target{};
  auto to = MakeRef(target);
  if (!to || Requeue(*ref, 8, *to, 0, 0).error() != UringErrorCode::kAgain) return 14;
  abi::FutexWaitv pair[2]{*MakeWaitv(*ref, 7), *MakeWaitv(*to, 0)};
  if (sys::SysFutexRequeue(pair, 1, 0, 0) != -22) return 15;
  one[0] = *MakeWaitv(*ref, 7);
  auto any_timed = WaitAny(one, abi::AbsoluteDeadline{});
  if (!any_timed || any_timed->outcome != WaitOutcome::kTimedOut) return 16;
  std::array<abi::FutexWaitv, 129> oversized{};
  if (WaitAny(oversized).error() != UringErrorCode::kInvalid) return 17;
  if (Requeue(*ref, 7, *to, -1, 1).error() != UringErrorCode::kInvalid) return 18;
  if (Requeue(*ref, 7, *to, 1, -1).error() != UringErrorCode::kInvalid) return 19;
  FutexRef fault{0x1000, 2, 0};
  if (Wait(fault, 1).error() != UringErrorCode::kFault) return 20;
  FutexNumaWord32 numa{};
  auto numa_default = MakeNumaRef(numa);
  auto numa_mpol = MakeNumaRef(numa, FutexScope::kShared, FutexLocality::kNumaMpol);
  if (!numa_default || numa_default->flags != (abi::kFutex2SizeU32 | abi::kFutex2Numa)) return 21;
  if (!numa_mpol || numa_mpol->flags != (abi::kFutex2SizeU32 | abi::kFutex2Numa | abi::kFutex2Mpol)) return 22;
  if (MakeNumaRef(numa, FutexScope::kShared, FutexLocality::kDefault).error() != UringErrorCode::kInvalid) return 23;
  if (MakeNumaRef(numa, FutexScope::kShared, static_cast<FutexLocality>(8)).error() != UringErrorCode::kInvalid) return 24;
  return 0;
}
