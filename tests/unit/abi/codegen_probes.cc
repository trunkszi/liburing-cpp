#include <liburing/abi/proofs.hpp>
#include <liburing/sys/syscall.hpp>
liburing::abi::FlatCqe AbiCqeEcho(liburing::abi::FlatCqe value) noexcept asm("abi_cqe_echo");
liburing::abi::FlatCqe AbiCqeEcho(liburing::abi::FlatCqe value) noexcept { return value; }
// NOLINTNEXTLINE(readability-identifier-naming) -- Stable symbols consumed by codegen gates.
extern "C" std::uint64_t abi_sqe_offset(const liburing::abi::FlatSqe* __restrict sqe) noexcept { return sqe->off(); }
// NOLINTNEXTLINE(readability-identifier-naming) -- Stable symbols consumed by codegen gates.
extern "C" void abi_cmd_store(liburing::abi::FlatSqe* __restrict sqe, std::uint32_t command) noexcept { sqe->cmd_op(command); }
extern "C" std::int64_t syscall_pid_probe() noexcept { return liburing::sys::Syscall6(liburing::sys::kSysGetpid); }

extern "C" [[noreturn, gnu::cold, gnu::noinline]] void invariant_panic_probe() noexcept {
  liburing::sys::Panic("invariant probe");
}
