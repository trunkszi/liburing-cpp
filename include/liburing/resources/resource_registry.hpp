/** @file @brief Ring-global coordinator for bgids, fixed-file/buffer ranges, and control registrations. */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/error/uring_error.hpp>
#include <span>
#include <uarch/uarch.hpp>

namespace liburing::resources {

struct IndexRange {
  std::uint32_t offset{};
  std::uint32_t count{};

  constexpr bool operator==(const IndexRange&) const noexcept = default;
};

class ResourceRegistry final {
 public:
  static constexpr std::uint32_t kMaxBgids = 64U;

 private:
  uarch::Bitmap64 free_bgids_{uarch::Bitmap64::all()};
  std::uint32_t max_files_{0U};
  std::uint32_t next_file_offset_{0U};
  std::uint32_t max_buffers_{0U};
  std::uint32_t next_buffer_offset_{0U};

 public:
  constexpr explicit ResourceRegistry(std::uint32_t max_files = 64U, std::uint32_t max_buffers = 64U) noexcept
      : max_files_(max_files), max_buffers_(max_buffers <= 65536U ? max_buffers : 65536U) {}

  [[nodiscard]] constexpr Result<std::uint16_t> AllocateBgid() noexcept {
    const auto free_bit = free_bgids_.lowest_one();
    if (!free_bit) {
      return std::unexpected(UringErrorCode::kNoSpace);
    }
    free_bgids_ = free_bgids_.clear(*free_bit);
    return static_cast<std::uint16_t>(*free_bit);
  }

  [[nodiscard]] constexpr Result<void> ReleaseBgid(std::uint16_t bgid) noexcept {
    if (bgid >= kMaxBgids || free_bgids_.test(bgid)) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    free_bgids_ = free_bgids_.set(bgid);
    return {};
  }

  [[nodiscard]] constexpr bool IsBgidAllocated(std::uint16_t bgid) const noexcept {
    return bgid < kMaxBgids && !free_bgids_.test(bgid);
  }

  [[nodiscard]] constexpr std::uint32_t FreeBgidCount() const noexcept { return free_bgids_.count(); }

  [[nodiscard]] constexpr Result<IndexRange> AllocateFileRange(std::uint32_t count) noexcept {
    if (count == 0U) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    if (count > max_files_ - next_file_offset_) {
      return std::unexpected(UringErrorCode::kNoSpace);
    }
    const IndexRange range{.offset = next_file_offset_, .count = count};
    next_file_offset_ += count;
    return range;
  }

  template <class Ring>
  [[nodiscard]] Result<IndexRange> ConfigureKernelFileAllocRange(Ring& ring, std::uint32_t count) noexcept {
    auto allocated = AllocateFileRange(count);
    if (!allocated) return std::unexpected(allocated.error());
    const abi::IoUringFileIndexRange range{.off = allocated->offset, .len = allocated->count};
    auto result = ring.Register(abi::kRegisterFileAllocRange, &range, 0U);
    if (!result) {
      next_file_offset_ = allocated->offset;
      return std::unexpected(result.error());
    }
    return *allocated;
  }

  [[nodiscard]] constexpr Result<IndexRange> AllocateBufferRange(std::uint32_t count) noexcept {
    if (count == 0U) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    if (count > max_buffers_ - next_buffer_offset_) {
      return std::unexpected(UringErrorCode::kNoSpace);
    }
    const IndexRange range{.offset = next_buffer_offset_, .count = count};
    next_buffer_offset_ += count;
    return range;
  }

  [[nodiscard]] constexpr std::uint32_t RemainingFiles() const noexcept { return max_files_ - next_file_offset_; }

  [[nodiscard]] constexpr std::uint32_t RemainingBuffers() const noexcept { return max_buffers_ - next_buffer_offset_; }

  template <class Ring>
  [[nodiscard]] Result<void> RegisterEventfd(Ring& ring, std::int32_t event_fd, bool async_only = false) const noexcept {
    if (event_fd < 0) return std::unexpected(UringErrorCode::kBadFd);
    const std::uint32_t op = async_only ? abi::kRegisterEventfdAsync : abi::kRegisterEventfd;
    auto res = ring.Register(op, &event_fd, 1U);
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> UnregisterEventfd(Ring& ring) const noexcept {
    auto res = ring.Register(abi::kUnregisterEventfd, nullptr, 0U);
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<std::uint16_t> RegisterPersonality(Ring& ring) const noexcept {
    auto res = ring.Register(abi::kRegisterPersonality, nullptr, 0U);
    if (!res) return std::unexpected(res.error());
    return static_cast<std::uint16_t>(*res);
  }

  template <class Ring>
  [[nodiscard]] Result<void> UnregisterPersonality(Ring& ring, std::uint16_t personality_id) const noexcept {
    auto res = ring.Register(abi::kUnregisterPersonality, nullptr, personality_id);
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> RegisterRestrictions(Ring& ring,
                                                  std::span<const abi::IoUringRestriction> restrictions) const noexcept {
    if (restrictions.empty()) return std::unexpected(UringErrorCode::kInvalid);
    auto res = ring.Register(abi::kRegisterRestrictions, restrictions.data(), static_cast<std::uint32_t>(restrictions.size()));
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> EnableRings(Ring& ring) const noexcept {
    auto res = ring.Register(abi::kRegisterEnableRings, nullptr, 0U);
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> RegisterIoWqAff(Ring& ring, std::span<const std::byte> cpu_mask) const noexcept {
    if (cpu_mask.empty()) return std::unexpected(UringErrorCode::kInvalid);
    auto res = ring.Register(abi::kRegisterIoWqAff, cpu_mask.data(), static_cast<std::uint32_t>(cpu_mask.size()));
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> UnregisterIoWqAff(Ring& ring) const noexcept {
    auto res = ring.Register(abi::kUnregisterIoWqAff, nullptr, 0U);
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<std::array<std::uint32_t, 2>> RegisterIoWqMaxWorkers(Ring& ring, std::uint32_t bounded_workers,
                                                                            std::uint32_t unbounded_workers) const noexcept {
    std::array<std::uint32_t, 2> counts{bounded_workers, unbounded_workers};
    auto res = ring.Register(abi::kRegisterIoWqMaxWorkers, counts.data(), 2U);
    if (!res) return std::unexpected(res.error());
    return counts;
  }

  template <class Ring>
  [[nodiscard]] Result<void> SyncCancel(Ring& ring, abi::IoUringSyncCancelReg& cancel_reg) const noexcept {
    auto res = ring.Register(abi::kRegisterSyncCancel, &cancel_reg, 1U);
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> RegisterClock(Ring& ring, std::uint32_t clock_id) const noexcept {
    const abi::IoUringClockRegister arg{.clockid = clock_id};
    auto res = ring.Register(abi::kRegisterClock, &arg, 0U);
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> SendMsgRing(Ring& ring, const abi::FlatSqe& msg_sqe) const noexcept {
    auto res = ring.Register(abi::kRegisterSendMsgRing, &msg_sqe, 1U);
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> ResizeRings(Ring& ring, abi::IoUringParams& new_params) const noexcept {
    if constexpr (requires { ring.ResizeRings(new_params.sq_entries, new_params.cq_entries); })
      return ring.ResizeRings(new_params.sq_entries, new_params.cq_entries);
    else
      return std::unexpected(UringErrorCode::kNotSupported);
  }
};

}  // namespace liburing::resources

namespace liburing {
using resources::IndexRange;
using resources::ResourceRegistry;
}  // namespace liburing
