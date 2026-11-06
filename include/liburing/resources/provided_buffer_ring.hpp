/** @file @brief Provided buffer ring (PBUF_RING / STATUS) and move-only BufferLease for standard, BUF_MORE, and BUNDLE modes.
 */
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/error/uring_error.hpp>
#include <liburing/sys/mmap_region.hpp>
#include <liburing/sys/syscall.hpp>
#include <span>
#include <uarch/uarch.hpp>
#include <utility>

namespace liburing::resources {

class ProvidedBufferRing;

class BufferLease final {
  ProvidedBufferRing* owner_{nullptr};
  std::span<std::byte> first_span_{};
  std::uint32_t total_bytes_{0U};
  std::uint16_t start_bid_{0U};
  std::uint16_t start_slot_{0U};
  std::uint16_t buffer_count_{0U};
  bool has_more_{false};
  bool active_{false};

  friend class ProvidedBufferRing;

  constexpr BufferLease(ProvidedBufferRing* owner, std::span<std::byte> first_span, std::uint32_t total_bytes,
                        std::uint16_t start_bid, std::uint16_t start_slot, std::uint16_t buffer_count, bool has_more) noexcept
      : owner_(owner),
        first_span_(first_span),
        total_bytes_(total_bytes),
        start_bid_(start_bid),
        start_slot_(start_slot),
        buffer_count_(buffer_count),
        has_more_(has_more),
        active_(true) {}

 public:
  constexpr BufferLease() noexcept = default;
  BufferLease(const BufferLease&) = delete;
  BufferLease& operator=(const BufferLease&) = delete;

  constexpr BufferLease(BufferLease&& other) noexcept
      : owner_(std::exchange(other.owner_, nullptr)),
        first_span_(std::exchange(other.first_span_, {})),
        total_bytes_(std::exchange(other.total_bytes_, 0U)),
        start_bid_(std::exchange(other.start_bid_, 0U)),
        start_slot_(std::exchange(other.start_slot_, 0U)),
        buffer_count_(std::exchange(other.buffer_count_, 0U)),
        has_more_(std::exchange(other.has_more_, false)),
        active_(std::exchange(other.active_, false)) {}

  constexpr BufferLease& operator=(BufferLease&& other) noexcept {
    if (this != &other) {
      Release();
      owner_ = std::exchange(other.owner_, nullptr);
      first_span_ = std::exchange(other.first_span_, {});
      total_bytes_ = std::exchange(other.total_bytes_, 0U);
      start_bid_ = std::exchange(other.start_bid_, 0U);
      start_slot_ = std::exchange(other.start_slot_, 0U);
      buffer_count_ = std::exchange(other.buffer_count_, 0U);
      has_more_ = std::exchange(other.has_more_, false);
      active_ = std::exchange(other.active_, false);
    }
    return *this;
  }

  constexpr ~BufferLease() noexcept { Release(); }

  [[nodiscard]] constexpr bool IsActive() const noexcept { return active_; }
  [[nodiscard]] constexpr std::uint16_t Bid() const noexcept { return start_bid_; }
  [[nodiscard]] constexpr std::uint16_t BufferId() const noexcept { return start_bid_; }
  [[nodiscard]] constexpr std::uint16_t BufferCount() const noexcept { return buffer_count_; }
  [[nodiscard]] constexpr std::uint32_t TotalBytes() const noexcept { return total_bytes_; }
  [[nodiscard]] constexpr bool HasMore() const noexcept { return has_more_; }
  [[nodiscard]] constexpr std::span<std::byte> Bytes() const noexcept { return first_span_; }
  [[nodiscard]] constexpr std::span<std::byte> Span() const noexcept { return first_span_; }
  [[nodiscard]] constexpr std::uint16_t BufferIdAt(std::size_t index) const noexcept;
  [[nodiscard]] constexpr std::span<std::byte> SpanAt(std::size_t index) const noexcept;

  constexpr void Release() noexcept;
};

class ProvidedBufferRing final {
 public:
  static constexpr std::size_t kMaxIncrementalEntries = 1024U;

