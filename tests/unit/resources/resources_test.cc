#include <array>
#include <cstddef>
#include <cstdint>
#include <liburing/resources/fixed_buffer_table.hpp>
#include <liburing/resources/fixed_file_table.hpp>
#include <liburing/resources/napi.hpp>
#include <liburing/resources/provided_buffer_ring.hpp>
#include <liburing/resources/resource_registry.hpp>
#include <liburing/resources/zcrx_ifq.hpp>
#include <liburing/ring/ring.hpp>
#include <string_view>

namespace {

using liburing::resources::BufferLease;
using liburing::resources::FixedBufferTable;
using liburing::resources::FixedFileTable;
using liburing::resources::IoVec;
using liburing::resources::NapiController;
using liburing::resources::ProvidedBufferRing;
using liburing::resources::ResourceRegistry;
using liburing::resources::ZcrxRefillQueue;
using liburing::ring::KernelRing;
using liburing::ring::RingMode;
using liburing::ring::ScriptedRing;

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool buf_more_blocks_recycle() noexcept {
  alignas(64) std::array<liburing::abi::IoUringBuf, 8> descriptors{};
  alignas(64) std::array<std::byte, 8 * 64> backing{};
  auto pbuf = ProvidedBufferRing::CreateInMemory(descriptors, backing, 64U, 3U, true);
  if (!pbuf) return false;
  if (pbuf->SharedTail() != 8U) return false;

  // Simulate CQE 1: selects bid 2 with 24 bytes and kCqeBufMore set (incremental consumption not yet done).
  const std::uint32_t flags_more =
      liburing::abi::kCqeBuffer | liburing::abi::kCqeBufMore | (2U << liburing::abi::kCqeBufferShift);
  {
    auto lease1 = pbuf->AcquireLease(24, flags_more);
    if (!lease1 || lease1->Bid() != 2U || lease1->Bytes().size() != 24U || !lease1->HasMore()) return false;
    lease1->Bytes()[0] = std::byte{0x41};
  }
  // Because kCqeBufMore was set, releasing lease1 MUST NOT advance SharedTail()!
  if (pbuf->SharedTail() != 8U) return false;

  // Simulate CQE 2: final chunk on bid 2 with 40 bytes and kCqeBufMore CLEAR.
  const std::uint32_t flags_final = liburing::abi::kCqeBuffer | (2U << liburing::abi::kCqeBufferShift);
  {
    auto lease2 = pbuf->AcquireLease(40, flags_final);
    if (!lease2 || lease2->Bid() != 2U || lease2->Bytes().size() != 40U || lease2->HasMore()) return false;
    if (lease2->Bytes().data() != backing.data() + (2U * 64U) + 24U) return false;
  }
  // Now that the final CQE without kCqeBufMore has released, bid 2 MUST be recycled to the ring tail (tail == 9)!
  return pbuf->SharedTail() == 9U && descriptors[8U & 7U].bid == 2U && descriptors[8U & 7U].len == 64U;
}

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool bundle_recycles_multiple_bids() noexcept {
  alignas(64) std::array<liburing::abi::IoUringBuf, 8> descriptors{};
  alignas(64) std::array<std::byte, 8 * 64> backing{};
  auto pbuf = ProvidedBufferRing::CreateInMemory(descriptors, backing, 64U, 5U, false);
  if (!pbuf || pbuf->SharedTail() != 8U) return false;

  // Bundle starting at bid 1 with total_bytes = 150 and stride = 64 -> ceil(150 / 64) == 3 buffers (bids 1, 2, 3).
  const std::uint32_t bundle_flags = liburing::abi::kCqeBuffer | (1U << liburing::abi::kCqeBufferShift);
  {
    auto bundle = pbuf->AcquireBundleLease(150, bundle_flags);
    if (!bundle || bundle->Bid() != 1U || bundle->BufferCount() != 3U || bundle->TotalBytes() != 150U) return false;
    // Move-construct lease to verify RAII move-only transfer does not double-recycle.
    BufferLease moved = std::move(*bundle);
    if (bundle->IsActive() || !moved.IsActive() || pbuf->SharedTail() != 8U) return false;
  }
  // On destruction of moved, all 3 consumed bids (1, 2, 3) must be recycled to the buffer ring tail (8 + 3 = 11).
  if (pbuf->SharedTail() != 11U) return false;
  if (descriptors[0].bid != 1U || descriptors[1].bid != 2U || descriptors[2].bid != 3U) return false;

  // Out-of-order bundle release test: ring slots 0..3 hold bids 0..3.
  alignas(64) std::array<liburing::abi::IoUringBuf, 4> ooo_desc{};
  alignas(64) std::array<std::byte, 4 * 64> ooo_backing{};
  for (std::size_t i = 0; i < ooo_backing.size(); ++i) {
    ooo_backing[i] = static_cast<std::byte>(i & 0xffU);
  }
  auto ooo_ring = ProvidedBufferRing::CreateInMemory(ooo_desc, ooo_backing, 64U, 1U, false);
  if (!ooo_ring) return false;
  const auto make_flag = [](std::uint32_t bid) noexcept {
    return liburing::abi::kCqeBuffer | (bid << liburing::abi::kCqeBufferShift);
  };
  auto lease_a = ooo_ring->AcquireBundleLease(128, make_flag(0U));
  if (!lease_a || lease_a->BufferId() != 0U || lease_a->BufferIdAt(0U) != 0U || lease_a->BufferIdAt(1U) != 1U) {
    return false;
  }
  if (lease_a->Span().size() != 64U || lease_a->SpanAt(0U).size() != 64U || lease_a->SpanAt(1U).size() != 64U) {
    return false;
  }
  {
    auto lease_b = ooo_ring->AcquireLease(64, make_flag(2U));
    if (!lease_b || lease_b->BufferId() != 2U) return false;
  }
  // lease_b released first -> bid 2 republished at ring slot 4 (index 0).
  // Next bundle C consumes ring slots 3 and 4 -> bids 3 and 2 (total_bytes = 100: 64B in bid 3, 36B in bid 2).
  {
    auto lease_c = ooo_ring->AcquireBundleLease(100, make_flag(3U));
    if (!lease_c || lease_c->BufferCount() != 2U) return false;
    if (lease_c->BufferIdAt(0U) != 3U || lease_c->BufferIdAt(1U) != 2U) return false;
    if (lease_c->SpanAt(0U).data() != ooo_backing.data() + (3U * 64U) || lease_c->SpanAt(0U).size() != 64U) {
      return false;
    }
    if (lease_c->SpanAt(1U).data() != ooo_backing.data() + (2U * 64U) || lease_c->SpanAt(1U).size() != 36U) {
      return false;
    }
  }
  // After lease_c released (while lease_a holding bids 0,1 is still active): slots 5 and 6 must hold bids 3 and 2, NEVER bid 0!
  if (!lease_a->IsActive()) return false;
  if (ooo_desc[5U & 3U].bid != 3U || ooo_desc[6U & 3U].bid != 2U) return false;

  // Out-of-order CQE arrival for single leases across two sockets sharing bgid 1:
  // Socket A selected slot 0 (bid 0), Socket B selected slot 1 (bid 1), but B's CQE arrives before A's CQE.
  alignas(64) std::array<liburing::abi::IoUringBuf, 4> single_ooo_desc{};
  alignas(64) std::array<std::byte, 4 * 64> single_ooo_backing{};
  auto single_ooo_ring = ProvidedBufferRing::CreateInMemory(single_ooo_desc, single_ooo_backing, 64U, 1U, false);
  if (!single_ooo_ring) return false;
  auto single_b = single_ooo_ring->AcquireLease(64, make_flag(1U));
  auto single_a = single_ooo_ring->AcquireLease(64, make_flag(0U));
  if (!single_a || !single_b) return false;
  if (single_a->Bid() != 0U || single_a->BufferIdAt(0U) != 0U) return false;
  if (single_b->Bid() != 1U || single_b->BufferIdAt(0U) != 1U) return false;
  single_a->Release();
  if (single_ooo_desc[0].bid != 0U) return false;
  single_b->Release();
  if (single_ooo_desc[1].bid != 1U) return false;

  // Out-of-order CQE arrival for bundle leases across two operations:
  // Bundle A selected slots 0..1 (bids 0, 1), Bundle B selected slots 2..3 (bids 2, 3), but B's CQE arrives first.
  alignas(64) std::array<liburing::abi::IoUringBuf, 4> bundle_ooo_desc{};
  alignas(64) std::array<std::byte, 4 * 64> bundle_ooo_backing{};
  auto bundle_ooo_ring = ProvidedBufferRing::CreateInMemory(bundle_ooo_desc, bundle_ooo_backing, 64U, 1U, false);
  if (!bundle_ooo_ring) return false;
  auto bundle_b = bundle_ooo_ring->AcquireBundleLease(128, make_flag(2U));
  auto bundle_a = bundle_ooo_ring->AcquireBundleLease(128, make_flag(0U));
  if (!bundle_a || !bundle_b) return false;
  if (bundle_a->BufferIdAt(0U) != 0U || bundle_a->BufferIdAt(1U) != 1U) return false;
  if (bundle_b->BufferIdAt(0U) != 2U || bundle_b->BufferIdAt(1U) != 3U) return false;
  bundle_a->Release();
  if (bundle_ooo_desc[0].bid != 0U || bundle_ooo_desc[1].bid != 1U) return false;
  bundle_b->Release();
  if (bundle_ooo_desc[2].bid != 2U || bundle_ooo_desc[3].bid != 3U) return false;

  // Heavy churn (> 2 * ring_entries recycles on other buffers) while a single lease is held:
  {
    alignas(64) std::array<liburing::abi::IoUringBuf, 4> churn_desc{};
    alignas(64) std::array<std::byte, 4 * 64> churn_backing{};
    auto churn_ring = ProvidedBufferRing::CreateInMemory(churn_desc, churn_backing, 64U, 1U, false);
    if (!churn_ring) return false;
    auto held_single = churn_ring->AcquireLease(64, make_flag(0U));
    if (!held_single) return false;
    std::uint32_t next_slot = 1U;
    for (int round = 0; round < 12; ++round) {
      const std::uint16_t bid = churn_desc[next_slot & 3U].bid;
      auto temp = churn_ring->AcquireLease(64, make_flag(bid));
      if (!temp) return false;
      ++next_slot;
    }
    if (held_single->BufferIdAt(0U) != 0U) return false;
    const std::uint16_t tail_before = churn_ring->SharedTail();
    held_single->Release();
    if (churn_desc[tail_before & 3U].bid != 0U) return false;
  }

  // Heavy churn (> 2 * ring_entries recycles on other buffers) while a bundle lease is held:
  {
    alignas(64) std::array<liburing::abi::IoUringBuf, 4> churn_desc{};
    alignas(64) std::array<std::byte, 4 * 64> churn_backing{};
    auto churn_ring = ProvidedBufferRing::CreateInMemory(churn_desc, churn_backing, 64U, 1U, false);
    if (!churn_ring) return false;
    auto held_bundle = churn_ring->AcquireBundleLease(100, make_flag(0U));
    if (!held_bundle || held_bundle->BufferCount() != 2U) return false;
    std::uint32_t next_slot = 2U;
    for (int round = 0; round < 12; ++round) {
      const std::uint16_t bid = churn_desc[next_slot & 3U].bid;
      auto temp = churn_ring->AcquireLease(64, make_flag(bid));
      if (!temp) return false;
      ++next_slot;
    }
    if (held_bundle->BufferIdAt(0U) != 0U || held_bundle->BufferIdAt(1U) != 1U) return false;
    if (held_bundle->SpanAt(0U).data() != churn_backing.data() || held_bundle->SpanAt(0U).size() != 64U) return false;
    if (held_bundle->SpanAt(1U).data() != churn_backing.data() + 64U || held_bundle->SpanAt(1U).size() != 36U) {
      return false;
    }
    const std::uint16_t tail_before = churn_ring->SharedTail();
    held_bundle->Release();
    if (churn_desc[tail_before & 3U].bid != 0U || churn_desc[(tail_before + 1U) & 3U].bid != 1U) return false;
  }
  return true;
}

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool resource_registry_isolates_bgids() noexcept {
  ResourceRegistry registry(32U, 16U);
  auto bgid0 = registry.AllocateBgid();
  auto bgid1 = registry.AllocateBgid();
  auto bgid2 = registry.AllocateBgid();
  if (!bgid0 || !bgid1 || !bgid2) return false;
  if (*bgid0 == *bgid1 || *bgid1 == *bgid2 || *bgid0 == *bgid2) return false;
  if (!registry.IsBgidAllocated(*bgid1) || !registry.ReleaseBgid(*bgid1) || registry.IsBgidAllocated(*bgid1)) {
    return false;
  }
  auto reused = registry.AllocateBgid();
  if (!reused || *reused != *bgid1) return false;

  auto files_a = registry.AllocateFileRange(8U);
  auto files_b = registry.AllocateFileRange(12U);
  if (!files_a || !files_b || files_a->offset + files_a->count > files_b->offset) return false;
  if (registry.AllocateFileRange(16U).has_value()) return false;

  auto bufs_a = registry.AllocateBufferRange(4U);
  auto bufs_b = registry.AllocateBufferRange(8U);
  if (!bufs_a || !bufs_b || bufs_a->offset + bufs_a->count > bufs_b->offset) return false;
  return !registry.AllocateBufferRange(8U).has_value();
}

[[nodiscard]] bool TestKernelResourceRegistrations() noexcept {
  auto ring = KernelRing::Create(8U, RingMode::kStandard64_16);
  if (!ring) return false;

  // FixedFileTable sparse registration + slot allocation
  FixedFileTable<64> file_table({0U, 8U});
  auto slot0 = file_table.AllocateSlot();
  auto slot1 = file_table.AllocateSlot();
  if (!slot0 || !slot1 || *slot0 != 0U || *slot1 != 1U) return false;
  if (!file_table.RegisterSparseFiles(*ring, 8U)) return false;
  if (!file_table.SetFileAllocRange(*ring, 2U, 6U)) return false;
  if (!file_table.UnregisterFiles(*ring)) return false;
  if (!file_table.ReleaseSlot(*slot0)) return false;

  // FixedBufferTable registration + update + unregister
  FixedBufferTable<64> buf_table;
  alignas(4096) std::array<std::byte, 4096> page{};
  const std::array<IoVec, 1> iovecs{IoVec{.iov_base = page.data(), .iov_len = page.size()}};
  if (!buf_table.RegisterBuffers(*ring, iovecs)) return false;
  if (!buf_table.UnregisterBuffers(*ring)) return false;

  ResourceRegistry real_registry(16U, 16U);
  if (!real_registry.RegisterClock(*ring, 1U)) return false;
  auto real_pers = real_registry.RegisterPersonality(*ring);
  if (!real_pers || !real_registry.UnregisterPersonality(*ring, *real_pers)) return false;

  // Kernel-mapped ProvidedBufferRing
  alignas(64) std::array<std::byte, 8 * 128> pbuf_storage{};
  auto kring_pbuf = ProvidedBufferRing::CreateKernelMapped(*ring, 8U, 128U, 7U, pbuf_storage, true);
  if (!kring_pbuf) return false;
  auto status = kring_pbuf->QueryStatus(*ring);
  if (!status || status->head != 0U) return false;
  if (!kring_pbuf->Unregister(*ring)) return false;

  // ScriptedRing control-plane wrappers (NAPI, ZCRX, Personality, Restrictions, Clock)
  ScriptedRing scripted(8U, 16U);
  ResourceRegistry registry(16U, 16U);
  auto pers = registry.RegisterPersonality(scripted);
  if (!pers || !registry.UnregisterPersonality(scripted, *pers)) return false;
  if (!NapiController::RegisterDynamicNapi(scripted, 50U, true)) return false;
  if (!NapiController::UnregisterNapi(scripted)) return false;

  std::uint32_t rq_head = 0;
  std::uint32_t rq_tail = 0;
  std::array<std::uint64_t, 4> rqes{};
  ZcrxRefillQueue zcrx_rq(&rq_head, &rq_tail, 4U, rqes.data());
  if (!zcrx_rq.EnqueueAreaOffset(2U, 4096U, 2048U) || zcrx_rq.FlushTail() != 1U) return false;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string_view name(argv[1]);
    if (name == "buf_more_blocks_recycle") return buf_more_blocks_recycle() ? 0 : 1;
    if (name == "bundle_recycles_multiple_bids") return bundle_recycles_multiple_bids() ? 0 : 1;
    if (name == "resource_registry_isolates_bgids") return resource_registry_isolates_bgids() ? 0 : 1;
    return 2;
  }
  return buf_more_blocks_recycle() && bundle_recycles_multiple_bids() && resource_registry_isolates_bgids() &&
                 TestKernelResourceRegistrations()
             ? 0
             : 1;
}
