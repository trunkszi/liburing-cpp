#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <liburing/abi/proofs.hpp>
using namespace liburing::abi;

consteval bool Aliases() {
  FlatSqe sqe{};
  sqe.off() = 0x1122334455667788ULL;
  sqe.cmd_op(0xaabbccddU);
  if (sqe.addr2() != 0x11223344aabbccddULL) return false;
  sqe.buf_group() = 7;
  if (sqe.buf_index() != 7) return false;
  sqe.file_index() = 0xaabbccdd;
  sqe.addr_len(0x1234);
  if (sqe.optlen() != 0xaabb1234) return false;
  sqe.splice_fd_in(-42);
  if (sqe.file_index() != 0xffffffd6U || sqe.splice_fd_in() != -42) return false;
  sqe.msg_flags() = 0x0800;
  return sqe.op_flags() == 0x0800 && sqe.rw_flags() == 0x0800;
}
static_assert(Aliases());
int main() {
  FlatSqe128 sqe{};
  sqe.sqe.user_data = 123;
  std::memset(sqe.command().data(), 0x5a, sqe.command().size());
  const auto bytes = std::bit_cast<std::array<std::byte, 128>>(sqe);
  if (sqe.sqe.user_data != 123) return 1;
  for (std::size_t i = 48; i < bytes.size(); ++i)
    if (bytes[i] != std::byte{0x5a}) return 2;
  return 0;
}