 private:
  sys::MmapRegion mapped_ring_{};
  abi::IoUringBuf* descriptors_{nullptr};
  std::byte* backing_base_{nullptr};
  std::uint32_t ring_entries_{0U};
  std::uint32_t mask_{0U};
  std::uint32_t hist_mask_{0U};
  std::uint32_t buf_stride_{0U};
  uarch::FastDivisorU32 stride_div_{uarch::arithmetic::divisor_u32<1U>()};
  std::uint16_t bgid_{0U};
  std::uint16_t local_tail_{0U};
  bool incremental_{false};
  std::array<std::uint32_t, kMaxIncrementalEntries> consumed_offsets_{};
  std::array<std::uint16_t, kMaxIncrementalEntries> active_leases_{};
  std::array<bool, kMaxIncrementalEntries> kernel_done_{};
  std::array<std::uint16_t, kMaxIncrementalEntries> bid_to_slot_{};
  std::array<std::uint16_t, kMaxIncrementalEntries> next_bid_in_lease_{};
  std::array<std::uint16_t, kMaxIncrementalEntries * 2U> slot_bids_{};

  // In Linux UAPI struct io_uring_buf_ring, the shared 16-bit tail overlays descriptors_[0].resv.
  constexpr void StoreSharedTail(std::uint16_t value) noexcept {
    if (descriptors_ == nullptr) return;
    if consteval {
      descriptors_[0].resv = value;
    } else {
      __atomic_store_n(&descriptors_[0].resv, value, __ATOMIC_RELEASE);
    }
  }

  [[nodiscard]] constexpr std::uint16_t LoadSharedTail() const noexcept {
    if (descriptors_ == nullptr) return 0U;
    if consteval {
      return descriptors_[0].resv;
    } else {
      return __atomic_load_n(&descriptors_[0].resv, __ATOMIC_ACQUIRE);
    }
  }

  constexpr void PublishBid(std::uint16_t bid) noexcept {
    const std::uint32_t slot = static_cast<std::uint32_t>(local_tail_) & mask_;
    const std::uint32_t hist_slot = static_cast<std::uint32_t>(local_tail_) & hist_mask_;
    slot_bids_[hist_slot] = bid;
    bid_to_slot_[static_cast<std::uint32_t>(bid) & mask_] = local_tail_;
    auto* entry = &descriptors_[slot];
    entry->addr = reinterpret_cast<std::uint64_t>(backing_base_ + (static_cast<std::size_t>(bid) * buf_stride_));
    entry->len = buf_stride_;
    entry->bid = bid;
    ++local_tail_;
  }

  constexpr void PopulateInitialBuffers() noexcept {
    local_tail_ = 0U;
    for (std::uint32_t i = 0; i < ring_entries_; ++i) {
      PublishBid(static_cast<std::uint16_t>(i));
    }
    StoreSharedTail(local_tail_);
  }

 public:
  constexpr ProvidedBufferRing() noexcept = default;
  ProvidedBufferRing(const ProvidedBufferRing&) = delete;
  ProvidedBufferRing& operator=(const ProvidedBufferRing&) = delete;
  ProvidedBufferRing(ProvidedBufferRing&&) noexcept = default;
  ProvidedBufferRing& operator=(ProvidedBufferRing&&) noexcept = default;

  [[nodiscard]] static Result<ProvidedBufferRing> CreateInMemory(std::span<abi::IoUringBuf> descriptors,
                                                                 std::span<std::byte> backing_storage, std::uint32_t buf_stride,
                                                                 std::uint16_t bgid, bool incremental = false) noexcept {
    const auto entries = static_cast<std::uint32_t>(descriptors.size());
    if (entries == 0U || entries > kMaxIncrementalEntries || !std::has_single_bit(entries) || buf_stride == 0U ||
        backing_storage.size() < static_cast<std::size_t>(entries) * static_cast<std::size_t>(buf_stride)) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    ProvidedBufferRing ring;
    ring.descriptors_ = descriptors.data();
    ring.backing_base_ = backing_storage.data();
    ring.ring_entries_ = entries;
    ring.mask_ = entries - 1U;
    ring.hist_mask_ = (entries * 2U) - 1U;
    ring.buf_stride_ = buf_stride;
    ring.stride_div_ = *uarch::FastDivisorU32::try_new(buf_stride);
    ring.bgid_ = bgid;
    ring.incremental_ = incremental;
    ring.PopulateInitialBuffers();
    return ring;
  }

