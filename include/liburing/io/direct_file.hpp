/** @file @brief O_DIRECT descriptors validate alignment before encoding fixed-buffer transfers. */
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <liburing/ops/fs.hpp>
#include <liburing/resources/fixed_buffer_pool.hpp>
#include <liburing/sys/unique_fd.hpp>
#include <uarch/uarch.hpp>

namespace liburing::io {
enum class DirectAccess : std::uint8_t { kReadOnly = 0, kReadWrite = 1 };

struct StatxTimestamp {
  std::int64_t seconds;
  std::uint32_t nanoseconds;
  std::int32_t reserved;
};
struct StatxLayout {
  std::uint32_t stx_mask;
  std::uint32_t stx_blksize;
  std::uint64_t stx_attributes;
  std::uint32_t stx_nlink;
  std::uint32_t stx_uid;
  std::uint32_t stx_gid;
  std::uint16_t stx_mode;
  std::uint16_t spare0;
  std::uint64_t stx_ino;
  std::uint64_t stx_size;
  std::uint64_t stx_blocks;
  std::uint64_t stx_attributes_mask;
  StatxTimestamp stx_atime;
  StatxTimestamp stx_btime;
  StatxTimestamp stx_ctime;
  StatxTimestamp stx_mtime;
  std::uint32_t stx_rdev_major;
  std::uint32_t stx_rdev_minor;
  std::uint32_t stx_dev_major;
  std::uint32_t stx_dev_minor;
  std::uint64_t stx_mnt_id;
  std::uint32_t stx_dio_mem_align;
  std::uint32_t stx_dio_offset_align;
  std::array<std::uint64_t, 12> spare3;
};
static_assert(sizeof(StatxLayout) == 256 && alignof(StatxLayout) == 8 && uarch::NoPadding<StatxLayout>);
static_assert(offsetof(StatxLayout, stx_mask) == 0 && offsetof(StatxLayout, stx_mode) == 28);
static_assert(offsetof(StatxLayout, stx_rdev_major) == 128 && offsetof(StatxLayout, stx_rdev_minor) == 132);
static_assert(offsetof(StatxLayout, stx_dev_major) == 136 && offsetof(StatxLayout, stx_dev_minor) == 140);
static_assert(offsetof(StatxLayout, stx_dio_mem_align) == 152 && offsetof(StatxLayout, stx_dio_offset_align) == 156);

namespace detail {
inline constexpr std::uint32_t kStatxDioAlign = 0x2000U;
struct DioAlignments {
  std::uint32_t memory;
  std::uint32_t offset;
};
static_assert(uarch::NoPadding<DioAlignments> && uarch::RegisterPassable<DioAlignments>);
[[nodiscard]] inline Result<DioAlignments> ValidateDioAlignments(const StatxLayout& info) noexcept {
  if ((info.stx_mask & kStatxDioAlign) == 0) return DioAlignments{4096, 4096};
  if (!std::has_single_bit(info.stx_dio_mem_align) || !std::has_single_bit(info.stx_dio_offset_align))
    return std::unexpected(UringErrorCode::kInvalid);
  return DioAlignments{info.stx_dio_mem_align, info.stx_dio_offset_align};
}
}  // namespace detail

// Prepared operations borrow this fd and their buffer through the terminal CQE.
class DirectFile final {
  sys::UniqueFd fd_;
  std::uint32_t mem_align_{4096};
  std::uint32_t offset_align_{4096};
  std::uint32_t rdev_major_{};
  std::uint32_t rdev_minor_{};
  std::uint32_t dev_major_{};
  std::uint32_t dev_minor_{};
  std::uint16_t mode_{};
  DirectFile(sys::UniqueFd fd, const StatxLayout& info, detail::DioAlignments alignments) noexcept
      : fd_(std::move(fd)),
        mem_align_(alignments.memory),
        offset_align_(alignments.offset),
        rdev_major_(info.stx_rdev_major),
        rdev_minor_(info.stx_rdev_minor),
        dev_major_(info.stx_dev_major),
        dev_minor_(info.stx_dev_minor),
        mode_(info.stx_mode) {}
  [[nodiscard]] static UringErrorCode LastError() noexcept { return SyscallError{static_cast<std::uint16_t>(errno)}.code(); }

 public:
  DirectFile(DirectFile&&) noexcept = default;
  DirectFile& operator=(DirectFile&&) noexcept = default;
  DirectFile(const DirectFile&) = delete;
  DirectFile& operator=(const DirectFile&) = delete;

