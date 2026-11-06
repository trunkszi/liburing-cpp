/** @file @brief Bitmap64 O(1) fixed-file slot allocator and REGISTER_FILES/FILES2/UPDATE2/ALLOC_RANGE wrappers. */
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

template <std::size_t MaxFiles = 64U>
class FixedFileTable final {
  static_assert(MaxFiles <= 0xffffffffULL);
  static_assert(MaxFiles > 0U && (MaxFiles % 64U) == 0U, "MaxFiles must be a positive multiple of 64");

 public:
  static constexpr std::size_t kWordCount = MaxFiles / 64U;

 private:
  std::array<uarch::Bitmap64, kWordCount> free_words_{};
  std::uint32_t registered_entries_{};
  IndexRange alloc_range_{};
  std::uint32_t base_offset_{0U};
  std::uint32_t capacity_{static_cast<std::uint32_t>(MaxFiles)};

  template <class Ring>
  [[nodiscard]] Result<void> FinishRegistration(Ring& ring, std::uint32_t entries) noexcept {
    const abi::IoUringFileIndexRange disabled{};
    auto result = ring.Register(abi::kRegisterFileAllocRange, &disabled, 0U);
    if (!result) {
      static_cast<void>(ring.Register(abi::kUnregisterFiles, nullptr, 0U));
      return std::unexpected(result.error());
    }
    registered_entries_ = entries;
    alloc_range_ = {};
    return {};
  }

