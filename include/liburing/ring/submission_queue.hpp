/** @file @brief Single-issuer SQ cursor, compile-time ChainShape prefix-pad solver, and transactional SqeBatch. */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/core/op_header.hpp>
#include <liburing/core/user_data_token.hpp>
#include <liburing/error/uring_error.hpp>
#include <liburing/ring/ring_geometry.hpp>
#include <span>

namespace liburing::ring {

template <OpKind KindValue, std::uint8_t SqeSlots = 1U>
struct ChainOpSpec {
  static constexpr OpKind kKind = KindValue;
  static constexpr std::uint8_t kSqeSlots = SqeSlots;
};

using OneShot64Op = ChainOpSpec<OpKind::kOneShot, 1U>;
using OneShot128Op = ChainOpSpec<OpKind::kOneShot, 2U>;
using ZeroCopy64Op = ChainOpSpec<OpKind::kZeroCopy, 1U>;
using ZeroCopy128Op = ChainOpSpec<OpKind::kZeroCopy, 2U>;
using Multishot64Op = ChainOpSpec<OpKind::kMultishot, 1U>;
using Multishot128Op = ChainOpSpec<OpKind::kMultishot, 2U>;

inline constexpr std::uint64_t kTokenMemberMask = UserDataToken{}.with<^^TokenSpec::member>(3U).raw();
inline constexpr std::array<std::uint64_t, 4> kTokenMemberBits = {
    UserDataToken{}.with<^^TokenSpec::member>(0U).raw(),
    UserDataToken{}.with<^^TokenSpec::member>(1U).raw(),
    UserDataToken{}.with<^^TokenSpec::member>(2U).raw(),
    UserDataToken{}.with<^^TokenSpec::member>(3U).raw(),
};

[[nodiscard]] constexpr UserDataToken WithChainMember(UserDataToken token, std::size_t member) noexcept {
  return UserDataToken::from_raw((token.raw() & ~kTokenMemberMask) | kTokenMemberBits[member & 3U]);
}

/**
 * Compile-time linked-SQE chain descriptor.
 *
 * Because every 2-slot (128B) member m' occupies 2 contiguous slots, no subsequent member m can start at
 * offset o_{m'} + 1. Therefore shifting the chain head by +1 via a single 64B prefix pad NOP (with
 * kSqeCqeSkipSuccess and kNullToken, strictly before chain head o_0) moves any 2-slot member off the ring
 * wrap boundary ((sq_tail + o_m) & mask) == mask without ever splitting IOSQE_IO_LINK bindings.
 */
template <class... Ops>
struct ChainShape {
  static_assert(sizeof...(Ops) >= 1U && sizeof...(Ops) <= 4U, "ChainShape requires between 1 and 4 chain members");
  static_assert((((Ops::kKind == OpKind::kOneShot) || (Ops::kKind == OpKind::kZeroCopy)) && ...),
                "ChainShape forbids OpKind::kMultishot members in IOSQE_IO_LINK chains");
  static_assert((((Ops::kSqeSlots == 1U) || (Ops::kSqeSlots == 2U)) && ...),
                "ChainShape member kSqeSlots must be 1 (64B) or 2 (128B)");

  static constexpr std::size_t kMemberCount = sizeof...(Ops);
  static constexpr std::array<OpKind, kMemberCount> kMemberKinds = {Ops::kKind...};
  static constexpr std::array<std::uint8_t, kMemberCount> kMemberSlots = {static_cast<std::uint8_t>(Ops::kSqeSlots)...};

  [[nodiscard]] static consteval std::array<std::uint32_t, kMemberCount> ComputeOffsets() noexcept {
    std::array<std::uint32_t, kMemberCount> offsets{};
    std::uint32_t running = 0U;
    for (std::size_t i = 0; i < kMemberCount; ++i) {
      offsets[i] = running;
      running += kMemberSlots[i];
    }
    return offsets;
  }