  template <class Ring>
  [[nodiscard]] static Result<ProvidedBufferRing> CreateKernelMapped(Ring& ring, std::uint32_t ring_entries,
                                                                     std::uint32_t buf_stride, std::uint16_t bgid,
                                                                     std::span<std::byte> backing_storage,
                                                                     bool incremental = false) noexcept {
    if (ring_entries == 0U || ring_entries > kMaxIncrementalEntries || !std::has_single_bit(ring_entries) || buf_stride == 0U ||
        backing_storage.size() < static_cast<std::size_t>(ring_entries) * static_cast<std::size_t>(buf_stride)) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    abi::IoUringBufReg reg{
        .ring_addr = 0ULL,
        .ring_entries = ring_entries,
        .bgid = bgid,
        .flags = static_cast<std::uint16_t>(abi::kPbufRingMmap | (incremental ? abi::kPbufRingInc : 0U)),
    };
    auto reg_res = ring.Register(abi::kRegisterPbufRing, &reg, 1U);
    if (!reg_res) return std::unexpected(reg_res.error());

    const std::size_t map_bytes = static_cast<std::size_t>(ring_entries) * sizeof(abi::IoUringBuf);
    const std::uint64_t map_offset = abi::kOffPbufRing | (static_cast<std::uint64_t>(bgid) << abi::kOffPbufShift);
    auto mapped = sys::MmapRegion::Map(nullptr, map_bytes, sys::kProtRead | sys::kProtWrite,
                                       sys::kMapShared | sys::kMapPopulate, ring.Fd(), map_offset);
    if (!mapped) {
      const abi::IoUringBufReg unreg{.bgid = bgid};
      static_cast<void>(ring.Register(abi::kUnregisterPbufRing, &unreg, 1U));
      return std::unexpected(mapped.error());
    }

    ProvidedBufferRing pbuf;
    pbuf.mapped_ring_ = std::move(*mapped);
    pbuf.descriptors_ = reinterpret_cast<abi::IoUringBuf*>(pbuf.mapped_ring_.bytes().data());
    pbuf.backing_base_ = backing_storage.data();
    pbuf.ring_entries_ = ring_entries;
    pbuf.mask_ = ring_entries - 1U;
    pbuf.hist_mask_ = (ring_entries * 2U) - 1U;
    pbuf.buf_stride_ = buf_stride;
    pbuf.stride_div_ = *uarch::FastDivisorU32::try_new(buf_stride);
    pbuf.bgid_ = bgid;
    pbuf.incremental_ = incremental;
    pbuf.PopulateInitialBuffers();
    return pbuf;
  }

  [[nodiscard]] constexpr std::uint16_t Bgid() const noexcept { return bgid_; }
  [[nodiscard]] constexpr std::uint32_t RingEntries() const noexcept { return ring_entries_; }
  [[nodiscard]] constexpr std::uint32_t BufferStride() const noexcept { return buf_stride_; }
  [[nodiscard]] constexpr bool IsIncremental() const noexcept { return incremental_; }
  [[nodiscard]] constexpr std::uint16_t SharedTail() const noexcept { return LoadSharedTail(); }

  [[nodiscard]] constexpr std::uint16_t SlotBidAt(std::uint16_t start_slot, std::size_t index) const noexcept {
    const std::uint32_t hist_slot = (static_cast<std::uint32_t>(start_slot) + static_cast<std::uint32_t>(index)) & hist_mask_;
    return slot_bids_[hist_slot];
  }

