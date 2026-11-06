#include <liburing/futex2/futex_word.hpp>
using namespace liburing::futex2;
static_assert(sizeof(FutexWord32) == 4 && alignof(FutexWord32) == 4);
static_assert(sizeof(FutexNumaWord32) == 8 && alignof(FutexNumaWord32) == 8);
int main() {
  FutexWord32 word{7};
  if (LoadAcquire(word) != 7) return 1;
  StoreRelease(word, 9);
  std::uint32_t expected = 8;
  if (CompareExchange(word, expected, 12) || expected != 9) return 2;
  if (!CompareExchange(word, expected, 12)) return 3;
  if (FetchAdd(word, 3) != 12 || LoadAcquire(word) != 15) return 4;
  return 0;
}