  [[nodiscard]] static consteval std::uint32_t ComputeTotalSlots() noexcept {
    std::uint32_t sum = 0U;
    for (std::size_t i = 0; i < kMemberCount; ++i) {
      sum += kMemberSlots[i];
    }
    return sum;
  }

  static constexpr std::array<std::uint32_t, kMemberCount> kMemberOffsets = ComputeOffsets();
  static constexpr std::uint32_t kTotalSlots = ComputeTotalSlots();
  static constexpr bool kHas128ByteMember = ((Ops::kSqeSlots == 2U) || ...);

  [[nodiscard]] static constexpr bool NeedsPrefixPad(std::uint32_t sq_tail, std::uint32_t mask) noexcept {
    for (std::size_t m = 0; m < kMemberCount; ++m) {
      if (kMemberSlots[m] == 2U && ((sq_tail + kMemberOffsets[m]) & mask) == mask) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] static constexpr std::uint32_t PrefixPadSlots(std::uint32_t sq_tail, std::uint32_t mask,
                                                              SqRingMode sq_mode = SqRingMode::kSqMixed) noexcept {
    return (sq_mode == SqRingMode::kSqMixed && NeedsPrefixPad(sq_tail, mask)) ? 1U : 0U;
  }

  [[nodiscard]] static constexpr std::uint32_t PrefixPadSlots(std::uint32_t sq_tail, std::uint32_t mask,
                                                              RingMode mode) noexcept {
    return PrefixPadSlots(sq_tail, mask, ToSqRingMode(mode));
  }

  [[nodiscard]] static constexpr std::uint32_t RequiredFreeSlots(std::uint32_t sq_tail, std::uint32_t mask,
                                                                 SqRingMode sq_mode = SqRingMode::kSqMixed) noexcept {
    if (sq_mode == SqRingMode::kSq128) {
      return static_cast<std::uint32_t>(kMemberCount);
    }
    return kTotalSlots + PrefixPadSlots(sq_tail, mask, sq_mode);
  }

  [[nodiscard]] static constexpr std::uint32_t RequiredFreeSlots(std::uint32_t sq_tail, std::uint32_t mask,
                                                                 RingMode mode) noexcept {
    return RequiredFreeSlots(sq_tail, mask, ToSqRingMode(mode));
  }

  [[nodiscard]] static constexpr abi::FlatSqe MakePadNopSqe() noexcept {
    abi::FlatSqe pad{};
    pad.opcode = static_cast<std::uint8_t>(abi::kOpNop);
    pad.flags = static_cast<std::uint8_t>(abi::kSqeCqeSkipSuccess);
    pad.user_data = kNullToken.raw();
    return pad;
  }
};

template <std::size_t MemberCount>
struct ChainReservation {
  bool prefix_padded{};
  std::uint32_t start_tail{};
  std::uint32_t chain_head_tail{};
  std::uint32_t slots_consumed{};
  std::array<abi::FlatSqe*, MemberCount> sqes{};
  std::array<std::uint8_t, MemberCount> member_slots{};

  [[nodiscard]] constexpr abi::FlatSqe* Member64(std::size_t index) const noexcept {
    return index < MemberCount ? sqes[index] : nullptr;
  }

  [[nodiscard]] constexpr abi::FlatSqe* Member128SecondSlot(std::size_t index) const noexcept {
    if (index >= MemberCount || member_slots[index] != 2U || sqes[index] == nullptr) {
      return nullptr;
    }
    return sqes[index] + 1;
  }

