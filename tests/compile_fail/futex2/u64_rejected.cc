// expected-error: currently only implements FUTEX2_SIZE_U32
#include <liburing/futex2/futex_flags.hpp>
void Unsupported(liburing::futex2::FutexWord64& word) { static_cast<void>(liburing::futex2::MakeRef(word)); }
