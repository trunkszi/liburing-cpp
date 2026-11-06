#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <liburing/io/direct_file.hpp>
#include <liburing/ring/ring.hpp>

using namespace liburing;
using io::DirectAccess;
using io::DirectFile;
using resources::FixedBufferPool;
#define REQUIRE(condition)                                         \
  do {                                                             \
    if (!(condition)) {                                            \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
      return __LINE__;                                             \
    }                                                              \
  } while (false)

int RejectBuffered() {
  const int raw = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  auto fd = sys::UniqueFd::Adopt(raw);
  REQUIRE(fd);
  auto file = DirectFile::FromOwnedFd(std::move(*fd));
  REQUIRE(!file && file.error() == UringErrorCode::kInvalid);
  REQUIRE(::fcntl(raw, F_GETFL) == -1 && errno == EBADF);
  auto invalid = DirectFile::FromOwnedFd(sys::UniqueFd{});
  REQUIRE(!invalid && invalid.error() == UringErrorCode::kBadFd);
  REQUIRE(!DirectFile::Open(nullptr, DirectAccess::kReadOnly));
  REQUIRE(!DirectFile::Open("/does-not-exist/t8", DirectAccess::kReadOnly));
  return 0;
}

int Transfer(DirectFile& file) {
  REQUIRE(file.Fd() >= 0 && std::has_single_bit(file.MemAlignment()) && std::has_single_bit(file.OffsetAlignment()));
  REQUIRE((::fcntl(file.Fd(), F_GETFL) & O_DIRECT) != 0);
  REQUIRE(file.SupportsPolledIo());
  const auto length = std::max(4096U, file.OffsetAlignment());
  const auto alignment = std::max(64U, file.MemAlignment());
  ScriptedRing scripted{8, 16};
  auto pool = FixedBufferPool::Create(scripted, 1, length, alignment);
  REQUIRE(pool);
  auto buffer = pool->Checkout();
  REQUIRE(buffer);
  REQUIRE(file.ValidateTransfer(buffer->Data(), length, 0) == length);
  auto is_invalid = [](const auto& result) { return !result && result.error() == UringErrorCode::kInvalid; };
  REQUIRE(is_invalid(file.ValidateTransfer(nullptr, length, 0)));
  REQUIRE(is_invalid(file.ValidateTransfer(buffer->Data(), 0, 0)));
  REQUIRE(is_invalid(file.ValidateTransfer(buffer->Data(), static_cast<std::size_t>(UINT32_MAX) + 1, 0)));
  if (file.MemAlignment() > 1) REQUIRE(is_invalid(file.ValidateTransfer(buffer->Data() + 1, length, 0)));
  if (file.OffsetAlignment() > 1) {
    REQUIRE(is_invalid(file.ValidateTransfer(buffer->Data(), length - 1, 0)));
    REQUIRE(is_invalid(file.ValidateTransfer(buffer->Data(), length, 1)));
  }
  REQUIRE(is_invalid(file.PrepareReadFixed(*pool, *buffer, length + 1, 0)));
  REQUIRE(is_invalid(file.PrepareWriteFixed(*pool, *buffer, length + 1, 0)));
  resources::BufferHandle empty;
  REQUIRE(is_invalid(file.PrepareReadFixed(*pool, empty, length, 0)));
  REQUIRE(is_invalid(file.PrepareWriteFixed(*pool, empty, length, 0)));
  auto stale = *buffer;
  REQUIRE(pool->ReleaseCheckout(*buffer));
  buffer = pool->Checkout();
  REQUIRE(buffer && buffer->Generation() != stale.Generation());
  REQUIRE(is_invalid(file.PrepareReadFixed(*pool, stale, length, 0)));
  REQUIRE(is_invalid(file.PrepareWriteFixed(*pool, stale, length, 0)));
  REQUIRE(!pool->HasPendingDma());
  auto foreign = FixedBufferPool::CreateOffline(1, length, alignment);
  REQUIRE(foreign && is_invalid(file.PrepareReadFixed(*foreign, *buffer, length, 0)));
  REQUIRE(is_invalid(file.PrepareWriteFixed(*foreign, *buffer, length, 0)));
  {
    auto read = file.PrepareReadFixed(*pool, *buffer, length, length);
    REQUIRE(read && pool->HasPendingDma());
    REQUIRE(is_invalid(file.PrepareReadFixed(*pool, *buffer, length, 0)));
    REQUIRE(is_invalid(file.PrepareWriteFixed(*pool, *buffer, length, 0)));
    abi::FlatSqe sqe{};
    read->Encode(sqe);
    REQUIRE(sqe.opcode == abi::kOpReadFixed && sqe.fd == file.Fd() && sqe.len == length && sqe.offset == length);
    REQUIRE(sqe.address == reinterpret_cast<std::uintptr_t>(buffer->Data()) && sqe.buf_index() == buffer->Index());
  }
  // No SQE was submitted, so completing the preparation restores this test's slot.
  REQUIRE(pool->CompleteInFlight(*buffer));
  buffer = pool->Checkout();
  REQUIRE(buffer);
  {
    auto write = file.PrepareWriteFixed(*pool, *buffer, length, 0);
    REQUIRE(write && pool->HasPendingDma());
    abi::FlatSqe sqe{};
    write->Encode(sqe);
    REQUIRE(sqe.opcode == abi::kOpWriteFixed && sqe.offset == 0 && sqe.len == length);
  }
  REQUIRE(pool->Quarantine(*buffer));
  REQUIRE(is_invalid(file.PrepareReadFixed(*pool, *buffer, length, 0)));
  REQUIRE(is_invalid(file.PrepareWriteFixed(*pool, *buffer, length, 0)));
  REQUIRE(pool->CompleteInFlight(*buffer));
  REQUIRE(is_invalid(file.PrepareReadFixed(*pool, *buffer, length, 0)));
  REQUIRE(is_invalid(file.PrepareWriteFixed(*pool, *buffer, length, 0)));

  auto ring = KernelRing::Create(8);
  REQUIRE(ring);
  auto registered = FixedBufferPool::Create(*ring, 1, length, alignment);
  REQUIRE(registered);
  auto dma = registered->Checkout();
  REQUIRE(dma);
  std::memset(dma->Data(), 0x5a, length);
  for (bool writing : {true, false}) {
    auto slot = ring->Sq().TryReserve64();
    REQUIRE(slot);
    if (writing) {
      auto op = file.PrepareWriteFixed(*registered, *dma, length, 0);
      REQUIRE(op);
      op->Encode(**slot);
    } else {
      std::memset(dma->Data(), 0, length);
      auto op = file.PrepareReadFixed(*registered, *dma, length, 0);
      REQUIRE(op);
      op->Encode(**slot);
    }
    REQUIRE(registered->HasPendingDma());
    REQUIRE(ring->SubmitAndWait(1));
    std::array<Completion, 1> completions{};
    REQUIRE(ring->Cq().ReapBatch(completions) == 1);
    REQUIRE(completions[0].result == static_cast<std::int32_t>(length));
    REQUIRE(registered->CompleteInFlight(*dma));
    dma = registered->Checkout();
    REQUIRE(dma);
  }
  REQUIRE(std::all_of(dma->Bytes().begin(), dma->Bytes().end(), [](std::uint8_t value) { return value == 0x5a; }));
  REQUIRE(registered->ReleaseCheckout(*dma));
  return 0;
}