  [[nodiscard]] abi::FlatSqe128* Member128(std::size_t index) const noexcept {
    if (index >= MemberCount || member_slots[index] != 2U || sqes[index] == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<abi::FlatSqe128*>(sqes[index]);
  }

  template <std::size_t Index, class Op>
    requires(Index < MemberCount)
  constexpr void Emplace(const Op& op) const noexcept {
    auto* slot = sqes[Index];
    if (slot == nullptr) return;
    const auto saved_link = static_cast<std::uint8_t>(slot->flags & (abi::kSqeIoLink | abi::kSqeIoHardlink));
    const auto saved_token = slot->user_data;
    if constexpr (Op::kSqeSlots == 1) {
      op.Encode(*slot);
    } else {
      op.Encode(*reinterpret_cast<abi::FlatSqe128*>(slot));
    }
    slot->flags = static_cast<std::uint8_t>(slot->flags | saved_link);
    if (slot->user_data == 0) {
      slot->user_data = saved_token;
    } else if (saved_token != 0) {
      slot->user_data = WithChainMember(UserDataToken::from_raw(slot->user_data), Index).raw();
    }
  }
};

class SqCursor final {
  std::uint32_t* head_{};
  std::uint32_t* tail_{};
  std::uint32_t mask_{};
  std::uint32_t ring_entries_{};
  std::uint32_t* flags_{};
  std::uint32_t* dropped_{};
  abi::FlatSqe* sqes_{};
  std::uint32_t local_tail_{};
  std::uint32_t flushed_tail_{};
  std::uint32_t submitted_tail_{};
  SqRingMode sq_mode_{SqRingMode::kSq64};

  [[nodiscard]] static constexpr std::uint32_t LoadAcquire(const std::uint32_t* word) noexcept {
    if (word == nullptr) return 0U;
    if consteval {
      return *word;
    } else {
      return __atomic_load_n(word, __ATOMIC_ACQUIRE);
    }
  }

  static constexpr void StoreRelease(std::uint32_t* word, std::uint32_t value) noexcept {
    if (word == nullptr) return;
    if consteval {
      *word = value;
    } else {
      __atomic_store_n(word, value, __ATOMIC_RELEASE);
    }
  }

  [[nodiscard]] constexpr abi::FlatSqe* PhysicalSlot(std::uint32_t logical_tail) noexcept {
    const std::uint32_t index = logical_tail & mask_;
    const std::uint32_t stride_slots = (sq_mode_ == SqRingMode::kSq128) ? 2U : 1U;
    return &sqes_[index * stride_slots];
  }

  [[nodiscard]] constexpr const abi::FlatSqe* PhysicalSlot(std::uint32_t logical_tail) const noexcept {
    const std::uint32_t index = logical_tail & mask_;
    const std::uint32_t stride_slots = (sq_mode_ == SqRingMode::kSq128) ? 2U : 1U;
    return &sqes_[index * stride_slots];
  }

 public:
  constexpr SqCursor() noexcept = default;

  constexpr SqCursor(std::uint32_t* head, std::uint32_t* tail, std::uint32_t mask, std::uint32_t ring_entries,
                     std::uint32_t* flags, std::uint32_t* dropped, abi::FlatSqe* sqes,
                     SqRingMode sq_mode = SqRingMode::kSq64) noexcept
      : head_(head),
        tail_(tail),
        mask_(mask),
        ring_entries_(ring_entries),
        flags_(flags),
        dropped_(dropped),
        sqes_(sqes),
        local_tail_(tail != nullptr ? *tail : 0U),
        flushed_tail_(local_tail_),
        submitted_tail_(local_tail_),
        sq_mode_(sq_mode) {}

  constexpr SqCursor(std::uint32_t* head, std::uint32_t* tail, std::uint32_t mask, std::uint32_t ring_entries,
                     std::uint32_t* flags, std::uint32_t* dropped, abi::FlatSqe* sqes, RingMode mode) noexcept
      : SqCursor(head, tail, mask, ring_entries, flags, dropped, sqes, ToSqRingMode(mode)) {}

