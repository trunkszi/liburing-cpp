/** @file @brief File operations retain explicit offsets and kernel-owned buffer identities. */
#pragma once
#include <liburing/ops/core.hpp>
namespace liburing::ops {
struct OpenHow {
  std::uint64_t flags;
  std::uint64_t mode;
  std::uint64_t resolve;
};
static_assert(sizeof(OpenHow) == 24 && uarch::NoPadding<OpenHow>);
using Readv = detail::Operation<abi::kOpReadv>;
using Writev = detail::Operation<abi::kOpWritev>;
using Fsync = detail::Operation<abi::kOpFsync>;
using ReadFixed = detail::Operation<abi::kOpReadFixed>;
using WriteFixed = detail::Operation<abi::kOpWriteFixed>;
using SyncFileRange = detail::Operation<abi::kOpSyncFileRange>;
using Fallocate = detail::Operation<abi::kOpFallocate>;
using Openat = detail::Operation<abi::kOpOpenat>;
using Close = detail::Operation<abi::kOpClose>;
using FilesUpdate = detail::Operation<abi::kOpFilesUpdate>;
using Statx = detail::Operation<abi::kOpStatx>;
using Read = detail::Operation<abi::kOpRead>;
using Write = detail::Operation<abi::kOpWrite>;
using Fadvise = detail::Operation<abi::kOpFadvise>;
using Madvise = detail::Operation<abi::kOpMadvise>;
using Openat2 = detail::Operation<abi::kOpOpenat2>;
using Splice = detail::Operation<abi::kOpSplice>;
using Tee = detail::Operation<abi::kOpTee>;
using Renameat = detail::Operation<abi::kOpRenameat>;
using Unlinkat = detail::Operation<abi::kOpUnlinkat>;
using Mkdirat = detail::Operation<abi::kOpMkdirat>;
using Symlinkat = detail::Operation<abi::kOpSymlinkat>;
using Linkat = detail::Operation<abi::kOpLinkat>;
using ReadMultishot = detail::Operation<abi::kOpReadMultishot, OpKind::kMultishot>;
using Ftruncate = detail::Operation<abi::kOpFtruncate>;
using ReadvFixed = detail::Operation<abi::kOpReadvFixed>;
using WritevFixed = detail::Operation<abi::kOpWritevFixed>;
[[nodiscard]] constexpr Readv PrepareReadv(std::int32_t fd, UserAddress vectors, std::uint32_t count, std::uint64_t offset = 0,
                                           std::uint32_t flags = 0) noexcept {
  return Readv{{.fd = fd, .offset = offset, .address = vectors.Raw(), .len = count, .operation_flags = flags}};
}
[[nodiscard]] constexpr Writev PrepareWritev(std::int32_t fd, UserAddress vectors, std::uint32_t count,
                                             std::uint64_t offset = 0, std::uint32_t flags = 0) noexcept {
  return Writev{{.fd = fd, .offset = offset, .address = vectors.Raw(), .len = count, .operation_flags = flags}};
}
[[nodiscard]] constexpr Fsync PrepareFsync(std::int32_t fd, std::uint32_t flags = 0) noexcept {
  return Fsync{{.fd = fd, .operation_flags = flags}};
}
[[nodiscard]] constexpr ReadFixed PrepareReadFixed(std::int32_t fd, UserAddress buffer, std::uint32_t length,
                                                   std::uint64_t offset, std::uint16_t index,
                                                   std::uint32_t flags = 0) noexcept {
  return ReadFixed{
      {.fd = fd, .offset = offset, .address = buffer.Raw(), .len = length, .operation_flags = flags, .buffer_index = index}};
}
[[nodiscard]] constexpr WriteFixed PrepareWriteFixed(std::int32_t fd, UserAddress buffer, std::uint32_t length,
                                                     std::uint64_t offset, std::uint16_t index,
                                                     std::uint32_t flags = 0) noexcept {
  return WriteFixed{
      {.fd = fd, .offset = offset, .address = buffer.Raw(), .len = length, .operation_flags = flags, .buffer_index = index}};
}
[[nodiscard]] constexpr SyncFileRange PrepareSyncFileRange(std::int32_t fd, std::uint32_t length, std::uint64_t offset,
                                                           std::uint32_t flags = 0) noexcept {
  return SyncFileRange{{.fd = fd, .offset = offset, .len = length, .operation_flags = flags}};
}
[[nodiscard]] constexpr Fallocate PrepareFallocate(std::int32_t fd, std::uint32_t mode, std::uint64_t offset,
                                                   std::uint64_t length) noexcept {
  return Fallocate{{.fd = fd, .offset = offset, .address = length, .len = mode}};
}
[[nodiscard]] constexpr Openat PrepareOpenat(std::int32_t directory, UserAddress path, std::uint32_t flags,
                                             std::uint32_t mode = 0) noexcept {
  return Openat{{.fd = directory, .address = path.Raw(), .len = mode, .operation_flags = flags}};
}
[[nodiscard]] constexpr Openat PrepareOpenatDirect(std::int32_t directory, UserAddress path, std::uint32_t flags,
                                                   std::uint32_t mode, std::uint32_t file_index) noexcept {
  return Openat{{.fd = directory,
                 .address = path.Raw(),
                 .len = mode,
                 .operation_flags = flags,
                 .file_or_splice = detail::EncodeDirectFileIndex(file_index)}};
}
[[nodiscard]] constexpr Close PrepareClose(std::int32_t fd) noexcept { return Close{{.fd = fd}}; }
[[nodiscard]] constexpr FilesUpdate PrepareFilesUpdate(UserAddress fds, std::uint32_t count, std::uint32_t offset) noexcept {
  return FilesUpdate{{.fd = -1, .offset = offset, .address = fds.Raw(), .len = count}};
}
[[nodiscard]] constexpr Statx PrepareStatx(std::int32_t directory, UserAddress path, std::uint32_t flags, std::uint32_t mask,
                                           UserAddress output) noexcept {
  return Statx{{.fd = directory, .offset = output.Raw(), .address = path.Raw(), .len = mask, .operation_flags = flags}};
}
[[nodiscard]] constexpr Read PrepareRead(std::int32_t fd, UserAddress buffer, std::uint32_t length, std::uint64_t offset = 0,
                                         std::uint32_t flags = 0) noexcept {
  return Read{{.fd = fd, .offset = offset, .address = buffer.Raw(), .len = length, .operation_flags = flags}};
}
[[nodiscard]] constexpr Write PrepareWrite(std::int32_t fd, UserAddress buffer, std::uint32_t length, std::uint64_t offset = 0,
                                           std::uint32_t flags = 0) noexcept {
  return Write{{.fd = fd, .offset = offset, .address = buffer.Raw(), .len = length, .operation_flags = flags}};
}
[[nodiscard]] constexpr Fadvise PrepareFadvise(std::int32_t fd, std::uint64_t offset, std::uint32_t length,
                                               std::uint32_t advice) noexcept {
  return Fadvise{{.fd = fd, .offset = offset, .len = length, .operation_flags = advice}};
}
[[nodiscard]] constexpr Madvise PrepareMadvise(UserAddress address, std::uint32_t length, std::uint32_t advice) noexcept {
  return Madvise{{.fd = -1, .address = address.Raw(), .len = length, .operation_flags = advice}};
}
[[nodiscard]] constexpr Openat2 PrepareOpenat2(std::int32_t directory, UserAddress path, UserAddress how,
                                               std::uint32_t size = sizeof(OpenHow)) noexcept {
  return Openat2{{.fd = directory, .offset = how.Raw(), .address = path.Raw(), .len = size}};
}
[[nodiscard]] constexpr Splice PrepareSplice(std::int32_t input, std::int64_t input_offset, std::int32_t output,
                                             std::int64_t output_offset, std::uint32_t length,
                                             std::uint32_t flags = 0) noexcept {
  return Splice{{.fd = output,
                 .offset = std::bit_cast<std::uint64_t>(output_offset),
                 .address = std::bit_cast<std::uint64_t>(input_offset),
                 .len = length,
                 .operation_flags = flags,
                 .file_or_splice = std::bit_cast<std::uint32_t>(input)}};
}
[[nodiscard]] constexpr Tee PrepareTee(std::int32_t input, std::int32_t output, std::uint32_t length,
                                       std::uint32_t flags = 0) noexcept {
  return Tee{{.fd = output, .len = length, .operation_flags = flags, .file_or_splice = std::bit_cast<std::uint32_t>(input)}};
}
[[nodiscard]] constexpr Renameat PrepareRenameat(std::int32_t old_directory, UserAddress old_path, std::int32_t new_directory,
                                                 UserAddress new_path, std::uint32_t flags = 0) noexcept {
  return Renameat{{.fd = old_directory,
                   .offset = new_path.Raw(),
                   .address = old_path.Raw(),
                   .len = static_cast<std::uint32_t>(new_directory),
                   .operation_flags = flags}};
}
[[nodiscard]] constexpr Unlinkat PrepareUnlinkat(std::int32_t directory, UserAddress path, std::uint32_t flags = 0) noexcept {
  return Unlinkat{{.fd = directory, .address = path.Raw(), .operation_flags = flags}};
}
[[nodiscard]] constexpr Mkdirat PrepareMkdirat(std::int32_t directory, UserAddress path, std::uint32_t mode) noexcept {
  return Mkdirat{{.fd = directory, .address = path.Raw(), .len = mode}};
}
[[nodiscard]] constexpr Symlinkat PrepareSymlinkat(UserAddress target, std::int32_t directory, UserAddress path) noexcept {
  return Symlinkat{{.fd = directory, .offset = path.Raw(), .address = target.Raw()}};
}
[[nodiscard]] constexpr Linkat PrepareLinkat(std::int32_t old_directory, UserAddress old_path, std::int32_t new_directory,
                                             UserAddress new_path, std::uint32_t flags = 0) noexcept {
  return Linkat{{.fd = old_directory,
                 .offset = new_path.Raw(),
                 .address = old_path.Raw(),
                 .len = static_cast<std::uint32_t>(new_directory),
                 .operation_flags = flags}};
}
[[nodiscard]] constexpr ReadMultishot PrepareReadMultishot(std::int32_t fd, std::uint32_t length, std::uint64_t offset,
                                                           std::uint16_t group) noexcept {
  return ReadMultishot{{.fd = fd, .offset = offset, .len = length, .buffer_index = group}}
      .WithSqeFlags<abi::kSqeBufferSelect>();
}
[[nodiscard]] constexpr Ftruncate PrepareFtruncate(std::int32_t fd, std::uint64_t length) noexcept {
  return Ftruncate{{.fd = fd, .offset = length}};
}
[[nodiscard]] constexpr ReadvFixed PrepareReadvFixed(std::int32_t fd, UserAddress vectors, std::uint32_t count,
                                                     std::uint64_t offset, std::uint16_t index,
                                                     std::uint32_t flags = 0) noexcept {
  return ReadvFixed{
      {.fd = fd, .offset = offset, .address = vectors.Raw(), .len = count, .operation_flags = flags, .buffer_index = index}};
}
[[nodiscard]] constexpr WritevFixed PrepareWritevFixed(std::int32_t fd, UserAddress vectors, std::uint32_t count,
                                                       std::uint64_t offset, std::uint16_t index,
                                                       std::uint32_t flags = 0) noexcept {
  return WritevFixed{
      {.fd = fd, .offset = offset, .address = vectors.Raw(), .len = count, .operation_flags = flags, .buffer_index = index}};
}
}  // namespace liburing::ops
