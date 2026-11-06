#include <liburing/core/completion.hpp>
#include <liburing/core/op_header.hpp>

extern "C" std::uint64_t t2_generation_delta(liburing::UserDataToken token, liburing::OpHeader header) noexcept {
  return (token.raw() ^ header.raw()) >> 32;
}
liburing::OperationKey t2_key_pair(liburing::OperationKey key) noexcept asm("t2_key_pair");
liburing::OperationKey t2_key_pair(liburing::OperationKey key) noexcept { return key; }
liburing::Completion t2_completion_pair(liburing::Completion completion) noexcept asm("t2_completion_pair");
liburing::Completion t2_completion_pair(liburing::Completion completion) noexcept { return completion; }
extern "C" liburing::OpState t2_fsm_next(liburing::OpKind kind, liburing::OpState state, liburing::CqeClass event) noexcept {
  return liburing::kNext[static_cast<unsigned>(kind)][static_cast<unsigned>(state)][static_cast<unsigned>(event)];
}
liburing::OpHeader t2_advance(liburing::OpHeader header, liburing::CqeClass event) noexcept asm("t2_advance");
liburing::OpHeader t2_advance(liburing::OpHeader header, liburing::CqeClass event) noexcept {
  return liburing::Advance(header, event);
}