  [[nodiscard]] static SqCursor FromMappedRing(std::span<std::byte> sq_ring, std::span<std::byte> sqes,
                                               const abi::IoUringParams& params) noexcept {
    auto* base = sq_ring.data();
    auto* head = reinterpret_cast<std::uint32_t*>(base + params.sq_off.head);
    auto* tail = reinterpret_cast<std::uint32_t*>(base + params.sq_off.tail);
    const auto mask = *reinterpret_cast<const std::uint32_t*>(base + params.sq_off.ring_mask);
    const auto entries = *reinterpret_cast<const std::uint32_t*>(base + params.sq_off.ring_entries);
    auto* flags = reinterpret_cast<std::uint32_t*>(base + params.sq_off.flags);
    auto* dropped = reinterpret_cast<std::uint32_t*>(base + params.sq_off.dropped);
    if ((params.flags & abi::kSetupNoSqArray) == 0U) {
      auto* sq_array = reinterpret_cast<std::uint32_t*>(base + params.sq_off.array);
      for (std::uint32_t i = 0; i < entries; ++i) {
        sq_array[i] = i;
      }
    }
    return {head,
            tail,
            mask,
            entries,
            flags,
            dropped,
            reinterpret_cast<abi::FlatSqe*>(sqes.data()),
            DetectSqRingMode(params.flags)};
  }

  [[nodiscard]] constexpr SqRingMode SqMode() const noexcept { return sq_mode_; }
  [[nodiscard]] constexpr RingMode Mode() const noexcept { return ToRingMode(sq_mode_); }
  [[nodiscard]] constexpr std::uint32_t Mask() const noexcept { return mask_; }
  [[nodiscard]] constexpr std::uint32_t RingEntries() const noexcept { return ring_entries_; }
  [[nodiscard]] constexpr std::uint32_t Head() const noexcept { return LoadAcquire(head_); }
  [[nodiscard]] constexpr std::uint32_t Tail() const noexcept { return local_tail_; }
  [[nodiscard]] constexpr std::uint32_t FlushedTail() const noexcept { return flushed_tail_; }
  [[nodiscard]] constexpr std::uint32_t SharedTail() const noexcept { return LoadAcquire(tail_); }
  [[nodiscard]] constexpr std::uint32_t UnflushedCount() const noexcept { return local_tail_ - flushed_tail_; }

  [[nodiscard]] constexpr std::uint32_t InFlightCount() const noexcept { return local_tail_ - Head(); }

  [[nodiscard]] constexpr std::uint32_t FreeSlots() const noexcept {
    const std::uint32_t used = local_tail_ - Head();
    return used <= ring_entries_ ? (ring_entries_ - used) : 0U;
  }

  [[nodiscard]] constexpr std::uint32_t Flags() const noexcept { return LoadAcquire(flags_); }
  [[nodiscard]] constexpr bool NeedsWakeup() const noexcept { return (Flags() & abi::kSqNeedWakeup) != 0U; }
  [[nodiscard]] constexpr bool HasCqOverflow() const noexcept { return (Flags() & abi::kSqCqOverflow) != 0U; }
  [[nodiscard]] constexpr bool HasTaskrun() const noexcept { return (Flags() & abi::kSqTaskrun) != 0U; }
  [[nodiscard]] constexpr std::uint32_t Dropped() const noexcept { return LoadAcquire(dropped_); }

  [[nodiscard]] constexpr const abi::FlatSqe& SlotAt(std::uint32_t index) const noexcept { return *PhysicalSlot(index); }

  [[nodiscard]] constexpr abi::FlatSqe& SlotAt(std::uint32_t index) noexcept { return *PhysicalSlot(index); }

  constexpr std::uint32_t FlushTail() noexcept {
    const std::uint32_t newly_flushed = local_tail_ - flushed_tail_;
    StoreRelease(tail_, local_tail_);
    flushed_tail_ = local_tail_;
    return newly_flushed;
  }

  [[nodiscard]] constexpr std::uint32_t PrepareSubmit() noexcept {
    static_cast<void>(FlushTail());
    return flushed_tail_ - submitted_tail_;
  }

  constexpr void SyncSubmittedWithHead() noexcept {
    const auto head = Head();
    if ((head - submitted_tail_) <= (flushed_tail_ - submitted_tail_)) submitted_tail_ = head;
  }