  [[nodiscard]] constexpr std::uint16_t LeaseBidAt(std::uint16_t start_bid, std::uint16_t offset) const noexcept {
    std::uint16_t cur = start_bid;
    for (std::uint16_t i = 0U; i < offset; ++i) {
      cur = next_bid_in_lease_[static_cast<std::uint32_t>(cur) & mask_];
    }
    return cur;
  }

  [[nodiscard]] constexpr std::span<std::byte> BufferSpan(std::uint16_t bid, std::uint32_t len,
                                                          std::uint32_t offset = 0U) const noexcept {
    if (backing_base_ == nullptr || bid >= ring_entries_) return {};
    std::byte* ptr = backing_base_ + (static_cast<std::size_t>(bid) * buf_stride_) + offset;
    return {ptr, len};
  }

  [[nodiscard]] Result<BufferLease> AcquireLease(std::int32_t cqe_res, std::uint32_t cqe_flags) noexcept {
    if ((cqe_flags & abi::kCqeBuffer) == 0U || cqe_res < 0) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    const auto bid = static_cast<std::uint16_t>(cqe_flags >> abi::kCqeBufferShift);
    if (bid >= ring_entries_) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    if (kernel_done_[bid]) return std::unexpected(UringErrorCode::kBusy);
    if (active_leases_[bid] == 65535U) return std::unexpected(UringErrorCode::kOverflow);
    const auto bytes_used = static_cast<std::uint32_t>(cqe_res);
    const bool has_more = (cqe_flags & abi::kCqeBufMore) != 0U;
    const std::uint32_t offset = incremental_ ? consumed_offsets_[bid] : 0U;
    if (bytes_used > buf_stride_ - offset || (has_more && !incremental_)) {
      return std::unexpected(UringErrorCode::kOverflow);
    }
    if (incremental_) {
      consumed_offsets_[bid] = offset + bytes_used;
    }
    kernel_done_[bid] = !has_more;
    ++active_leases_[bid];
    const std::uint16_t start_slot = bid_to_slot_[static_cast<std::uint32_t>(bid) & mask_];
    std::byte* slice_start = backing_base_ + (static_cast<std::size_t>(bid) * buf_stride_) + offset;
    return BufferLease(this, std::span<std::byte>(slice_start, bytes_used), bytes_used, bid, start_slot, 1U, has_more);
  }

