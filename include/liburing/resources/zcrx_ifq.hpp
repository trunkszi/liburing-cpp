/** @file @brief Zero-copy RX interface queue (ZCRX_IFQ), ZCRX_CTRL, MEM_REGION, and BPF_FILTER wrappers. */
#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/error/uring_error.hpp>
#include <liburing/sys/syscall.hpp>

namespace liburing::resources {

class ZcrxRefillQueue final {
  std::uint32_t* head_{nullptr};
  std::uint32_t* tail_{nullptr};
  std::uint32_t ring_entries_{0U};
  std::uint32_t mask_{0U};
  abi::IoUringZcrxRqe* rqes_{nullptr};
  std::uint32_t local_tail_{0U};
  std::uint32_t flushed_tail_{0U};

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

 public:
  constexpr ZcrxRefillQueue() noexcept = default;

  constexpr ZcrxRefillQueue(std::uint32_t* head, std::uint32_t* tail, std::uint32_t ring_entries,
                            abi::IoUringZcrxRqe* rqes) noexcept
      : head_(head),
        tail_(tail),
        ring_entries_(ring_entries),
        mask_(ring_entries > 0U ? (ring_entries - 1U) : 0U),
        rqes_(rqes),
        local_tail_(tail != nullptr ? *tail : 0U),
        flushed_tail_(local_tail_) {}

  ZcrxRefillQueue(std::uint32_t* head, std::uint32_t* tail, std::uint32_t entries, std::uint64_t* storage) noexcept
      : ZcrxRefillQueue(head, tail, entries, reinterpret_cast<abi::IoUringZcrxRqe*>(storage)) {}

  [[nodiscard]] static constexpr std::uint64_t MakeAreaToken(std::uint16_t area_id, std::uint64_t area_offset) noexcept {
    const std::uint64_t high = (static_cast<std::uint64_t>(area_id) << abi::kZcrxAreaShift) & abi::kZcrxAreaMask;
    return high | (area_offset & ~abi::kZcrxAreaMask);
  }

  [[nodiscard]] constexpr std::uint32_t FreeSlots() const noexcept {
    const std::uint32_t used = local_tail_ - LoadAcquire(head_);
    return used <= ring_entries_ ? (ring_entries_ - used) : 0U;
  }

  [[nodiscard]] constexpr bool EnqueueAreaOffset(std::uint16_t area_id, std::uint64_t area_offset,
                                                 std::uint32_t length) noexcept {
    if (rqes_ == nullptr || ring_entries_ == 0U || !std::has_single_bit(ring_entries_) || FreeSlots() == 0U) {
      return false;
    }
    rqes_[local_tail_ & mask_] = {.off = MakeAreaToken(area_id, area_offset), .len = length, .pad = 0};
    ++local_tail_;
    return true;
  }

  [[nodiscard]] constexpr std::uint32_t FlushTail() noexcept {
    const std::uint32_t count = local_tail_ - flushed_tail_;
    StoreRelease(tail_, local_tail_);
    flushed_tail_ = local_tail_;
    return count;
  }
};

template <class Ring>
[[nodiscard]] Result<void> RegisterMemRegion(Ring& ring, abi::IoUringRegionDesc& region_desc,
                                             std::uint64_t flags = abi::kMemRegionTypeUser) noexcept {
  const abi::IoUringMemRegionReg reg{
      .region_uptr = sys::Address(&region_desc),
      .flags = flags,
  };
  auto res = ring.Register(abi::kRegisterMemRegion, &reg, 1U);
  if (!res) return std::unexpected(res.error());
  return {};
}

template <class Ring>
[[nodiscard]] Result<std::uint32_t> RegisterZcrxIfq(Ring& ring, abi::IoUringZcrxIfqReg& ifq_reg) noexcept {
  auto res = ring.Register(abi::kRegisterZcrxIfq, &ifq_reg, 1U);
  if (!res) return std::unexpected(res.error());
  return ifq_reg.zcrx_id;
}

template <class Ring>
[[nodiscard]] Result<void> RegisterZcrxCtrl(Ring& ring, void* ctrl_arg, std::uint32_t nr_args = 1U) noexcept {
  if (ctrl_arg == nullptr) return std::unexpected(UringErrorCode::kInvalid);
  auto res = ring.Register(abi::kRegisterZcrxCtrl, ctrl_arg, nr_args);
  if (!res) return std::unexpected(res.error());
  return {};
}

template <class Ring>
[[nodiscard]] Result<void> RegisterBpfFilter(Ring& ring, const void* filter_arg, std::uint32_t nr_args = 1U) noexcept {
  if (filter_arg == nullptr) return std::unexpected(UringErrorCode::kInvalid);
  auto res = ring.Register(abi::kRegisterBpfFilter, filter_arg, nr_args);
  if (!res) return std::unexpected(res.error());
  return {};
}

}  // namespace liburing::resources

namespace liburing {
using resources::RegisterBpfFilter;
using resources::RegisterMemRegion;
using resources::RegisterZcrxCtrl;
using resources::RegisterZcrxIfq;
using resources::ZcrxRefillQueue;
}  // namespace liburing
