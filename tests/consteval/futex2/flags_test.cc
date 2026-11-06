#include <liburing/futex2/futex_flags.hpp>
using namespace liburing::futex2;
static_assert(ValidateFutex2Flags(0x02).has_value());
static_assert(ValidateFutex2Flags(0x8e).has_value());
static_assert(!ValidateFutex2Flags(0x0a));
static_assert(!ValidateFutex2Flags(0x03));
static_assert(!ValidateFutex2Flags(0x12));
static_assert(Futex2Flags<>() == 0x02);
static_assert(Futex2Flags<FutexScope::kProcessPrivate, FutexLocality::kNumaMpol>() == 0x8e);
static_assert(!ValidateRef(FutexRef{0, 2, 0}));
static_assert(!ValidateRef(FutexRef{0x1004, 6, 0}));
static_assert(ValidateRef(FutexRef{0x1008, 6, 0}).has_value());
int main() {
  FutexWord32 word{};
  auto ref = MakeRef(word);
  if (!ref || ref->flags != 2 || ref->reserved != 0) return 1;
  auto waiter = MakeWaitv(*ref, 7);
  if (!waiter || waiter->val != 7 || waiter->flags != 2 || waiter->reserved != 0) return 2;
  FutexNumaWord32 numa{};
  auto local = MakeNumaRef(numa, FutexScope::kProcessPrivate, true);
  if (!local || local->flags != 0x8e) return 3;
  if (MakeRef(word, static_cast<FutexScope>(4))) return 4;
  return 0;
}
