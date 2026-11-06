/** @file @brief Bounded copy-out CQ cursor with immediate release of CQ head and F_SKIP/F_32 support. */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/core/completion.hpp>
#include <liburing/ring/ring_geometry.hpp>
#include <span>
#include <uarch/uarch.hpp>

namespace liburing::ring {

struct alignas(16) ReapedCqe {
  Completion completion{};
  std::array<std::uint64_t, 2> extra{};

  [[nodiscard]] constexpr bool HasExtra() const noexcept { return (completion.flags & abi::kCqe32) != 0U; }

  [[nodiscard]] constexpr CqeExtended ToExtended() const noexcept {
    return CqeExtended{.completion = completion, .extra = {extra[0], extra[1]}};
  }
};
static_assert(sizeof(ReapedCqe) == 32U && uarch::NoPadding<ReapedCqe>);

class CqCursor final {
  std::uint32_t* head_{};
  std::uint32_t* tail_{};
  std::uint32_t mask_{};
  std::uint32_t ring_entries_{};
  std::uint32_t* overflow_{};
  std::uint32_t* flags_{};
  const abi::FlatCqe* cqes_{};
  CqRingMode cq_mode_{CqRingMode::kCq16};

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

  [[nodiscard]] static constexpr std::array<std::uint64_t, 2> UnpackExtension(const abi::FlatCqe& ext_slot) noexcept {
    const std::uint64_t word0 = ext_slot.user_data;
    const std::uint64_t low = static_cast<std::uint32_t>(ext_slot.res);
    const std::uint64_t high = static_cast<std::uint64_t>(ext_slot.flags) << 32U;
    return {word0, low | high};
  }

 public:
  constexpr CqCursor() noexcept = default;

  constexpr CqCursor(std::uint32_t* head, std::uint32_t* tail, std::uint32_t mask, std::uint32_t ring_entries,
                     std::uint32_t* overflow, std::uint32_t* flags, const abi::FlatCqe* cqes,
                     CqRingMode cq_mode = CqRingMode::kCq16) noexcept
      : head_(head),
        tail_(tail),
        mask_(mask),
        ring_entries_(ring_entries),
        overflow_(overflow),
        flags_(flags),
        cqes_(cqes),
        cq_mode_(cq_mode) {}

  constexpr CqCursor(std::uint32_t* head, std::uint32_t* tail, std::uint32_t mask, std::uint32_t ring_entries,
                     std::uint32_t* overflow, std::uint32_t* flags, const abi::FlatCqe* cqes, RingMode mode) noexcept
      : CqCursor(head, tail, mask, ring_entries, overflow, flags, cqes, ToCqRingMode(mode)) {}

  [[nodiscard]] static CqCursor FromMappedRing(std::span<std::byte> cq_ring, const abi::IoUringParams& params) noexcept {
    auto* base = cq_ring.data();
    auto* head = reinterpret_cast<std::uint32_t*>(base + params.cq_off.head);
    auto* tail = reinterpret_cast<std::uint32_t*>(base + params.cq_off.tail);
    const auto mask = *reinterpret_cast<const std::uint32_t*>(base + params.cq_off.ring_mask);
    const auto entries = *reinterpret_cast<const std::uint32_t*>(base + params.cq_off.ring_entries);
    auto* overflow = reinterpret_cast<std::uint32_t*>(base + params.cq_off.overflow);
    auto* flags = params.cq_off.flags != 0U ? reinterpret_cast<std::uint32_t*>(base + params.cq_off.flags) : nullptr;
    const auto* cqes = reinterpret_cast<const abi::FlatCqe*>(base + params.cq_off.cqes);
    return {head, tail, mask, entries, overflow, flags, cqes, DetectCqRingMode(params.flags)};
  }

  [[nodiscard]] constexpr CqRingMode CqMode() const noexcept { return cq_mode_; }
  [[nodiscard]] constexpr RingMode Mode() const noexcept { return ToRingMode(cq_mode_); }
  [[nodiscard]] constexpr std::uint32_t Mask() const noexcept { return mask_; }
  [[nodiscard]] constexpr std::uint32_t RingEntries() const noexcept { return ring_entries_; }
  [[nodiscard]] constexpr std::uint32_t SharedHead() const noexcept { return LoadAcquire(head_); }
  [[nodiscard]] constexpr std::uint32_t Tail() const noexcept { return LoadAcquire(tail_); }
  [[nodiscard]] constexpr std::uint32_t Available() const noexcept { return Tail() - SharedHead(); }
  [[nodiscard]] constexpr std::uint32_t OverflowCount() const noexcept { return LoadAcquire(overflow_); }
  [[nodiscard]] constexpr std::uint32_t Flags() const noexcept { return LoadAcquire(flags_); }

