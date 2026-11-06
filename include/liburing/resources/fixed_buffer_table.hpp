/** @file @brief Bitmap64 O(1) fixed-buffer slot allocator and REGISTER_BUFFERS/BUFFERS2/UPDATE/CLONE_BUFFERS wrappers. */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/error/uring_error.hpp>
#include <liburing/resources/resource_registry.hpp>
#include <liburing/sys/syscall.hpp>
#include <span>
#include <uarch/uarch.hpp>

namespace liburing::resources {

struct IoVec {
  void* iov_base{};
  std::size_t iov_len{};
};

static_assert(sizeof(IoVec) == 16U && uarch::NoPadding<IoVec> && uarch::GprPair<IoVec>);

template <std::size_t MaxBuffers = 64U>
class FixedBufferTable final {
  static_assert(MaxBuffers <= 65536ULL);
  static_assert(MaxBuffers > 0U && (MaxBuffers % 64U) == 0U, "MaxBuffers must be a positive multiple of 64");

 public:
  static constexpr std::size_t kWordCount = MaxBuffers / 64U;

 private:
  std::array<uarch::Bitmap64, kWordCount> free_words_{};
  std::uint32_t base_offset_{0U};
  std::uint32_t capacity_{static_cast<std::uint32_t>(MaxBuffers)};

 public:
  [[nodiscard]] static constexpr bool ValidRange(IndexRange range) noexcept {
    return range.count <= MaxBuffers && static_cast<std::uint64_t>(range.offset) + range.count <= 65536ULL;
  }
  [[nodiscard]] static constexpr Result<FixedBufferTable> Create(IndexRange range = {
                                                                     0U, static_cast<std::uint32_t>(MaxBuffers)}) noexcept {
    if (!ValidRange(range)) return std::unexpected(UringErrorCode::kInvalid);
    return FixedBufferTable(range);
  }
  constexpr explicit FixedBufferTable(IndexRange range = {0U, static_cast<std::uint32_t>(MaxBuffers)}) noexcept
      : base_offset_(range.offset), capacity_(ValidRange(range) ? range.count : 0U) {
    for (std::uint32_t i = 0; i < capacity_; ++i) {
      const std::size_t word = uarch::PowerOfTwo<6>::div(i);
      const auto bit = static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(i));
      free_words_[word] = free_words_[word].set(bit);
    }
  }

  [[nodiscard]] constexpr std::uint32_t BaseOffset() const noexcept { return base_offset_; }
  [[nodiscard]] constexpr std::uint32_t Capacity() const noexcept { return capacity_; }

  [[nodiscard]] constexpr Result<std::uint16_t> AllocateSlot() noexcept {
    for (std::size_t w = 0; w < kWordCount; ++w) {
      const auto bit = free_words_[w].lowest_one();
      if (bit) {
        free_words_[w] = free_words_[w].clear(*bit);
        return static_cast<std::uint16_t>(base_offset_ + static_cast<std::uint32_t>(w * 64U) + *bit);
      }
    }
    return std::unexpected(UringErrorCode::kNoSpace);
  }

  [[nodiscard]] constexpr Result<void> ReleaseSlot(std::uint16_t global_slot) noexcept {
    if (global_slot < base_offset_ || (global_slot - base_offset_) >= capacity_) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    const std::uint32_t local = global_slot - base_offset_;
    const std::size_t word = uarch::PowerOfTwo<6>::div(local);
    const auto bit = static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(local));
    if (free_words_[word].test(bit)) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    free_words_[word] = free_words_[word].set(bit);
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> RegisterBuffers(Ring& ring, std::span<const IoVec> iovecs) const noexcept {
    if (iovecs.empty()) return std::unexpected(UringErrorCode::kInvalid);
    auto res = ring.Register(abi::kRegisterBuffers, iovecs.data(), static_cast<std::uint32_t>(iovecs.size()));
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> RegisterBuffers2(Ring& ring, std::span<const IoVec> iovecs,
                                              std::span<const std::uint64_t> tags = {},
                                              std::uint32_t flags = 0U) const noexcept {
    if (iovecs.empty() || (!tags.empty() && tags.size() != iovecs.size())) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    const abi::IoUringRsrcRegister reg{
        .nr = static_cast<std::uint32_t>(iovecs.size()),
        .flags = flags,
        .resv2 = 0U,
        .data = sys::Address(iovecs.data()),
        .tags = tags.empty() ? 0ULL : sys::Address(tags.data()),
    };
    auto res = ring.Register(abi::kRegisterBuffers2, &reg, sizeof(reg));
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> RegisterSparseBuffers(Ring& ring, std::uint32_t nr) const noexcept {
    if (nr == 0U) return std::unexpected(UringErrorCode::kInvalid);
    const abi::IoUringRsrcRegister reg{
        .nr = nr,
        .flags = abi::kRsrcRegisterSparse,
        .resv2 = 0U,
        .data = 0ULL,
        .tags = 0ULL,
    };
    auto res = ring.Register(abi::kRegisterBuffers2, &reg, sizeof(reg));
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<void> UnregisterBuffers(Ring& ring) const noexcept {
    auto res = ring.Register(abi::kUnregisterBuffers, nullptr, 0U);
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<std::uint32_t> UpdateBuffers(Ring& ring, std::uint32_t offset, std::span<const IoVec> iovecs,
                                                    std::span<const std::uint64_t> tags = {}) const noexcept {
    if (iovecs.empty() || (!tags.empty() && tags.size() != iovecs.size())) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    const abi::IoUringRsrcUpdate2 upd{
        .offset = offset,
        .resv = 0U,
        .data = sys::Address(iovecs.data()),
        .tags = tags.empty() ? 0ULL : sys::Address(tags.data()),
        .nr = static_cast<std::uint32_t>(iovecs.size()),
        .resv2 = 0U,
    };
    auto res = ring.Register(abi::kRegisterBuffersUpdate, &upd, sizeof(upd));
    if (!res) return std::unexpected(res.error());
    return static_cast<std::uint32_t>(*res);
  }

  template <class Ring>
  [[nodiscard]] Result<void> CloneBuffers(Ring& ring, std::uint32_t src_fd, std::uint32_t flags = 0U,
                                          std::uint32_t src_off = 0U, std::uint32_t dst_off = 0U,
                                          std::uint32_t nr = 0U) const noexcept {
    const abi::IoUringCloneBuffers clone{
        .src_fd = src_fd,
        .flags = flags,
        .src_off = src_off,
        .dst_off = dst_off,
        .nr = nr,
    };
    auto res = ring.Register(abi::kRegisterCloneBuffers, &clone, 1U);
    if (!res) return std::unexpected(res.error());
    return {};
  }
};

}  // namespace liburing::resources

namespace liburing {
using resources::FixedBufferTable;
using resources::IoVec;
}  // namespace liburing