  [[nodiscard]] static Result<DirectFile> Open(const char* path, DirectAccess access) noexcept {
    if (path == nullptr || (access != DirectAccess::kReadOnly && access != DirectAccess::kReadWrite))
      return std::unexpected(UringErrorCode::kInvalid);
    const int raw = ::open(path, (access == DirectAccess::kReadOnly ? O_RDONLY : O_RDWR) | O_DIRECT | O_CLOEXEC);
    if (raw < 0) return std::unexpected(LastError());
    auto owned = sys::UniqueFd::Adopt(raw);
    return FromOwnedFd(std::move(*owned));
  }
  [[nodiscard]] static Result<DirectFile> FromOwnedFd(sys::UniqueFd fd) noexcept {
    const int flags = ::fcntl(fd.get(), F_GETFL);
    if (flags < 0) return std::unexpected(LastError());
    if ((flags & O_DIRECT) == 0) return std::unexpected(UringErrorCode::kInvalid);
    StatxLayout info{};
    constexpr std::uint64_t at_empty_path = 0x1000;
    constexpr std::uint64_t statx_type_mode = 0x0001U | 0x0002U;
    auto status = ToResult(sys::Syscall6(sys::kSysStatx, static_cast<std::uint64_t>(fd.get()), sys::Address(""), at_empty_path,
                                         statx_type_mode | detail::kStatxDioAlign, sys::Address(&info)));
    if (!status) return std::unexpected(status.error());
    auto alignments = detail::ValidateDioAlignments(info);
    if (!alignments) return std::unexpected(alignments.error());
    return DirectFile{std::move(fd), info, *alignments};
  }
  [[nodiscard]] int Fd() const noexcept { return fd_.get(); }
  [[nodiscard]] std::uint32_t MemAlignment() const noexcept { return mem_align_; }
  [[nodiscard]] std::uint32_t OffsetAlignment() const noexcept { return offset_align_; }

  [[nodiscard]] Result<bool> SupportsPolledIo() const noexcept {
    const bool block = (mode_ & 0170000U) == 0060000U;
    const auto major = block ? rdev_major_ : dev_major_;
    const auto minor = block ? rdev_minor_ : dev_minor_;
    for (const char* suffix : {"/queue/io_poll", "/../queue/io_poll"}) {
      char path[128]{};
      const int length = std::snprintf(path, sizeof(path), "/sys/dev/block/%u:%u%s", major, minor, suffix);
      if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path)) return std::unexpected(UringErrorCode::kOverflow);
      const int raw = ::open(path, O_RDONLY | O_CLOEXEC);
      if (raw < 0) {
        if (errno == ENOENT || errno == ENOTDIR) continue;
        return std::unexpected(LastError());
      }
      auto fd = sys::UniqueFd::Adopt(raw);
      char flag{};
      ssize_t read_count;
      do {
        read_count = ::read(fd->get(), &flag, 1);
      } while (read_count < 0 && errno == EINTR);
      if (read_count < 0) return std::unexpected(LastError());
      if (read_count != 1 || (flag != '0' && flag != '1')) return std::unexpected(UringErrorCode::kInvalid);
      return flag == '1';
    }
    return false;
  }
  [[nodiscard]] Result<std::uint32_t> ValidateTransfer(const void* ptr, std::size_t len, std::uint64_t offset) const noexcept {
    if (ptr == nullptr || len == 0 || len > UINT32_MAX || (reinterpret_cast<std::uintptr_t>(ptr) & (mem_align_ - 1U)) != 0 ||
        (len & (offset_align_ - 1U)) != 0 || (offset & (offset_align_ - 1U)) != 0)
      return std::unexpected(UringErrorCode::kInvalid);
    return static_cast<std::uint32_t>(len);
  }
  [[nodiscard]] Result<ops::ReadFixed> PrepareReadFixed(resources::FixedBufferPool& pool, const resources::BufferHandle& buffer,
                                                        std::uint32_t len, std::uint64_t offset) const noexcept {
    if (len > buffer.Capacity()) return std::unexpected(UringErrorCode::kInvalid);
    auto valid = ValidateTransfer(buffer.Data(), len, offset);
    if (!valid) return std::unexpected(valid.error());
    auto submitted = pool.MarkInFlight(buffer);
    if (!submitted) return std::unexpected(submitted.error());
    return ops::PrepareReadFixed(Fd(), ops::UserAddress{buffer.Data()}, len, offset, buffer.Index());
  }
  [[nodiscard]] Result<ops::WriteFixed> PrepareWriteFixed(resources::FixedBufferPool& pool,
                                                          const resources::BufferHandle& buffer, std::uint32_t len,
                                                          std::uint64_t offset) const noexcept {
    if (len > buffer.Capacity()) return std::unexpected(UringErrorCode::kInvalid);
    auto valid = ValidateTransfer(buffer.Data(), len, offset);
    if (!valid) return std::unexpected(valid.error());
    auto submitted = pool.MarkInFlight(buffer);
    if (!submitted) return std::unexpected(submitted.error());
    return ops::PrepareWriteFixed(Fd(), ops::UserAddress{buffer.Data()}, len, offset, buffer.Index());
  }
};
}  // namespace liburing::io