  // Copies up to out.size() CQEs out of the ring and immediately release-stores CQ head before returning.
  [[nodiscard]] constexpr std::size_t ReapBatch(std::span<ReapedCqe> out) noexcept {
    if (head_ == nullptr || tail_ == nullptr || cqes_ == nullptr) return 0U;
    const std::uint32_t initial_head = *head_;
    std::uint32_t head = initial_head;
    const std::uint32_t tail = LoadAcquire(tail_);
    std::size_t count = 0U;

    while (head != tail && count < out.size()) {
      const std::uint32_t index = head & mask_;
      if (cq_mode_ == CqRingMode::kCq32) {
        const abi::FlatCqe& raw = cqes_[index * 2U];
        const abi::FlatCqe& ext = cqes_[index * 2U + 1U];
        head += 1U;
        if ((raw.flags & abi::kCqeSkip) != 0U) {
          continue;
        }
        out[count] = ReapedCqe{
            .completion =
                Completion{
                    .token = UserDataToken::from_raw(raw.user_data), .result = raw.res, .flags = raw.flags | abi::kCqe32},
            .extra = UnpackExtension(ext),
        };
        ++count;
      } else if (cq_mode_ == CqRingMode::kCqMixed) {
        const abi::FlatCqe& raw = cqes_[index];
        const bool is_32 = (raw.flags & abi::kCqe32) != 0U;
        const std::uint32_t step = is_32 ? 2U : 1U;
        if (tail - head < step) {
          break;
        }
        const std::array<std::uint64_t, 2> extra =
            is_32 ? UnpackExtension(cqes_[(index + 1U) & mask_]) : std::array<std::uint64_t, 2>{0ULL, 0ULL};
        head += step;
        if ((raw.flags & abi::kCqeSkip) != 0U) {
          continue;
        }
        out[count] = ReapedCqe{
            .completion = Completion{.token = UserDataToken::from_raw(raw.user_data), .result = raw.res, .flags = raw.flags},
            .extra = extra,
        };
        ++count;
      } else {
        const abi::FlatCqe& raw = cqes_[index];
        head += 1U;
        if ((raw.flags & abi::kCqeSkip) != 0U) {
          continue;
        }
        out[count] = ReapedCqe{
            .completion = Completion{.token = UserDataToken::from_raw(raw.user_data), .result = raw.res, .flags = raw.flags},
            .extra = {0ULL, 0ULL},
        };
        ++count;
      }
    }

    if (head != initial_head) {
      StoreRelease(head_, head);
    }
    return count;
  }

  [[nodiscard]] constexpr std::size_t ReapBatch(std::span<Completion> out) noexcept {
    if (head_ == nullptr || tail_ == nullptr || cqes_ == nullptr) return 0U;
    const std::uint32_t initial_head = *head_;
    std::uint32_t head = initial_head;
    const std::uint32_t tail = LoadAcquire(tail_);
    std::size_t count = 0U;

    while (head != tail && count < out.size()) {
      const std::uint32_t index = head & mask_;
      if (cq_mode_ == CqRingMode::kCq32) {
        const abi::FlatCqe& raw = cqes_[index * 2U];
        head += 1U;
        if ((raw.flags & abi::kCqeSkip) != 0U) continue;
        out[count++] =
            Completion{.token = UserDataToken::from_raw(raw.user_data), .result = raw.res, .flags = raw.flags | abi::kCqe32};
      } else if (cq_mode_ == CqRingMode::kCqMixed) {
        const abi::FlatCqe& raw = cqes_[index];
        const std::uint32_t step = ((raw.flags & abi::kCqe32) != 0U) ? 2U : 1U;
        if (tail - head < step) break;
        head += step;
        if ((raw.flags & abi::kCqeSkip) != 0U) continue;
        out[count++] = Completion{.token = UserDataToken::from_raw(raw.user_data), .result = raw.res, .flags = raw.flags};
      } else {
        const abi::FlatCqe& raw = cqes_[index];
        head += 1U;
        if ((raw.flags & abi::kCqeSkip) != 0U) continue;
        out[count++] = Completion{.token = UserDataToken::from_raw(raw.user_data), .result = raw.res, .flags = raw.flags};
      }
    }

    if (head != initial_head) {
      StoreRelease(head_, head);
    }
    return count;
  }

  template <class Callback>
  constexpr std::size_t ReapAndDispatch(std::span<ReapedCqe> scratch, Callback&& callback) noexcept {
    const std::size_t count = ReapBatch(scratch);
    if (count != 0U) {
      callback(std::span<const ReapedCqe>(scratch.data(), count));
    }
    return count;
  }
};

}  // namespace liburing::ring

namespace liburing {
using ring::CqCursor;
using ring::ReapedCqe;
}  // namespace liburing
