#include <array>
#include <cstring>
#include <liburing/ring/completion_queue.hpp>
#include <liburing/ring/ring.hpp>
#include <liburing/ring/ring_geometry.hpp>
#include <liburing/ring/submission_queue.hpp>
#include <string_view>

#include "../../support/host_kernel.hpp"

namespace {

using liburing::kNullToken;
using liburing::MakeToken;
using liburing::UserDataToken;
using liburing::ring::ChainShape;
using liburing::ring::KernelRing;
using liburing::ring::OneShot128Op;
using liburing::ring::OneShot64Op;
using liburing::ring::ReapedCqe;
using liburing::ring::RingMode;
using liburing::ring::ScriptedRing;
using liburing::ring::SqeBatch;
using liburing::ring::ZeroCopy128Op;
using liburing::ring::ZeroCopy64Op;

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool mixed_sqe_at_mask_inserts_pad() noexcept {
  ScriptedRing ring(4U, 8U, RingMode::kMixed64_128_16_32);
  for (std::uint32_t i = 0; i < 3U; ++i) {
    auto sqe = ring.Sq().TryReserve64();
    if (!sqe) return false;
    (*sqe)->opcode = static_cast<std::uint8_t>(liburing::abi::kOpNop);
    (*sqe)->user_data = MakeToken(i + 1U, 0U, 1U, 1U).raw();
  }
  if (!ring.Submit()) return false;
  std::array<ReapedCqe, 4> drained{};
  if (ring.Cq().ReapBatch(drained) != 3U) return false;

  // Now sq_tail == 3 == mask (for 4-entry ring). Reserving a 128B SQE must insert a 64B pad NOP at index 3.
  constexpr UserDataToken token_128 = MakeToken(9U, 0U, 2U, 1U);
  auto chain = ring.Sq().TryReserveChain<OneShot128Op>(token_128);
  if (!chain || !chain->prefix_padded || chain->slots_consumed != 3U) return false;
  auto* sqe128 = chain->Member128(0);
  if (sqe128 == nullptr) return false;
  sqe128->sqe.opcode = static_cast<std::uint8_t>(liburing::abi::kOpNop128);
  sqe128->extension[0] = 0xdeadbeefcafe1234ULL;

  // Verify the pad at slot 3 has kOpNop, kSqeCqeSkipSuccess, kNullToken, and no kSqeIoLink.
  const auto& pad = ring.Sq().SlotAt(3U);
  if (pad.opcode != liburing::abi::kOpNop || pad.flags != liburing::abi::kSqeCqeSkipSuccess ||
      pad.user_data != kNullToken.raw()) {
    return false;
  }
  const auto& primary = ring.Sq().SlotAt(0U);
  if (primary.opcode != liburing::abi::kOpNop128 || primary.user_data != token_128.raw()) return false;

  auto submitted = ring.Submit();
  if (!submitted || *submitted != 3U) return false;
  const auto count = ring.Cq().ReapBatch(drained);
  return count == 1U && drained[0].completion.token == token_128;
}

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool pad_never_splits_link_chain() noexcept {
  // Check across all starting sq_tail positions 0..7 on an 8-entry mixed ring that a 3-member chain
  // (64B -> 128B -> 64B) never places a pad NOP between linked members.
  for (std::uint32_t initial_tail = 0; initial_tail < 8U; ++initial_tail) {
    ScriptedRing ring(8U, 16U, RingMode::kMixed64_128_16_32);
    for (std::uint32_t i = 0; i < initial_tail; ++i) {
      auto sqe = ring.Sq().TryReserve64();
      if (!sqe) return false;
      (*sqe)->opcode = static_cast<std::uint8_t>(liburing::abi::kOpNop);
      (*sqe)->flags = static_cast<std::uint8_t>(liburing::abi::kSqeCqeSkipSuccess);
      (*sqe)->user_data = kNullToken.raw();
    }
    if (!ring.Submit()) return false;

    constexpr UserDataToken chain_token = MakeToken(3U, 0U, 4U, 2U);
    auto reserved = ring.Sq().TryReserveChain<OneShot64Op, ZeroCopy128Op, OneShot64Op>(chain_token);
    if (!reserved) return false;

    const bool expected_pad = ((initial_tail + 1U) & 7U) == 7U;
    if (reserved->prefix_padded != expected_pad) return false;

    const std::uint32_t head_tail = initial_tail + (expected_pad ? 1U : 0U);
    if (reserved->chain_head_tail != head_tail) return false;
    if (expected_pad) {
      const auto& pad = ring.Sq().SlotAt(initial_tail & 7U);
      if (pad.opcode != liburing::abi::kOpNop || pad.flags != liburing::abi::kSqeCqeSkipSuccess ||
          pad.user_data != kNullToken.raw() || (pad.flags & liburing::abi::kSqeIoLink) != 0) {
        return false;
      }
    }
    // Member 0 (1 slot) is at head_tail; Member 1 (2 slots) is immediately at head_tail + 1; Member 2 is at head_tail + 3.
    // Neither Member 1 nor any other 2-slot member starts at mask (7).
    if (((head_tail + 1U) & 7U) == 7U) return false;
    const auto& m0 = ring.Sq().SlotAt(head_tail & 7U);
    const auto& m1 = ring.Sq().SlotAt((head_tail + 1U) & 7U);
    const auto& m2 = ring.Sq().SlotAt((head_tail + 3U) & 7U);
    if ((m0.flags & liburing::abi::kSqeIoLink) == 0) return false;
    if ((m1.flags & liburing::abi::kSqeIoLink) == 0) return false;
    if ((m2.flags & liburing::abi::kSqeIoLink) != 0) return false;
  }
  return true;
}

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool cq_head_released_before_callback() noexcept {
  ScriptedRing ring(8U, 16U, RingMode::kMixed64_128_16_32);
  constexpr UserDataToken token_a = MakeToken(1U, 0U, 1U, 1U);
  constexpr UserDataToken token_b = MakeToken(2U, 0U, 1U, 1U);

  ring.ScriptSkipCqe();
  ring.ScriptCqe32(token_a, 128, 0U, 0xaaaaULL, 0xbbbbULL);
  ring.ScriptCqe(token_b, 64, 0U);

  std::array<ReapedCqe, 4> out{};
  bool released_early = false;
  const auto reaped = ring.Cq().ReapAndDispatch(out, [&](std::span<const ReapedCqe> batch) {
    // 1 slot for skip + 2 slots for CQE32 + 1 slot for standard CQE = 4 slots released before callback runs.
    released_early = (ring.Cq().SharedHead() == 4U) && (ring.Cq().Available() == 0U) && (batch.size() == 2U);
  });
  if (reaped != 2U || !released_early) return false;
  if (!out[0].HasExtra() || out[0].extra[0] != 0xaaaaULL || out[0].extra[1] != 0xbbbbULL) return false;
  return out[1].completion.token == token_b && out[1].completion.result == 64;
}

[[nodiscard]] bool TestModularWrapRollback() noexcept {
  std::uint32_t head = 0xFFFFFFFEU;
  std::uint32_t tail = 0xFFFFFFFEU;
  std::uint32_t flags = 0U;
  std::uint32_t dropped = 0U;
  std::array<liburing::abi::FlatSqe, 8> sqes{};
  liburing::ring::SqCursor sq(&head, &tail, 7U, 8U, &flags, &dropped, sqes.data());
  if (!sq.TryReserve64() || !sq.TryReserve64()) return false;
  if (sq.Tail() != 0U || sq.FlushedTail() != 0xFFFFFFFEU) return false;
  {
    auto batch = SqeBatch::Begin(sq);
    auto s = batch.Reserve64();
    if (!s) return false;
    (*s)->opcode = 26U;
  }
  return sq.Tail() == 0U;
}

[[nodiscard]] bool TestAsymmetricSqCqGeometries() noexcept {
  // 1. CQE32-only ring (64B SQEs + 32B CQEs): SQ cursor must use 64B stride across all 8 slots.
  {
    auto ring = KernelRing::Create(8U, RingMode::kStandard64_16, liburing::abi::kSetupCqe32);
    if (!ring) return false;
    if (ring->SqMode() != liburing::ring::SqRingMode::kSq64 || ring->CqMode() != liburing::ring::CqRingMode::kCq32) {
      return false;
    }
    const auto sq_entries = ring->Params().sq_entries;
    const auto* base = &ring->Sq().SlotAt(0U);
    for (std::uint32_t i = 0; i < sq_entries; ++i) {
      const auto* p = &ring->Sq().SlotAt(i);
      const auto end_bytes =
          static_cast<std::size_t>(reinterpret_cast<const std::byte*>(p + 1) - reinterpret_cast<const std::byte*>(base));
      if (end_bytes > static_cast<std::size_t>(sq_entries) * 64U) return false;
      auto reserved = ring->Sq().TryReserve64();
      if (!reserved) return false;
      (*reserved)->opcode = static_cast<std::uint8_t>(liburing::abi::kOpNop);
      (*reserved)->user_data = MakeToken(i + 1U, 0U, 1U, 1U).raw();
    }
    if (!ring->SubmitAndWait(sq_entries)) return false;
    std::array<ReapedCqe, 8> cqes{};
    const auto reaped = ring->Cq().ReapBatch(cqes);
    if (reaped != sq_entries || !cqes[0].HasExtra()) return false;
  }

  // 2. SQE128-only ring (128B SQEs + 16B CQEs): CQ cursor must use 16B stride.
  {
    auto ring = KernelRing::Create(8U, RingMode::kStandard64_16, liburing::abi::kSetupSqe128);
    if (!ring) return false;
    if (ring->SqMode() != liburing::ring::SqRingMode::kSq128 || ring->CqMode() != liburing::ring::CqRingMode::kCq16) {
      return false;
    }
    for (std::uint32_t i = 0; i < 4U; ++i) {
      auto sqe128 = ring->Sq().TryReserve128();
      if (!sqe128) return false;
      (*sqe128)->sqe.opcode = static_cast<std::uint8_t>(liburing::abi::kOpNop);
      (*sqe128)->sqe.user_data = MakeToken(i + 1U, 0U, 2U, 1U).raw();
    }
    if (!ring->SubmitAndWait(4U)) return false;
    std::array<ReapedCqe, 4> cqes{};
    const auto reaped = ring->Cq().ReapBatch(cqes);
    if (reaped != 4U || cqes[0].HasExtra()) return false;
  }
  return true;
}

[[nodiscard]] bool TestRealKernelRings() noexcept {
  // 1. Standard 64/16 single-issuer kernel ring
  {
    auto ring = KernelRing::Create(8U, RingMode::kStandard64_16, 0U, true);
    if (!ring || !ring->HasRegisteredFd()) return false;
    constexpr UserDataToken nop_token = MakeToken(1U, 0U, 1U, 1U);
    auto sqe = ring->Sq().TryReserve64();
    if (!sqe) return false;
    (*sqe)->opcode = static_cast<std::uint8_t>(liburing::abi::kOpNop);
    (*sqe)->user_data = nop_token.raw();
    auto submitted = ring->SubmitAndWait(1U);
    if (!submitted || *submitted != 1U) return false;
    std::array<ReapedCqe, 4> cqes{};
    const auto n = ring->Cq().ReapBatch(cqes);
    if (n != 1U || cqes[0].completion.token != nop_token || cqes[0].completion.result != 0) return false;
  }

  // 2. Mixed 64/128 SQE & 16/32 CQE kernel ring with wrap-boundary pad NOP + NOP128
  if (const int gate = liburing::test_support::SkipBelowKernel(6, 16); gate > 0)
    return false;
  else if (gate < 0) {
    auto ring = KernelRing::Create(4U, RingMode::kMixed64_128_16_32);
    if (!ring) return false;
    for (std::uint32_t i = 0; i < 3U; ++i) {
      auto sqe = ring->Sq().TryReserve64();
      if (!sqe) return false;
      (*sqe)->opcode = static_cast<std::uint8_t>(liburing::abi::kOpNop);
      (*sqe)->user_data = MakeToken(i + 1U, 0U, 1U, 1U).raw();
    }
    if (!ring->SubmitAndWait(3U)) return false;
    std::array<ReapedCqe, 4> cqes{};
    if (ring->Cq().ReapBatch(cqes) != 3U) return false;

    // Now at index 3 == mask: reserve a 128B NOP128 with kNopCqe32 so kernel emits a 32B CQE while skipping the pad NOP!
    constexpr UserDataToken mixed_token = MakeToken(4U, 0U, 2U, 7U);
    auto chain = ring->Sq().TryReserveChain<OneShot128Op>(mixed_token);
    if (!chain || !chain->prefix_padded) return false;
    auto* sqe128 = chain->Member128(0);
    sqe128->sqe.opcode = static_cast<std::uint8_t>(liburing::abi::kOpNop128);
    sqe128->sqe.nop_flags() = liburing::abi::kNopCqe32 | liburing::abi::kNopInjectResult;
    sqe128->sqe.len = 77U;
    sqe128->sqe.addr() = 0x123456789abcdef0ULL;
    sqe128->sqe.off() = 0x0fedcba987654321ULL;

    auto submitted = ring->SubmitAndWait(1U);
    if (!submitted || *submitted != 3U) return false;
    const auto n = ring->Cq().ReapBatch(cqes);
    if (n != 1U || cqes[0].completion.token != mixed_token || cqes[0].completion.result != 77) return false;
    if (!cqes[0].HasExtra()) return false;
  }

  // 3. NoMmap kernel ring
  {
    auto ring = KernelRing::CreateNoMmap(8U, RingMode::kStandard64_16);
    if (!ring) return false;
    constexpr UserDataToken no_mmap_token = MakeToken(2U, 0U, 3U, 1U);
    auto sqe = ring->Sq().TryReserve64();
    if (!sqe) return false;
    (*sqe)->opcode = static_cast<std::uint8_t>(liburing::abi::kOpNop);
    (*sqe)->user_data = no_mmap_token.raw();
    if (!ring->SubmitAndWait(1U)) return false;
    std::array<ReapedCqe, 4> cqes{};
    if (ring->Cq().ReapBatch(cqes) != 1U || cqes[0].completion.token != no_mmap_token) return false;
  }
  return true;
}

[[nodiscard]] constexpr bool TestScriptedResize() noexcept {
  for (auto mode : std::array{RingMode::kStandard64_16, RingMode::kFull128_32, RingMode::kMixed64_128_16_32}) {
    ScriptedRing ring(4U, 8U, mode);
    if (!ring.ResizeRings(16U) || ring.Params().sq_entries != 16U || ring.Params().cq_entries != 32U ||
        ring.Sq().Mask() != 15U || ring.Cq().Mask() != 31U || ring.Mode() != mode)
      return false;
    for (auto entries : std::array{0U, 3U, 128U, 0x80000000U}) {
      const auto result = ring.ResizeRings(entries);
      if (result || result.error() != liburing::UringErrorCode::kInvalid) return false;
    }
    for (auto entries : std::array{3U, 128U}) {
      const auto result = ring.ResizeRings(8U, entries);
      if (result || result.error() != liburing::UringErrorCode::kInvalid) return false;
    }
    if (ring.ResizeRings(64U) || ring.Params().sq_entries != 16U || ring.Params().cq_entries != 32U) return false;
    auto sqe = ring.Sq().TryReserve64();
    if (!sqe) return false;
    (*sqe)->opcode = static_cast<std::uint8_t>(liburing::abi::kOpNop);
    const auto unflushed = ring.ResizeRings(2U, 4U);
    if (unflushed || unflushed.error() != liburing::UringErrorCode::kBusy || ring.Sq().UnflushedCount() != 1U) return false;
    if (!ring.Submit()) return false;
    const auto pending_cqe = ring.ResizeRings(2U, 4U);
    if (pending_cqe || pending_cqe.error() != liburing::UringErrorCode::kBusy) return false;
    std::array<ReapedCqe, 2> completions{};
    if (ring.Cq().ReapBatch(completions) != 1U || !ring.ResizeRings(2U, 4U) || ring.Sq().Tail() != 0U ||
        ring.Cq().SharedHead() != 0U || ring.Sq().Mask() != 1U || ring.Cq().Mask() != 3U)
      return false;
    ring.SetAutoCompleteNops(false);
    ring.SetConsumeSqOnSubmit(false);
    if (!ring.Sq().TryReserve64() || !ring.Submit()) return false;
    const auto pending_sqe = ring.ResizeRings(4U, 8U);
    if (pending_sqe || pending_sqe.error() != liburing::UringErrorCode::kBusy) return false;
    ring.AdvanceSqHead(1U);
    if (!ring.ResizeRings(64U, 64U) || ring.Params().sq_entries != 64U || ring.Params().cq_entries != 64U ||
        ring.Sq().Tail() != 0U || ring.Cq().SharedHead() != 0U)
      return false;
  }
  return true;
}

[[nodiscard]] bool TestKernelResizeGuard() noexcept {
  auto ring = KernelRing::Create(8U);
  if (!ring) return false;
  const auto result = ring->ResizeRings(16U);
  return !result && result.error() == liburing::UringErrorCode::kNotSupported && ring->Params().sq_entries == 8U;
}

[[nodiscard]] constexpr bool TestPartialSubmitAccounting() noexcept {
  for (auto base : std::array{0U, 0xfffffffeU}) {
    std::uint32_t head = base;
    std::uint32_t tail = base;
    std::uint32_t flags = 0U;
    std::uint32_t dropped = 0U;
    std::array<liburing::abi::FlatSqe, 8> sqes{};
    liburing::ring::SqCursor cursor(&head, &tail, 7U, 8U, &flags, &dropped, sqes.data());
    for (unsigned i = 0; i < 4U; ++i)
      if (!cursor.TryReserve64()) return false;
    if (cursor.PrepareSubmit() != 4U || cursor.PrepareSubmit() != 4U || tail != base + 4U) return false;
    cursor.CompleteSubmit(1U);
    if (cursor.PrepareSubmit() != 3U) return false;
    cursor.CompleteSubmit(0U);
    if (cursor.PrepareSubmit() != 3U) return false;
    head = base + 2U;
    cursor.SyncSubmittedWithHead();
    if (cursor.PrepareSubmit() != 2U) return false;
    head = base + 3U;
    cursor.CompleteSubmit(0U);
    if (cursor.PrepareSubmit() != 1U) return false;
    cursor.CompleteSubmit(99U);
    cursor.SyncSubmittedWithHead();
    if (cursor.PrepareSubmit() != 0U) return false;
    head = base + 5U;
    cursor.SyncSubmittedWithHead();
    if (cursor.PrepareSubmit() != 0U) return false;
    head = base + 4U;
    if (!cursor.TryReserve64() || !cursor.TryReserve64() || cursor.ConsumePendingSubmitCount() != 2U ||
        cursor.PrepareSubmit() != 0U)
      return false;
    if (!cursor.TryReserve64() || cursor.PrepareSubmit() != 1U) return false;
    cursor.CompleteSubmit(0U, true);
    if (cursor.PrepareSubmit() != 0U) return false;
  }
  return true;
}
static_assert(TestPartialSubmitAccounting());

[[nodiscard]] bool TestKernelSubmitRetry() noexcept {
  auto ring = KernelRing::Create(8U);
  if (!ring) return false;
  auto sqe = ring->Sq().TryReserve64();
  if (!sqe) return false;
  (*sqe)->opcode = static_cast<std::uint8_t>(liburing::abi::kOpNop);
  (*sqe)->user_data = 42U;
  const auto invalid = ring->SubmitAndWait(0U, 0x80000000U);
  if (invalid || invalid.error() != liburing::UringErrorCode::kInvalid) return false;
  const auto retried = ring->Submit();
  return retried && *retried == 1U && ring->Sq().Head() == 1U && ring->Cq().Available() == 1U;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string_view name(argv[1]);
    if (name == "mixed_sqe_at_mask_inserts_pad") return mixed_sqe_at_mask_inserts_pad() ? 0 : 1;
    if (name == "pad_never_splits_link_chain") return pad_never_splits_link_chain() ? 0 : 1;
    if (name == "cq_head_released_before_callback") return cq_head_released_before_callback() ? 0 : 1;
    return 2;
  }
  return mixed_sqe_at_mask_inserts_pad() && pad_never_splits_link_chain() && cq_head_released_before_callback() &&
                 TestPartialSubmitAccounting() && TestKernelSubmitRetry() && TestScriptedResize() && TestKernelResizeGuard() &&
                 TestModularWrapRollback() && TestAsymmetricSqCqGeometries() && TestRealKernelRings()
             ? 0
             : 1;
}
