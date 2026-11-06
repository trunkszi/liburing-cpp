#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <liburing/ops/buffers.hpp>
#include <liburing/ops/core.hpp>
#include <liburing/ops/fs.hpp>
#include <liburing/ops/futex.hpp>
#include <liburing/ops/net.hpp>
#include <liburing/ops/uring_cmd.hpp>
#include <liburing/ops/xattr.hpp>
namespace ops_test {
using namespace liburing;
using namespace liburing::ops;
constexpr UserDataToken kTarget = UserDataToken::from_raw(0x7788);
using Coverage = std::array<bool, 65>;
template <OpDescriptor Op>
constexpr bool Check(const Op& op, abi::FlatSqe expected, OpKind kind = OpKind::kOneShot,
                     std::array<std::uint64_t, 8> extension = {}) {
  using Sqe = typename Op::Sqe;
  std::array<std::uint64_t, sizeof(Sqe) / 8> dirty{};
  dirty.fill(0xffffffffffffffffULL);
  auto actual = std::bit_cast<Sqe>(dirty);
  // Reservation initializes the engine-owned fields before descriptor encoding.
  if constexpr (Op::kSqeSlots == 1) {
    actual.flags = 0;
    actual.user_data = 0;
  } else {
    actual.sqe.flags = 0;
    actual.sqe.user_data = 0;
  }
  op.Encode(actual);
  auto required = Op::RequiredCaps();
  const auto opcode = static_cast<std::uint32_t>(expected.opcode);
  if (Op::kOpcode != opcode || Op::kKind != kind || Op::kSqeSlots != (opcode >= 63 ? 2U : 1U) ||
      Op::kRequiresCqe32 != (opcode == 58) || required.opcodes[0].count() + required.opcodes[1].count() != 1 ||
      !required.opcodes[opcode < 64 ? 0U : 1U].test(opcode < 64 ? opcode : 0))
    return false;
  if constexpr (Op::kSqeSlots == 1)
    return std::bit_cast<std::array<std::uint64_t, 8>>(actual) == std::bit_cast<std::array<std::uint64_t, 8>>(expected);
  else {
    if (std::bit_cast<std::array<std::uint64_t, 8>>(actual.sqe) != std::bit_cast<std::array<std::uint64_t, 8>>(expected))
      return false;
    for (std::size_t i = 0; i < 8; ++i)
      if (actual.extension[i] != extension[i]) return false;
    return true;
  }
}
template <OpDescriptor Op>
constexpr bool Cover(Coverage& seen, const Op& op, abi::FlatSqe expected, OpKind kind = OpKind::kOneShot,
                     std::array<std::uint64_t, 8> extension = {}) {
  if (!Check(op, expected, kind, extension) || expected.opcode >= seen.size() || seen[expected.opcode]) return false;
  seen[expected.opcode] = true;
  return true;
}
constexpr bool CoreCases(Coverage& seen) {
  return Cover(seen, PrepareNop(), {.opcode = 0, .fd = -1}) &&
         Cover(seen, PrepareTimeout(0x101, 7, 8),
               {.opcode = 11, .fd = -1, .offset = 7, .address = 0x101, .len = 1, .operation_flags = 8}) &&
         Cover(seen, PrepareTimeoutRemove(kTarget, 9), {.opcode = 12, .fd = -1, .address = 0x7788, .operation_flags = 9}) &&
         Cover(seen, PrepareAsyncCancel(kTarget, 10), {.opcode = 14, .fd = -1, .address = 0x7788, .operation_flags = 10}) &&
         Cover(seen, PrepareLinkTimeout(0x102, 11),
               {.opcode = 15, .fd = -1, .address = 0x102, .len = 1, .operation_flags = 11}) &&
         Cover(seen, PrepareEpollCtl(12, 13, 14, 0x103), {.opcode = 29, .fd = 12, .offset = 13, .address = 0x103, .len = 14}) &&
         Cover(seen, PrepareMsgRing(15, 16, 0x104, 17),
               {.opcode = 40, .fd = 15, .offset = 0x104, .len = 16, .operation_flags = 17}) &&
         Cover(seen, PrepareWaitid(18, 19, 0x105, 20, 21),
               {.opcode = 50, .fd = 19, .offset = 0x105, .len = 18, .operation_flags = 21, .file_or_splice = 20}) &&
         Cover(seen, PrepareFixedFdInstall(22, 23), {.opcode = 54, .flags = 1, .fd = 22, .operation_flags = 23}) &&
         Cover(seen, PrepareEpollWait(24, 0x106, 25, 26),
               {.opcode = 59, .fd = 24, .address = 0x106, .len = 25, .operation_flags = 26}) &&
         Cover(seen, PreparePipe(0x107, 27), {.opcode = 62, .fd = 0, .address = 0x107, .operation_flags = 27}) &&
         Cover(seen, PrepareNop128(), {.opcode = 63, .fd = -1});
}
constexpr bool FsCases(Coverage& seen) {
  return Cover(seen, PrepareReadv(3, 0x101, 4, 0x102, 5),
               {.opcode = 1, .fd = 3, .offset = 0x102, .address = 0x101, .len = 4, .operation_flags = 5}) &&
         Cover(seen, PrepareWritev(6, 0x103, 7, 0x104, 8),
               {.opcode = 2, .fd = 6, .offset = 0x104, .address = 0x103, .len = 7, .operation_flags = 8}) &&
         Cover(seen, PrepareFsync(9, 10), {.opcode = 3, .fd = 9, .operation_flags = 10}) &&
         Cover(seen, PrepareReadFixed(11, 0x105, 12, 0x106, 13, 14),
               {.opcode = 4,
                .fd = 11,
                .offset = 0x106,
                .address = 0x105,
                .len = 12,
                .operation_flags = 14,
                .buffer_index = 13}) &&
         Cover(seen, PrepareWriteFixed(15, 0x107, 16, 0x108, 17, 18),
               {.opcode = 5,
                .fd = 15,
                .offset = 0x108,
                .address = 0x107,
                .len = 16,
                .operation_flags = 18,
                .buffer_index = 17}) &&
         Cover(seen, PrepareSyncFileRange(19, 20, 0x109, 21),
               {.opcode = 8, .fd = 19, .offset = 0x109, .len = 20, .operation_flags = 21}) &&
         Cover(seen, PrepareFallocate(22, 23, 0x110, 0x111),
               {.opcode = 17, .fd = 22, .offset = 0x110, .address = 0x111, .len = 23}) &&
         Cover(seen, PrepareOpenat(-100, 0x112, 24, 25),
               {.opcode = 18, .fd = -100, .address = 0x112, .len = 25, .operation_flags = 24}) &&
         Cover(seen, PrepareClose(26), {.opcode = 19, .fd = 26}) &&
         Cover(seen, PrepareFilesUpdate(0x113, 27, 28), {.opcode = 20, .fd = -1, .offset = 28, .address = 0x113, .len = 27}) &&
         Cover(seen, PrepareStatx(-100, 0x114, 29, 30, 0x115),
               {.opcode = 21, .fd = -100, .offset = 0x115, .address = 0x114, .len = 30, .operation_flags = 29}) &&
         Cover(seen, PrepareRead(31, 0x116, 32, 0x117, 33),
               {.opcode = 22, .fd = 31, .offset = 0x117, .address = 0x116, .len = 32, .operation_flags = 33}) &&
         Cover(seen, PrepareWrite(34, 0x118, 35, 0x119, 36),
               {.opcode = 23, .fd = 34, .offset = 0x119, .address = 0x118, .len = 35, .operation_flags = 36}) &&
         Cover(seen, PrepareFadvise(37, 0x120, 38, 39),
               {.opcode = 24, .fd = 37, .offset = 0x120, .len = 38, .operation_flags = 39}) &&
         Cover(seen, PrepareMadvise(0x121, 40, 41),
               {.opcode = 25, .fd = -1, .address = 0x121, .len = 40, .operation_flags = 41}) &&
         Cover(seen, PrepareOpenat2(-100, 0x122, 0x123),
               {.opcode = 28, .fd = -100, .offset = 0x123, .address = 0x122, .len = 24}) &&
         Cover(seen, PrepareSplice(-42, -1, 43, 0x124, 44, 45),
               {.opcode = 30,
                .fd = 43,
                .offset = 0x124,
                .address = 0xffffffffffffffffULL,
                .len = 44,
                .operation_flags = 45,
                .file_or_splice = 0xffffffd6U}) &&
         Cover(seen, PrepareTee(46, 47, 48, 49),
               {.opcode = 33, .fd = 47, .len = 48, .operation_flags = 49, .file_or_splice = 46}) &&
         Cover(seen, PrepareRenameat(-100, 0x125, -101, 0x126, 50),
               {.opcode = 35, .fd = -100, .offset = 0x126, .address = 0x125, .len = 0xffffff9bU, .operation_flags = 50}) &&
         Cover(seen, PrepareUnlinkat(-100, 0x127, 51), {.opcode = 36, .fd = -100, .address = 0x127, .operation_flags = 51}) &&
         Cover(seen, PrepareMkdirat(-100, 0x128, 52), {.opcode = 37, .fd = -100, .address = 0x128, .len = 52}) &&
         Cover(seen, PrepareSymlinkat(0x129, -100, 0x130), {.opcode = 38, .fd = -100, .offset = 0x130, .address = 0x129}) &&
         Cover(seen, PrepareLinkat(-100, 0x131, -101, 0x132, 53),
               {.opcode = 39, .fd = -100, .offset = 0x132, .address = 0x131, .len = 0xffffff9bU, .operation_flags = 53}) &&
         Cover(seen, PrepareReadMultishot(54, 55, 0x133, 56),
               {.opcode = 49, .flags = 32, .fd = 54, .offset = 0x133, .len = 55, .buffer_index = 56}, OpKind::kMultishot) &&
         Cover(seen, PrepareFtruncate(57, 0x134), {.opcode = 55, .fd = 57, .offset = 0x134}) &&
         Cover(seen, PrepareReadvFixed(58, 0x135, 59, 0x136, 60, 61),
               {.opcode = 60,
                .fd = 58,
                .offset = 0x136,
                .address = 0x135,
                .len = 59,
                .operation_flags = 61,
                .buffer_index = 60}) &&
         Cover(
             seen, PrepareWritevFixed(62, 0x137, 63, 0x138, 64, 65),
             {.opcode = 61, .fd = 62, .offset = 0x138, .address = 0x137, .len = 63, .operation_flags = 65, .buffer_index = 64});
}
constexpr bool NetCases(Coverage& seen) {
  return Cover(seen, PreparePollAdd(3, 0x101), {.opcode = 6, .fd = 3, .operation_flags = 0x101}) &&
         Cover(seen, PreparePollRemove(kTarget), {.opcode = 7, .fd = -1, .address = 0x7788}) &&
         Cover(seen, PrepareSendmsg(4, 0x102, 5), {.opcode = 9, .fd = 4, .address = 0x102, .len = 1, .operation_flags = 5}) &&
         Cover(seen, PrepareRecvmsg(6, 0x103, 7), {.opcode = 10, .fd = 6, .address = 0x103, .len = 1, .operation_flags = 7}) &&
         Cover(seen, PrepareAccept(8, 0x104, 0x105, 9),
               {.opcode = 13, .fd = 8, .offset = 0x105, .address = 0x104, .operation_flags = 9}) &&
         Cover(seen, PrepareConnect(10, 0x106, 11), {.opcode = 16, .fd = 10, .offset = 11, .address = 0x106}) &&
         Cover(seen, PrepareSend(12, 0x107, 13, 14),
               {.opcode = 26, .fd = 12, .address = 0x107, .len = 13, .operation_flags = 14}) &&
         Cover(seen, PrepareRecv(15, 0x108, 16, 17),
               {.opcode = 27, .fd = 15, .address = 0x108, .len = 16, .operation_flags = 17}) &&
         Cover(seen, PrepareShutdown(18, 19), {.opcode = 34, .fd = 18, .len = 19}) &&
         Cover(seen, PrepareSocket(20, 21, 22, 23), {.opcode = 45, .fd = 20, .offset = 21, .len = 22, .operation_flags = 23}) &&
         Cover(seen, PrepareSendZc(24, 0x109, 25, 26, 27),
               {.opcode = 47, .ioprio = 27, .fd = 24, .address = 0x109, .len = 25, .operation_flags = 26}, OpKind::kZeroCopy) &&
         Cover(seen, PrepareSendmsgZc(28, 0x110, 29, 30),
               {.opcode = 48, .ioprio = 30, .fd = 28, .address = 0x110, .len = 1, .operation_flags = 29}, OpKind::kZeroCopy) &&
         Cover(seen, PrepareBind(31, 0x111, 32), {.opcode = 56, .fd = 31, .offset = 32, .address = 0x111}) &&
         Cover(seen, PrepareListen(33, 34), {.opcode = 57, .fd = 33, .len = 34}) &&
         Cover(seen, PrepareRecvZc(35, 36, 37), {.opcode = 58, .ioprio = 2, .fd = 35, .len = 36, .file_or_splice = 37},
               OpKind::kMultishot);
}
constexpr bool OtherCases(Coverage& seen) {
  if (!Cover(seen, PrepareFsetxattr(3, 0x101, 0x102, 4, 5),
             {.opcode = 41, .fd = 3, .offset = 0x102, .address = 0x101, .len = 4, .operation_flags = 5}) ||
      !Cover(seen, PrepareSetxattr(0x103, 0x104, 0x105, 6, 7),
             {.opcode = 42, .fd = 0, .offset = 0x104, .address = 0x103, .len = 6, .operation_flags = 7, .address3 = 0x105}) ||
      !Cover(seen, PrepareFgetxattr(8, 0x106, 0x107, 9),
             {.opcode = 43, .fd = 8, .offset = 0x107, .address = 0x106, .len = 9}) ||
      !Cover(seen, PrepareGetxattr(0x108, 0x109, 0x110, 10),
             {.opcode = 44, .fd = 0, .offset = 0x109, .address = 0x108, .len = 10, .address3 = 0x110}) ||
      !Cover(seen, PrepareFutexWait(UserAddress{0x111}, 11, 12),
             {.opcode = 51, .fd = 2, .offset = 11, .address = 0x111, .address3 = 12}) ||
      !Cover(seen, PrepareFutexWake(UserAddress{0x112}, 13, 14),
             {.opcode = 52, .fd = 2, .offset = 13, .address = 0x112, .address3 = 14}) ||
      !Cover(seen, PrepareFutexWaitv(0x113, 15), {.opcode = 53, .fd = 0, .address = 0x113, .len = 15}) ||
      !Cover(seen, PrepareProvideBuffers(0x114, 16, 17, 18, 19),
             {.opcode = 31, .fd = 17, .offset = 19, .address = 0x114, .len = 16, .buffer_index = 18}) ||
      !Cover(seen, PrepareRemoveBuffers(20, 21), {.opcode = 32, .fd = 20, .buffer_index = 21}))
    return false;
  std::array<std::byte, 16> small{};
  std::array<std::byte, 80> large{};
  for (std::size_t i = 0; i < small.size(); ++i) small[i] = static_cast<std::byte>(i + 1);
  for (std::size_t i = 0; i < large.size(); ++i) large[i] = static_cast<std::byte>(i + 1);
  return Cover(seen, PrepareUringCmd(22, 23, small, 24),
               {.opcode = 46,
                .fd = 22,
                .offset = 23,
                .operation_flags = 24,
                .address3 = 0x0807060504030201ULL,
                .extra = 0x100f0e0d0c0b0a09ULL}) &&
         Cover(seen, PrepareUringCmd128(25, 26, large, 27),
               {.opcode = 64,
                .fd = 25,
                .offset = 26,
                .operation_flags = 27,
                .address3 = 0x0807060504030201ULL,
                .extra = 0x100f0e0d0c0b0a09ULL},
               OpKind::kOneShot,
               {0x1817161514131211ULL, 0x201f1e1d1c1b1a19ULL, 0x2827262524232221ULL, 0x302f2e2d2c2b2a29ULL,
                0x3837363534333231ULL, 0x403f3e3d3c3b3a39ULL, 0x4847464544434241ULL, 0x504f4e4d4c4b4a49ULL});
}
constexpr bool AllEncodings() {
  Coverage seen{};
  if (!CoreCases(seen) || !FsCases(seen) || !NetCases(seen) || !OtherCases(seen)) return false;
  return std::ranges::all_of(seen, [](bool present) { return present; });
}
constexpr bool Variants() {
  return Check(PrepareAccept(2, nullptr, nullptr), {.opcode = 13, .fd = 2}) &&
         Check(PrepareFutexWait<abi::kFutex2SizeU32 | abi::kFutex2Numa | abi::kFutex2Mpol | abi::kFutex2Private>(
                   UserAddress{0x1000}, 0x12345678U, 0xffffffffU),
               {.opcode = 51, .fd = 142, .offset = 0x12345678U, .address = 0x1000, .address3 = 0xffffffffU}) &&
         Check(PrepareFutexWake<abi::kFutex2SizeU32 | abi::kFutex2Private>(UserAddress{0x2000}, 7, 8),
               {.opcode = 52, .fd = 130, .offset = 7, .address = 0x2000, .address3 = 8}) &&
         Check(PreparePollMultishot(3, 7), {.opcode = 6, .fd = 3, .len = 1, .operation_flags = 7}, OpKind::kMultishot) &&
         Check(PrepareRecvMultishot(4, 0x101, 0, 5, 6),
               {.opcode = 27, .flags = 32, .ioprio = 2, .fd = 4, .address = 0x101, .operation_flags = 5, .buffer_index = 6},
               OpKind::kMultishot) &&
         Check(PrepareRecvmsgMultishot(7, 0x102, 8, 9),
               {.opcode = 10,
                .flags = 32,
                .ioprio = 2,
                .fd = 7,
                .address = 0x102,
                .len = 1,
                .operation_flags = 8,
                .buffer_index = 9},
               OpKind::kMultishot) &&
         Check(PrepareAcceptMultishot(10, 0x103, 0x104, 11),
               {.opcode = 13, .ioprio = 1, .fd = 10, .offset = 0x104, .address = 0x103, .operation_flags = 11},
               OpKind::kMultishot) &&
         Check(PrepareNop().WithToken(kTarget).WithSqeFlags<abi::kSqeAsync>(),
               {.opcode = 0, .flags = 16, .fd = -1, .user_data = 0x7788}) &&
         Check(PreparePadNop(), {.opcode = 0, .flags = 64, .fd = -1}) &&
         Check(PrepareSkipSuccessNop(), {.opcode = 0, .flags = 64, .fd = -1});
}
}  // namespace ops_test