int RealFile() {
  char path[] = "/tmp/liburing-t8-XXXXXX";
  const int raw = ::mkstemp(path);
  REQUIRE(raw >= 0);
  auto seed = sys::UniqueFd::Adopt(raw);
  auto read_only = DirectFile::Open(path, DirectAccess::kReadOnly);
  auto file = DirectFile::Open(path, DirectAccess::kReadWrite);
  auto adopted_fd = sys::UniqueFd::Adopt(::open(path, O_RDWR | O_DIRECT | O_CLOEXEC));
  REQUIRE(::unlink(path) == 0);
  REQUIRE(seed && read_only && file && adopted_fd);
  REQUIRE((::fcntl(read_only->Fd(), F_GETFL) & O_ACCMODE) == O_RDONLY);
  REQUIRE((::fcntl(file->Fd(), F_GETFD) & FD_CLOEXEC) != 0);
  auto adopted = DirectFile::FromOwnedFd(std::move(*adopted_fd));
  REQUIRE(adopted && adopted->MemAlignment() == file->MemAlignment());
  REQUIRE(adopted->OffsetAlignment() == file->OffsetAlignment());
  const int fd = file->Fd();
  auto moved = std::move(*file);
  REQUIRE(moved.Fd() == fd && file->Fd() == -1);
  return Transfer(moved);
}

int StatxAlignmentContract() {
  io::StatxLayout info{};
  auto fallback = io::detail::ValidateDioAlignments(info);
  REQUIRE(fallback && fallback->memory == 4096 && fallback->offset == 4096);
  info.stx_mask = io::detail::kStatxDioAlign;
  REQUIRE(!io::detail::ValidateDioAlignments(info));
  info.stx_dio_mem_align = 512;
  REQUIRE(!io::detail::ValidateDioAlignments(info));
  info.stx_dio_offset_align = 4096;
  auto valid = io::detail::ValidateDioAlignments(info);
  REQUIRE(valid && valid->memory == 512 && valid->offset == 4096);
  info.stx_dio_mem_align = 513;
  REQUIRE(!io::detail::ValidateDioAlignments(info));
  info.stx_mask = 0;
  fallback = io::detail::ValidateDioAlignments(info);
  REQUIRE(fallback && fallback->memory == 4096 && fallback->offset == 4096);
  return 0;
}

int main() {
  if (auto result = StatxAlignmentContract()) return result;
  if (auto result = RejectBuffered()) return result;
  return RealFile();
}
