#include <liburing/futex2/futex2.hpp>
using liburing::futex2::FutexRef;
using liburing::futex2::FutexWord32;
FutexRef EchoRef(FutexRef ref) noexcept asm("futex_ref_echo");
FutexRef EchoRef(FutexRef ref) noexcept { return ref; }
extern "C" std::uint32_t futex_load_probe(FutexWord32* __restrict word) noexcept {
  return liburing::futex2::LoadAcquire(*word);
}
extern "C" void futex_store_probe(FutexWord32* __restrict word, std::uint32_t value) noexcept {
  liburing::futex2::StoreRelease(*word, value);
}
extern "C" std::uint32_t futex_add_probe(FutexWord32* __restrict word, std::uint32_t value) noexcept {
  return liburing::futex2::FetchAdd(*word, value);
}
extern "C" std::uint64_t futex_wake_probe(liburing::futex2::FutexWord32* __restrict word) noexcept {
  auto ref = liburing::futex2::MakeRef(*word);
  if (!ref) return static_cast<std::uint64_t>(ref.error());
  auto result = liburing::futex2::Wake(*ref, 0);
  return result ? *result : static_cast<std::uint64_t>(result.error());
}