  constexpr void CompleteSubmit(std::uint32_t submitted, bool sqpoll = false) noexcept {
    if (sqpoll) {
      submitted_tail_ = flushed_tail_;
    } else {
      const auto pending = flushed_tail_ - submitted_tail_;
      submitted_tail_ += submitted <= pending ? submitted : pending;
      SyncSubmittedWithHead();
    }
  }

  [[nodiscard]] constexpr std::uint32_t ConsumePendingSubmitCount() noexcept {
    const auto pending = PrepareSubmit();
    CompleteSubmit(pending, true);
    return pending;
  }

  constexpr void RollbackUnflushed(std::uint32_t saved_tail) noexcept {
    if ((saved_tail - flushed_tail_) <= (local_tail_ - flushed_tail_)) {
      local_tail_ = saved_tail;
    }
  }

  [[nodiscard]] constexpr Result<abi::FlatSqe*> TryReserve64() noexcept {
    if (FreeSlots() < 1U) {
      return std::unexpected(UringErrorCode::kSqRingFull);
    }
    auto* slot = PhysicalSlot(local_tail_);
    *slot = abi::FlatSqe{};
    if (sq_mode_ == SqRingMode::kSq128) {
      *(slot + 1) = abi::FlatSqe{};
    }
    ++local_tail_;
    return slot;
  }

  [[nodiscard]] Result<abi::FlatSqe128*> TryReserve128() noexcept {
    if (sq_mode_ == SqRingMode::kSq64) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    if (sq_mode_ == SqRingMode::kSq128) {
      if (FreeSlots() < 1U) {
        return std::unexpected(UringErrorCode::kSqRingFull);
      }
      auto* slot = PhysicalSlot(local_tail_);
      *slot = abi::FlatSqe{};
      *(slot + 1) = abi::FlatSqe{};
      ++local_tail_;
      return reinterpret_cast<abi::FlatSqe128*>(slot);
    }
    // SqRingMode::kSqMixed: a 2-slot SQE128 must not start at (local_tail_ & mask_) == mask_.
    const bool needs_pad = (local_tail_ & mask_) == mask_;
    const std::uint32_t required = needs_pad ? 3U : 2U;
    if (ring_entries_ < 2U) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    if (FreeSlots() < required) {
      return std::unexpected(UringErrorCode::kSqRingFull);
    }
    if (needs_pad) {
      *PhysicalSlot(local_tail_) = ChainShape<OneShot128Op>::MakePadNopSqe();
      ++local_tail_;
    }
    auto* primary = PhysicalSlot(local_tail_);
    *primary = abi::FlatSqe{};
    *(primary + 1) = abi::FlatSqe{};
    local_tail_ += 2U;
    return reinterpret_cast<abi::FlatSqe128*>(primary);
  }

  template <class... Ops>
  [[nodiscard]] constexpr Result<ChainReservation<sizeof...(Ops)>> TryReserveChain(UserDataToken base_token) noexcept {
    using Shape = ChainShape<Ops...>;
    if (sq_mode_ == SqRingMode::kSq64 && Shape::kHas128ByteMember) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    const bool pad = (sq_mode_ == SqRingMode::kSqMixed) && Shape::NeedsPrefixPad(local_tail_, mask_);
    const std::uint32_t required = Shape::RequiredFreeSlots(local_tail_, mask_, sq_mode_);
    if (required > ring_entries_) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    if (sq_mode_ == SqRingMode::kSqMixed && Shape::NeedsPrefixPad(local_tail_ + (pad ? 1U : 0U), mask_)) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    if (FreeSlots() < required) {
      return std::unexpected(UringErrorCode::kSqRingFull);
    }

    ChainReservation<sizeof...(Ops)> result{};
    result.prefix_padded = pad;
    result.start_tail = local_tail_;
    if (pad) {
      *PhysicalSlot(local_tail_) = Shape::MakePadNopSqe();
      ++local_tail_;
    }
    result.chain_head_tail = local_tail_;
    result.slots_consumed = required;
    result.member_slots = Shape::kMemberSlots;

    for (std::size_t m = 0; m < Shape::kMemberCount; ++m) {
      auto* slot = PhysicalSlot(local_tail_);
      *slot = abi::FlatSqe{};
      if (sq_mode_ == SqRingMode::kSq128 || Shape::kMemberSlots[m] == 2U) {
        *(slot + 1) = abi::FlatSqe{};
      }
      slot->user_data = WithChainMember(base_token, m).raw();
      if (m + 1U < Shape::kMemberCount) {
        slot->flags = static_cast<std::uint8_t>(abi::kSqeIoLink);
      }
      result.sqes[m] = slot;
      local_tail_ += (sq_mode_ == SqRingMode::kSq128) ? 1U : static_cast<std::uint32_t>(Shape::kMemberSlots[m]);
    }
    return result;
  }
};

class SqeBatch final {
  SqCursor* cursor_{};
  std::uint32_t saved_tail_{};
  bool committed_{false};

