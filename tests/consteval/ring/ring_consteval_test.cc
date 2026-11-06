#include <array>
#include <liburing/ring/completion_queue.hpp>
#include <liburing/ring/ring.hpp>
#include <liburing/ring/ring_geometry.hpp>
#include <liburing/ring/submission_queue.hpp>

namespace {

using liburing::Completion;
using liburing::kNullToken;
using liburing::MakeToken;
using liburing::OpKind;
using liburing::UserDataToken;
using liburing::ring::ChainShape;
using liburing::ring::CqCursor;
using liburing::ring::Full128_32;
using liburing::ring::Mixed64_128_16_32;
using liburing::ring::OneShot128Op;
using liburing::ring::OneShot64Op;
using liburing::ring::ReapedCqe;
using liburing::ring::RingMode;
using liburing::ring::SqCursor;
using liburing::ring::SqeBatch;
using liburing::ring::Standard64_16;
using liburing::ring::ZeroCopy128Op;
using liburing::ring::ZeroCopy64Op;

// NOLINTNEXTLINE(readability-identifier-naming)
consteval bool mixed_sqe_at_mask_inserts_pad() noexcept {
  std::uint32_t sq_head = 3;
  std::uint32_t sq_tail = 3;
  std::uint32_t sq_flags = 0;
  std::uint32_t sq_dropped = 0;
  std::array<liburing::abi::FlatSqe, 4> sqes{};
  SqCursor cursor(&sq_head, &sq_tail, 3U, 4U, &sq_flags, &sq_dropped, sqes.data(), RingMode::kMixed64_128_16_32);

  using Single128 = ChainShape<OneShot128Op>;
  static_assert(Single128::kTotalSlots == 2U);
  if (!Single128::NeedsPrefixPad(3U, 3U)) return false;

  constexpr UserDataToken token = MakeToken(1U, 0U, 2U, 9U);
  auto reserved = cursor.TryReserveChain<OneShot128Op>(token);
  if (!reserved || !reserved->prefix_padded || reserved->slots_consumed != 3U) return false;
  if (cursor.Tail() != 6U) return false;

  const auto& pad = sqes[3];
  if (pad.opcode != liburing::abi::kOpNop) return false;
  if (pad.flags != liburing::abi::kSqeCqeSkipSuccess) return false;
  if ((pad.flags & liburing::abi::kSqeIoLink) != 0) return false;
  if (pad.user_data != kNullToken.raw()) return false;

  auto* sqe_primary = reserved->Member64(0);
  auto* sqe_ext = reserved->Member128SecondSlot(0);
  if (sqe_primary == nullptr || sqe_ext == nullptr) return false;
  sqe_primary->opcode = static_cast<std::uint8_t>(liburing::abi::kOpUringCmd128);
  if (sqes[0].opcode != liburing::abi::kOpUringCmd128) return false;
  if (sqes[0].user_data != token.raw()) return false;
  if ((sqes[0].flags & liburing::abi::kSqeIoLink) != 0) return false;
  return true;
}

// NOLINTNEXTLINE(readability-identifier-naming)
consteval bool pad_never_splits_link_chain() noexcept {
  using Chain64Then128Then64 = ChainShape<OneShot64Op, ZeroCopy128Op, OneShot64Op>;
  static_assert(Chain64Then128Then64::kMemberCount == 3U);
  static_assert(Chain64Then128Then64::kMemberOffsets[0] == 0U);
  static_assert(Chain64Then128Then64::kMemberOffsets[1] == 1U);
  static_assert(Chain64Then128Then64::kMemberOffsets[2] == 3U);
  static_assert(Chain64Then128Then64::kTotalSlots == 4U);

  // At sq_tail == 6 with mask == 7, member 0 is at 6 and member 1 (128B) would land at 7 == mask.
  // The pad NOP must be emitted at slot 6 BEFORE member 0 (so member 0 starts at 7, member 1 at 0..1, member 2 at 2).
  if (!Chain64Then128Then64::NeedsPrefixPad(6U, 7U)) return false;
  if (Chain64Then128Then64::NeedsPrefixPad(5U, 7U)) return false;

  std::uint32_t sq_head = 4;
  std::uint32_t sq_tail = 6;
  std::uint32_t sq_flags = 0;
  std::uint32_t sq_dropped = 0;
  std::array<liburing::abi::FlatSqe, 8> sqes{};
  SqCursor cursor(&sq_head, &sq_tail, 7U, 8U, &sq_flags, &sq_dropped, sqes.data(), RingMode::kMixed64_128_16_32);

  constexpr UserDataToken base_token = MakeToken(5U, 0U, 3U, 11U);
  auto chain = cursor.TryReserveChain<OneShot64Op, ZeroCopy128Op, OneShot64Op>(base_token);
  if (!chain || !chain->prefix_padded || chain->slots_consumed != 5U) return false;
  if (chain->chain_head_tail != 7U || cursor.Tail() != 11U) return false;

  // Slot 6 holds the unlinked prefix pad NOP.
  if (sqes[6].opcode != liburing::abi::kOpNop || sqes[6].flags != liburing::abi::kSqeCqeSkipSuccess ||
      sqes[6].user_data != kNullToken.raw()) {
    return false;
  }
  // Member 0 at slot 7 (linked), Member 1 (128B) at slots 0..1 (linked), Member 2 at slot 2 (unlinked tail).
  if (chain->Member64(0) != &sqes[7] || (sqes[7].flags & liburing::abi::kSqeIoLink) == 0) return false;
  if (chain->Member64(1) != &sqes[0] || chain->Member128SecondSlot(1) != &sqes[1] ||
      (sqes[0].flags & liburing::abi::kSqeIoLink) == 0) {
    return false;
  }
  if (chain->Member64(2) != &sqes[2] || (sqes[2].flags & liburing::abi::kSqeIoLink) != 0) return false;
  if (sqes[7].user_data != base_token.with<^^liburing::TokenSpec::member>(0U).raw()) return false;
  if (sqes[0].user_data != base_token.with<^^liburing::TokenSpec::member>(1U).raw()) return false;
  if (sqes[2].user_data != base_token.with<^^liburing::TokenSpec::member>(2U).raw()) return false;
  return true;
}

// NOLINTNEXTLINE(readability-identifier-naming)
consteval bool cq_head_released_before_callback() noexcept {
  std::uint32_t cq_head = 0;
  std::uint32_t cq_tail = 4;
  std::uint32_t cq_overflow = 0;
  std::uint32_t cq_flags = 0;
  std::array<liburing::abi::FlatCqe, 8> cqes{};
  constexpr UserDataToken token_1 = MakeToken(1U, 0U, 1U, 1U);
  constexpr UserDataToken token_2 = MakeToken(2U, 0U, 1U, 1U);
  // Slot 0: skipped via kCqeSkip
  cqes[0] = {.user_data = 0U, .res = 0, .flags = liburing::abi::kCqeSkip};
  // Slots 1..2: 32B extended CQE in mixed mode
  cqes[1] = {.user_data = token_1.raw(), .res = 64, .flags = liburing::abi::kCqe32};
  cqes[2] = {.user_data = 0x1122334455667788ULL, .res = static_cast<std::int32_t>(0x99aabbccU), .flags = 0xddeeff00U};
  // Slot 3: normal 16B CQE
  cqes[3] = {.user_data = token_2.raw(), .res = 16, .flags = 0U};

  CqCursor cursor(&cq_head, &cq_tail, 7U, 8U, &cq_overflow, &cq_flags, cqes.data(), RingMode::kMixed64_128_16_32);
  std::array<ReapedCqe, 4> batch{};
  bool head_was_released_before_cb = false;
  const auto count = cursor.ReapAndDispatch(batch, [&](std::span<const ReapedCqe> reaped) {
    head_was_released_before_cb = (cq_head == 4U) && (reaped.size() == 2U);
  });
  if (count != 2U || !head_was_released_before_cb) return false;
  if (batch[0].completion.token != token_1 || !batch[0].HasExtra() || batch[0].extra[0] != 0x1122334455667788ULL) {
    return false;
  }
  return batch[1].completion.token == token_2 && !batch[1].HasExtra() && batch[1].completion.result == 16;
}

consteval bool VerifyBatchRollback() noexcept {
  std::uint32_t sq_head = 0;
  std::uint32_t sq_tail = 2;
  std::uint32_t sq_flags = 0;
  std::uint32_t sq_dropped = 0;
  std::array<liburing::abi::FlatSqe, 4> sqes{};
  SqCursor cursor(&sq_head, &sq_tail, 3U, 4U, &sq_flags, &sq_dropped, sqes.data(), RingMode::kStandard64_16);
  {
    auto batch = SqeBatch::Begin(cursor);
    auto first = batch.Reserve64();
    auto second = batch.Reserve64();
    auto third = batch.Reserve64();
    if (!first || !second || third.has_value()) return false;
    // Let batch go out of scope without Commit() -> must roll back cursor.Tail() to 2.
  }
  if (cursor.Tail() != 2U || cursor.FreeSlots() != 2U) return false;

  // Modular wrap-around rollback across UINT32_MAX -> 0 -> 1.
  std::uint32_t wrap_head = 0xFFFFFFFEU;
  std::uint32_t wrap_tail = 0xFFFFFFFEU;
  std::array<liburing::abi::FlatSqe, 8> wrap_sqes{};
  SqCursor wrap_sq(&wrap_head, &wrap_tail, 7U, 8U, &sq_flags, &sq_dropped, wrap_sqes.data(), RingMode::kStandard64_16);
  if (!wrap_sq.TryReserve64() || !wrap_sq.TryReserve64()) return false;
  if (wrap_sq.Tail() != 0U || wrap_sq.FlushedTail() != 0xFFFFFFFEU) return false;
  {
    auto batch = SqeBatch::Begin(wrap_sq);
    auto ghost = batch.Reserve64();
    if (!ghost || wrap_sq.Tail() != 1U) return false;
    (*ghost)->opcode = 26U;
  }
  return wrap_sq.Tail() == 0U;
}

static_assert(sizeof(ReapedCqe) == 32U && uarch::NoPadding<ReapedCqe>);
static_assert(Standard64_16::kSqeSlotBytes == 64U && Standard64_16::kCqeSlotBytes == 16U);
static_assert(Full128_32::kSqeSlotBytes == 128U && Full128_32::kCqeSlotBytes == 32U);
static_assert(Mixed64_128_16_32::kIsMixedSqe && Mixed64_128_16_32::kIsMixedCqe);
static_assert(mixed_sqe_at_mask_inserts_pad());
static_assert(pad_never_splits_link_chain());
static_assert(cq_head_released_before_callback());
static_assert(VerifyBatchRollback());

}  // namespace

int main() { return 0; }
