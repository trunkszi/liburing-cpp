// expected-error: ChainShape forbids OpKind::kMultishot members in IOSQE_IO_LINK chains
#include <liburing/ring/submission_queue.hpp>

using InvalidChain = liburing::ring::ChainShape<liburing::ring::OneShot64Op, liburing::ring::Multishot64Op>;
static_assert(InvalidChain::kMemberCount == 2);