  [[nodiscard]] Result<BufferLease> AcquireBundleLease(std::int32_t cqe_res, std::uint32_t cqe_flags) noexcept {
    if ((cqe_flags & abi::kCqeBuffer) == 0U || cqe_res < 0 || buf_stride_ == 0U) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    const auto start_bid = static_cast<std::uint16_t>(cqe_flags >> abi::kCqeBufferShift);
    if (start_bid >= ring_entries_) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    const auto total_bytes = static_cast<std::uint32_t>(cqe_res);
    const std::uint32_t count = total_bytes == 0U ? 1U : stride_div_.div(total_bytes - 1U) + 1U;
    if (count > ring_entries_) {
      return std::unexpected(UringErrorCode::kOverflow);
    }
    const std::uint16_t start_slot = bid_to_slot_[static_cast<std::uint32_t>(start_bid) & mask_];
    if ((cqe_flags & abi::kCqeBufMore) != 0U) return std::unexpected(UringErrorCode::kInvalid);
    std::array<uarch::Bitmap64, kMaxIncrementalEntries / 64U> seen{};
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto bid = SlotBidAt(start_slot, i);
      if (bid >= ring_entries_) return std::unexpected(UringErrorCode::kInvalid);
      const auto word = uarch::PowerOfTwo<6>::div(bid);
      const auto bit = static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(bid));
      if (kernel_done_[bid] || active_leases_[bid] != 0 || seen[word].test(bit)) return std::unexpected(UringErrorCode::kBusy);
      seen[word] = seen[word].set(bit);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto bid = SlotBidAt(start_slot, i);
      kernel_done_[bid] = true;
      active_leases_[bid] = 1;
    }
    for (std::uint32_t i = 0U; i + 1U < count; ++i) {
      const std::uint16_t cur_bid = (i == 0U) ? start_bid : SlotBidAt(start_slot, i);
      const std::uint16_t next_bid = SlotBidAt(start_slot, i + 1U);
      next_bid_in_lease_[static_cast<std::uint32_t>(cur_bid) & mask_] = next_bid;
    }
    const std::uint32_t first_len = total_bytes < buf_stride_ ? total_bytes : buf_stride_;
    std::byte* first_ptr = backing_base_ + (static_cast<std::size_t>(start_bid) * buf_stride_);
    return BufferLease(this, std::span<std::byte>(first_ptr, first_len), total_bytes, start_bid, start_slot,
                       static_cast<std::uint16_t>(count), false);
  }

  constexpr void RecycleFromLease(std::uint16_t start_bid, std::uint16_t start_slot, std::uint16_t count,
                                  bool has_more) noexcept {
    static_cast<void>(start_slot);
    static_cast<void>(has_more);
    if (count == 0U || descriptors_ == nullptr) return;
    std::uint16_t cur = start_bid;
    for (std::uint16_t i = 0U; i < count; ++i) {
      const auto next = next_bid_in_lease_[cur];
      if (active_leases_[cur] == 0) sys::Panic("buffer lease underflow");
      --active_leases_[cur];
      // A final CQE releases kernel ownership, but older slice leases still own their bytes.
      if (kernel_done_[cur] && active_leases_[cur] == 0) {
        consumed_offsets_[cur] = 0;
        kernel_done_[cur] = false;
        PublishBid(cur);
      }
      cur = next;
    }
    StoreSharedTail(local_tail_);
  }

  template <class Ring>
  [[nodiscard]] Result<abi::IoUringBufStatus> QueryStatus(Ring& ring) const noexcept {
    abi::IoUringBufStatus status{.buf_group = bgid_};
    auto res = ring.Register(abi::kRegisterPbufStatus, &status, 1U);
    if (!res) return std::unexpected(res.error());
    return status;
  }

  template <class Ring>
  [[nodiscard]] Result<void> Unregister(Ring& ring) noexcept {
    const abi::IoUringBufReg reg{.bgid = bgid_};
    auto res = ring.Register(abi::kUnregisterPbufRing, &reg, 1U);
    if (!res) return std::unexpected(res.error());
    auto unmap = mapped_ring_.reset();
    if (!unmap) return std::unexpected(unmap.error());
    descriptors_ = nullptr;
    return {};
  }
};

constexpr std::uint16_t BufferLease::BufferIdAt(std::size_t index) const noexcept {
  if (!active_ || owner_ == nullptr || index >= buffer_count_) {
    return 0U;
  }
  if (index == 0U) {
    return start_bid_;
  }
  return owner_->LeaseBidAt(start_bid_, static_cast<std::uint16_t>(index));
}

constexpr std::span<std::byte> BufferLease::SpanAt(std::size_t index) const noexcept {
  if (!active_ || owner_ == nullptr || index >= buffer_count_) {
    return {};
  }
  if (index == 0U) {
    return first_span_;
  }
  const std::uint32_t stride = owner_->BufferStride();
  const std::uint32_t consumed_before = static_cast<std::uint32_t>(index) * stride;
  const std::uint32_t remaining = total_bytes_ > consumed_before ? (total_bytes_ - consumed_before) : 0U;
  const std::uint32_t len = remaining < stride ? remaining : stride;
  return owner_->BufferSpan(owner_->LeaseBidAt(start_bid_, static_cast<std::uint16_t>(index)), len);
}

constexpr void BufferLease::Release() noexcept {
  if (active_ && owner_ != nullptr) {
    owner_->RecycleFromLease(start_bid_, start_slot_, buffer_count_, has_more_);
  }
  owner_ = nullptr;
  first_span_ = {};
  total_bytes_ = 0U;
  start_bid_ = 0U;
  start_slot_ = 0U;
  buffer_count_ = 0U;
  has_more_ = false;
  active_ = false;
}

}  // namespace liburing::resources

namespace liburing {
using resources::BufferLease;
using resources::ProvidedBufferRing;
}  // namespace liburing