 public:
  [[nodiscard]] static constexpr bool ValidRange(IndexRange range) noexcept {
    return range.count <= MaxFiles && static_cast<std::uint64_t>(range.offset) + range.count <= 0xffffffffULL;
  }
  [[nodiscard]] static constexpr Result<FixedFileTable> Create(IndexRange range = {
                                                                   0U, static_cast<std::uint32_t>(MaxFiles)}) noexcept {
    if (!ValidRange(range)) return std::unexpected(UringErrorCode::kInvalid);
    return FixedFileTable(range);
  }
  constexpr explicit FixedFileTable(IndexRange range = {0U, static_cast<std::uint32_t>(MaxFiles)}) noexcept
      : base_offset_(range.offset), capacity_(ValidRange(range) ? range.count : 0U) {
    for (std::uint32_t i = 0; i < capacity_; ++i) {
      const std::size_t word = uarch::PowerOfTwo<6>::div(i);
      const auto bit = static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(i));
      free_words_[word] = free_words_[word].set(bit);
    }
  }

  [[nodiscard]] constexpr std::uint32_t BaseOffset() const noexcept { return base_offset_; }
  [[nodiscard]] constexpr std::uint32_t Capacity() const noexcept { return capacity_; }

  [[nodiscard]] constexpr Result<std::uint32_t> AllocateSlot() noexcept {
    for (std::size_t w = 0; w < kWordCount; ++w) {
      const auto bit = free_words_[w].lowest_one();
      if (bit) {
        free_words_[w] = free_words_[w].clear(*bit);
        return base_offset_ + static_cast<std::uint32_t>(w * 64U) + *bit;
      }
    }
    return std::unexpected(UringErrorCode::kNoSpace);
  }

  [[nodiscard]] constexpr Result<void> ReleaseSlot(std::uint32_t global_slot) noexcept {
    if (global_slot < base_offset_ || (global_slot - base_offset_) >= capacity_) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    if (global_slot >= alloc_range_.offset &&
        static_cast<std::uint64_t>(global_slot) < static_cast<std::uint64_t>(alloc_range_.offset) + alloc_range_.count)
      return std::unexpected(UringErrorCode::kBusy);
    const std::uint32_t local = global_slot - base_offset_;
    const std::size_t word = uarch::PowerOfTwo<6>::div(local);
    const auto bit = static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(local));
    if (free_words_[word].test(bit)) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    free_words_[word] = free_words_[word].set(bit);
    return {};
  }

  [[nodiscard]] constexpr bool IsAllocated(std::uint32_t global_slot) const noexcept {
    if (global_slot < base_offset_ || (global_slot - base_offset_) >= capacity_) {
      return false;
    }
    const std::uint32_t local = global_slot - base_offset_;
    const std::size_t word = uarch::PowerOfTwo<6>::div(local);
    const auto bit = static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(local));
    return !free_words_[word].test(bit);
  }

  [[nodiscard]] constexpr std::uint32_t FreeCount() const noexcept {
    std::uint32_t total = 0U;
    for (const auto& word : free_words_) {
      total += word.count();
    }
    return total;
  }

  template <class Ring>
  [[nodiscard]] Result<void> RegisterFiles(Ring& ring, std::span<const std::int32_t> fds) noexcept {
    if (static_cast<std::uint64_t>(base_offset_) + capacity_ > fds.size() || fds.empty())
      return std::unexpected(UringErrorCode::kInvalid);
    auto res = ring.Register(abi::kRegisterFiles, fds.data(), static_cast<std::uint32_t>(fds.size()));
    if (!res) return std::unexpected(res.error());
    return FinishRegistration(ring, static_cast<std::uint32_t>(fds.size()));
  }

  template <class Ring>
  [[nodiscard]] Result<void> RegisterFiles2(Ring& ring, std::span<const std::int32_t> fds,
                                            std::span<const std::uint64_t> tags = {}, std::uint32_t flags = 0U) noexcept {
    if (static_cast<std::uint64_t>(base_offset_) + capacity_ > fds.size() || fds.empty() ||
        (!tags.empty() && tags.size() != fds.size())) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    const abi::IoUringRsrcRegister reg{
        .nr = static_cast<std::uint32_t>(fds.size()),
        .flags = flags,
        .resv2 = 0U,
        .data = sys::Address(fds.data()),
        .tags = tags.empty() ? 0ULL : sys::Address(tags.data()),
    };
    auto res = ring.Register(abi::kRegisterFiles2, &reg, sizeof(reg));
    if (!res) return std::unexpected(res.error());
    return FinishRegistration(ring, static_cast<std::uint32_t>(fds.size()));
  }

  template <class Ring>
  [[nodiscard]] Result<void> RegisterSparseFiles(Ring& ring, std::uint32_t nr) noexcept {
    if (nr == 0U || static_cast<std::uint64_t>(base_offset_) + capacity_ > nr) return std::unexpected(UringErrorCode::kInvalid);
    const abi::IoUringRsrcRegister reg{
        .nr = nr,
        .flags = abi::kRsrcRegisterSparse,
        .resv2 = 0U,
        .data = 0ULL,
        .tags = 0ULL,
    };
    auto res = ring.Register(abi::kRegisterFiles2, &reg, sizeof(reg));
    if (!res) return std::unexpected(res.error());
    return FinishRegistration(ring, nr);
  }

  template <class Ring>
  [[nodiscard]] Result<void> UnregisterFiles(Ring& ring) noexcept {
    auto res = ring.Register(abi::kUnregisterFiles, nullptr, 0U);
    if (!res) return std::unexpected(res.error());
    for (std::uint32_t i = 0; i < alloc_range_.count; ++i) {
      const auto local = alloc_range_.offset + i - base_offset_;
      auto& word = free_words_[uarch::PowerOfTwo<6>::div(local)];
      word = word.set(static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(local)));
    }
    registered_entries_ = 0;
    alloc_range_ = {};
    return {};
  }

  template <class Ring>
  [[nodiscard]] Result<std::uint32_t> UpdateFiles(Ring& ring, std::uint32_t offset,
                                                  std::span<const std::int32_t> fds) const noexcept {
    if (fds.empty()) return std::unexpected(UringErrorCode::kInvalid);
    const abi::IoUringRsrcUpdate upd{
        .offset = offset,
        .resv = 0U,
        .data = sys::Address(fds.data()),
    };
    auto res = ring.Register(abi::kRegisterFilesUpdate, &upd, static_cast<std::uint32_t>(fds.size()));
    if (!res) return std::unexpected(res.error());
    return static_cast<std::uint32_t>(*res);
  }

  template <class Ring>
  [[nodiscard]] Result<std::uint32_t> UpdateFiles2(Ring& ring, std::uint32_t offset, std::span<const std::int32_t> fds,
                                                   std::span<const std::uint64_t> tags = {}) const noexcept {
    if (fds.empty() || (!tags.empty() && tags.size() != fds.size())) {
      return std::unexpected(UringErrorCode::kInvalid);
    }
    const abi::IoUringRsrcUpdate2 upd{
        .offset = offset,
        .resv = 0U,
        .data = sys::Address(fds.data()),
        .tags = tags.empty() ? 0ULL : sys::Address(tags.data()),
        .nr = static_cast<std::uint32_t>(fds.size()),
        .resv2 = 0U,
    };
    auto res = ring.Register(abi::kRegisterFilesUpdate2, &upd, sizeof(upd));
    if (!res) return std::unexpected(res.error());
    return static_cast<std::uint32_t>(*res);
  }

  [[nodiscard]] constexpr IndexRange KernelAllocRange() const noexcept { return alloc_range_; }
  template <class Ring>
  [[nodiscard]] Result<void> ConfigureKernelAllocRange(Ring& ring, IndexRange kernel_range) noexcept {
    const auto end = static_cast<std::uint64_t>(kernel_range.offset) + kernel_range.count;
    const auto local_end = static_cast<std::uint64_t>(base_offset_) + capacity_;
    if (registered_entries_ == 0 || kernel_range.count == 0 || kernel_range.offset < base_offset_ || end > local_end ||
        end > registered_entries_)
      return std::unexpected(UringErrorCode::kInvalid);
    auto reserved = [&](std::uint32_t slot) noexcept {
      return slot >= alloc_range_.offset &&
             static_cast<std::uint64_t>(slot) < static_cast<std::uint64_t>(alloc_range_.offset) + alloc_range_.count;
    };
    for (auto slot = kernel_range.offset; static_cast<std::uint64_t>(slot) < end; ++slot)
      if (IsAllocated(slot) && !reserved(slot)) return std::unexpected(UringErrorCode::kBusy);
    const abi::IoUringFileIndexRange range{.off = kernel_range.offset, .len = kernel_range.count, .resv = 0ULL};
    auto result = ring.Register(abi::kRegisterFileAllocRange, &range, 0U);
    if (!result) return std::unexpected(result.error());
    // Change bitmap ownership only after the kernel accepts the replacement partition.
    auto set_free = [&](std::uint32_t slot, bool free) noexcept {
      const auto local = slot - base_offset_;
      auto& word = free_words_[uarch::PowerOfTwo<6>::div(local)];
      const auto bit = static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(local));
      word = free ? word.set(bit) : word.clear(bit);
    };
    for (std::uint32_t i = 0; i < alloc_range_.count; ++i) set_free(alloc_range_.offset + i, true);
    for (std::uint32_t i = 0; i < kernel_range.count; ++i) set_free(kernel_range.offset + i, false);
    alloc_range_ = kernel_range;
    return {};
  }
  template <class Ring>
  [[nodiscard]] Result<void> SetFileAllocRange(Ring& ring, std::uint32_t offset, std::uint32_t len) noexcept {
    return ConfigureKernelAllocRange(ring, {offset, len});
  }
};

}  // namespace liburing::resources

namespace liburing {
using resources::FixedFileTable;
}  // namespace liburing
