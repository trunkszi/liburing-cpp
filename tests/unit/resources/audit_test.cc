#include <fcntl.h>

#include <array>
#include <cstring>
#include <liburing/abi/proofs.hpp>
#include <liburing/ops/fs.hpp>
#include <liburing/resources/fixed_buffer_table.hpp>
#include <liburing/resources/fixed_file_table.hpp>
#include <liburing/resources/provided_buffer_ring.hpp>
#include <liburing/resources/zcrx_ifq.hpp>
#include <liburing/ring/ring.hpp>
using namespace liburing;
#define REQUIRE(condition)             \
  do {                                 \
    if (!(condition)) return __LINE__; \
  } while (false)
int ZcrxStride() {
  struct KernelRqe {
    std::uint64_t off;
    std::uint32_t len;
    std::uint32_t pad;
  };
  alignas(8) std::array<std::uint64_t, 8> storage{};
  std::uint32_t head = 0, tail = 0;
  ZcrxRefillQueue queue{&head, &tail, 4, storage.data()};
  REQUIRE(queue.EnqueueAreaOffset(3, 4096, 123));
  REQUIRE(queue.EnqueueAreaOffset(7, 8192, 456));
  REQUIRE(queue.FlushTail() == 2 && tail == 2);
  std::array<KernelRqe, 4> view{};
  std::memcpy(view.data(), storage.data(), sizeof(view));
  REQUIRE(view[0].off == ((3ULL << 48) | 4096) && view[0].len == 123 && view[0].pad == 0);
  REQUIRE(view[1].off == ((7ULL << 48) | 8192) && view[1].len == 456 && view[1].pad == 0);
  head = 2;
  for (unsigned i = 0; i < 4; ++i) REQUIRE(queue.EnqueueAreaOffset(9, 16384 + i, 100 + i));
  REQUIRE(!queue.EnqueueAreaOffset(9, 0, 1) && queue.FlushTail() == 4 && tail == 6);
  std::memcpy(view.data(), storage.data(), sizeof(view));
  REQUIRE(view[0].off == ((9ULL << 48) | 16386) && view[0].len == 102 && view[0].pad == 0);
  return 0;
}
int LeaseLifetime() {
  std::array<abi::IoUringBuf, 4> descriptors{};
  std::array<std::byte, 256> backing{};
  auto ring = ProvidedBufferRing::CreateInMemory(descriptors, backing, 64, 1, true);
  REQUIRE(ring);
  auto first = ring->AcquireLease(24, abi::kCqeBuffer | abi::kCqeBufMore);
  auto final = ring->AcquireLease(40, abi::kCqeBuffer);
  REQUIRE(first && final && final->Bytes().data() == backing.data() + 24);
  auto duplicate = ring->AcquireLease(0, abi::kCqeBuffer);
  REQUIRE(!duplicate && duplicate.error() == UringErrorCode::kBusy);
  final->Release();
  REQUIRE(ring->SharedTail() == 4 && first->IsActive());
  first->Release();
  REQUIRE(ring->SharedTail() == 5);
  auto full = ring->AcquireLease(64, abi::kCqeBuffer);
  REQUIRE(full && full->Bytes().data() == backing.data());
  REQUIRE(!ring->AcquireBundleLease(128, abi::kCqeBuffer));
  full->Release();
  auto bundle = ring->AcquireBundleLease(128, abi::kCqeBuffer | (1U << abi::kCqeBufferShift));
  REQUIRE(bundle && bundle->BufferCount() == 2);
  REQUIRE(!ring->AcquireLease(1, abi::kCqeBuffer | (2U << abi::kCqeBufferShift)));
  REQUIRE(!ring->AcquireBundleLease(64, abi::kCqeBuffer | (1U << abi::kCqeBufferShift)));
  const auto before = ring->SharedTail();
  bundle->Release();
  REQUIRE(ring->SharedTail() == before + 2);
  return 0;
}
int FixedRanges() {
  REQUIRE(!FixedBufferTable<>::Create({65535, 2}));
  REQUIRE(!FixedFileTable<>::Create({0xffffffffU, 2}));
  REQUIRE(!FixedBufferTable<>::Create({0, 65}));
  FixedBufferTable<> invalid_buffer{{65535, 2}};
  FixedFileTable<> invalid_file{{0xffffffffU, 2}};
  REQUIRE(!invalid_buffer.AllocateSlot() && !invalid_file.AllocateSlot());
  auto buffer = FixedBufferTable<>::Create({65535, 1});
  REQUIRE(buffer && *buffer->AllocateSlot() == 65535 && !buffer->AllocateSlot());
  ScriptedRing ring{8, 16};
  FixedFileTable<> files{{0, 4}};
  auto occupied = files.AllocateSlot();
  REQUIRE(occupied && !files.ConfigureKernelAllocRange(ring, {0, 2}));
  REQUIRE(files.RegisterSparseFiles(ring, 4));
  REQUIRE(files.ConfigureKernelAllocRange(ring, {2, 2}));
  REQUIRE(files.KernelAllocRange().offset == 2 && files.KernelAllocRange().count == 2);
  REQUIRE(!files.ConfigureKernelAllocRange(ring, {3, 2}));
  REQUIRE(!files.ReleaseSlot(2));
  auto local = files.AllocateSlot();
  REQUIRE(local && *local == 1 && !files.AllocateSlot());
  REQUIRE(files.ReleaseSlot(*occupied) && files.ReleaseSlot(*local));
  REQUIRE(files.ConfigureKernelAllocRange(ring, {0, 2}));
  REQUIRE(*files.AllocateSlot() == 2);
  ResourceRegistry registry{6, 0};
  auto a_range = registry.AllocateFileRange(2);
  auto b_range = registry.AllocateFileRange(2);
  REQUIRE(a_range && b_range && b_range->offset == 2);
  FixedFileTable<> a{*a_range};
  REQUIRE(a.RegisterSparseFiles(ring, 6));
  REQUIRE(!a.ConfigureKernelAllocRange(ring, *b_range));
  auto kernel_range = registry.ConfigureKernelFileAllocRange(ring, 2);
  REQUIRE(kernel_range && kernel_range->offset == 4 && kernel_range->count == 2 && registry.RemainingFiles() == 0);
  return 0;
}
int RegistryResize() {
  ResourceRegistry registry;
  ScriptedRing scripted{8, 16};
  abi::IoUringParams params{};
  params.sq_entries = 16;
  params.cq_entries = 32;
  REQUIRE(registry.ResizeRings(scripted, params));
  REQUIRE(scripted.Sq().RingEntries() == 16 && scripted.Cq().RingEntries() == 32);
  auto kernel = KernelRing::Create(8);
  REQUIRE(kernel);
  auto resized = registry.ResizeRings(*kernel, params);
  REQUIRE(!resized && resized.error() == UringErrorCode::kNotSupported && kernel->Sq().RingEntries() == 8);
  return 0;
}
int ErrorNames() {
  for (unsigned i = 1; i <= 133; ++i) {
    const auto name = ErrorName(static_cast<UringErrorCode>(i));
    REQUIRE(!name.empty() && name != "unknown liburing error");
  }
  REQUIRE(ErrorName(static_cast<UringErrorCode>(98)) == "EADDRINUSE");
  return 0;
}
int KernelPartition() {
  auto ring = KernelRing::Create(8);
  REQUIRE(ring);
  FixedFileTable<> files{{0, 4}};
  REQUIRE(!files.RegisterSparseFiles(*ring, 3));
  REQUIRE(files.RegisterSparseFiles(*ring, 4));
  auto submit = [&](auto op) noexcept -> std::int32_t {
    auto sqe = ring->Sq().TryReserve64();
    if (!sqe) return -999;
    op.Encode(**sqe);
    if (!ring->SubmitAndWait(1)) return -998;
    std::array<Completion, 1> completion{};
    if (ring->Cq().ReapBatch(completion) != 1) return -997;
    return completion[0].result;
  };
  REQUIRE(submit(ops::PrepareOpenatDirect(AT_FDCWD, "/dev/null", O_RDONLY, 0, abi::kFileIndexAlloc)) < 0);
  REQUIRE(!files.ConfigureKernelAllocRange(*ring, {3, 2}));
  REQUIRE(files.ConfigureKernelAllocRange(*ring, {2, 2}));
  const auto automatic = submit(ops::PrepareOpenatDirect(AT_FDCWD, "/dev/null", O_RDONLY, 0, abi::kFileIndexAlloc));
  REQUIRE(automatic >= 2 && automatic < 4);
  auto local = files.AllocateSlot();
  REQUIRE(local && *local < 2);
  REQUIRE(submit(ops::PrepareOpenatDirect(AT_FDCWD, "/dev/zero", O_RDONLY, 0, *local)) == 0);
  REQUIRE(files.UnregisterFiles(*ring));
  return 0;
}
int main() {
  if (auto result = ZcrxStride()) return result;
  if (auto result = LeaseLifetime()) return result;
  if (auto result = FixedRanges()) return result;
  if (auto result = RegistryResize()) return result;
  if (auto result = ErrorNames()) return result;
  return KernelPartition();
}