 public:
  constexpr SqeBatch() noexcept = default;
  explicit constexpr SqeBatch(SqCursor& cursor) noexcept : cursor_(&cursor), saved_tail_(cursor.Tail()) {}

  SqeBatch(const SqeBatch&) = delete;
  SqeBatch& operator=(const SqeBatch&) = delete;

  constexpr SqeBatch(SqeBatch&& other) noexcept
      : cursor_(other.cursor_), saved_tail_(other.saved_tail_), committed_(other.committed_) {
    other.cursor_ = nullptr;
    other.committed_ = true;
  }

  constexpr SqeBatch& operator=(SqeBatch&& other) noexcept {
    if (this != &other) {
      Rollback();
      cursor_ = other.cursor_;
      saved_tail_ = other.saved_tail_;
      committed_ = other.committed_;
      other.cursor_ = nullptr;
      other.committed_ = true;
    }
    return *this;
  }

  constexpr ~SqeBatch() noexcept {
    if (!committed_) {
      Rollback();
    }
  }

  [[nodiscard]] static constexpr SqeBatch Begin(SqCursor& cursor) noexcept { return SqeBatch(cursor); }

  [[nodiscard]] constexpr Result<abi::FlatSqe*> Reserve64() noexcept {
    if (cursor_ == nullptr) return std::unexpected(UringErrorCode::kBadRing);
    auto sqe = cursor_->TryReserve64();
    if (!sqe) {
      Rollback();
    }
    return sqe;
  }

  [[nodiscard]] Result<abi::FlatSqe128*> Reserve128() noexcept {
    if (cursor_ == nullptr) return std::unexpected(UringErrorCode::kBadRing);
    auto sqe = cursor_->TryReserve128();
    if (!sqe) {
      Rollback();
    }
    return sqe;
  }

  template <class... Ops>
  [[nodiscard]] constexpr Result<ChainReservation<sizeof...(Ops)>> ReserveChain(UserDataToken base_token) noexcept {
    if (cursor_ == nullptr) return std::unexpected(UringErrorCode::kBadRing);
    auto chain = cursor_->TryReserveChain<Ops...>(base_token);
    if (!chain) {
      Rollback();
    }
    return chain;
  }

  constexpr void Commit() noexcept { committed_ = true; }

  constexpr void Rollback() noexcept {
    if (cursor_ != nullptr && !committed_) {
      cursor_->RollbackUnflushed(saved_tail_);
    }
  }
};

}  // namespace liburing::ring

namespace liburing {
using ring::ChainOpSpec;
using ring::ChainReservation;
using ring::ChainShape;
using ring::Multishot128Op;
using ring::Multishot64Op;
using ring::OneShot128Op;
using ring::OneShot64Op;
using ring::SqCursor;
using ring::SqeBatch;
using ring::ZeroCopy128Op;
using ring::ZeroCopy64Op;
}  // namespace liburing
