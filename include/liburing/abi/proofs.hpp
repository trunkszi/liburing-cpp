#pragma once
#include <array>
#include <cstddef>
#include <liburing/abi/uapi_futex.hpp>
#include <liburing/abi/uapi_io_uring.hpp>
#include <meta>
#include <uarch/uarch.hpp>
namespace liburing::abi {
template <class T, std::size_t N>
consteval bool MatchOffsets(const std::array<std::size_t, N>& expected) {
  static constexpr auto members =
      std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
  if (members.size() != N) return false;
  std::size_t i = 0;
  template for (constexpr auto member : members) {
    if (uarch::reflect::byte_offset(member) != expected[i++]) return false;
  }
  return true;
}
consteval bool CheckLayouts() {
  bool ok = true;
  static_assert(uarch::NoPadding<AbsoluteDeadline> && sizeof(AbsoluteDeadline) == 24);
  ok = MatchOffsets<AbsoluteDeadline>(std::array<std::size_t, 3>{0, 16, 20}) && ok;
  static_assert(uarch::NoPadding<FlatSqe> && sizeof(FlatSqe) == 64);
  ok = MatchOffsets<FlatSqe>(std::array<std::size_t, 14>{0, 1, 2, 4, 8, 16, 24, 28, 32, 40, 42, 44, 48, 56}) && ok;
  static_assert(uarch::NoPadding<FlatSqe128> && sizeof(FlatSqe128) == 128);
  ok = MatchOffsets<FlatSqe128>(std::array<std::size_t, 2>{0, 64}) && ok;
  static_assert(uarch::NoPadding<FlatCqe> && sizeof(FlatCqe) == 16);
  ok = MatchOffsets<FlatCqe>(std::array<std::size_t, 3>{0, 8, 12}) && ok;
  static_assert(uarch::NoPadding<FlatCqe32> && sizeof(FlatCqe32) == 32);
  ok = MatchOffsets<FlatCqe32>(std::array<std::size_t, 2>{0, 16}) && ok;
  static_assert(uarch::NoPadding<IoSqringOffsets> && sizeof(IoSqringOffsets) == 40);
  ok = MatchOffsets<IoSqringOffsets>(std::array<std::size_t, 9>{0, 4, 8, 12, 16, 20, 24, 28, 32}) && ok;
  static_assert(uarch::NoPadding<IoCqringOffsets> && sizeof(IoCqringOffsets) == 40);
  ok = MatchOffsets<IoCqringOffsets>(std::array<std::size_t, 9>{0, 4, 8, 12, 16, 20, 24, 28, 32}) && ok;
  static_assert(uarch::NoPadding<IoUringParams> && sizeof(IoUringParams) == 120);
  ok = MatchOffsets<IoUringParams>(std::array<std::size_t, 10>{0, 4, 8, 12, 16, 20, 24, 28, 40, 80}) && ok;
  static_assert(uarch::NoPadding<IoUringRsrcRegister> && sizeof(IoUringRsrcRegister) == 32);
  ok = MatchOffsets<IoUringRsrcRegister>(std::array<std::size_t, 5>{0, 4, 8, 16, 24}) && ok;
  static_assert(uarch::NoPadding<IoUringRsrcUpdate2> && sizeof(IoUringRsrcUpdate2) == 32);
  ok = MatchOffsets<IoUringRsrcUpdate2>(std::array<std::size_t, 6>{0, 4, 8, 16, 24, 28}) && ok;
  static_assert(uarch::NoPadding<IoUringRsrcUpdate> && sizeof(IoUringRsrcUpdate) == 16);
  ok = MatchOffsets<IoUringRsrcUpdate>(std::array<std::size_t, 3>{0, 4, 8}) && ok;
  static_assert(uarch::NoPadding<IoUringBuf> && sizeof(IoUringBuf) == 16);
  ok = MatchOffsets<IoUringBuf>(std::array<std::size_t, 4>{0, 8, 12, 14}) && ok;
  static_assert(uarch::NoPadding<IoUringBufRing> && sizeof(IoUringBufRing) == 16);
  ok = MatchOffsets<IoUringBufRing>(std::array<std::size_t, 4>{0, 8, 12, 14}) && ok;
  static_assert(uarch::NoPadding<IoUringBufReg> && sizeof(IoUringBufReg) == 40);
  ok = MatchOffsets<IoUringBufReg>(std::array<std::size_t, 6>{0, 8, 12, 14, 16, 20}) && ok;
  static_assert(uarch::NoPadding<IoUringGeteventsArg> && sizeof(IoUringGeteventsArg) == 24);
  ok = MatchOffsets<IoUringGeteventsArg>(std::array<std::size_t, 4>{0, 8, 12, 16}) && ok;
  static_assert(uarch::NoPadding<IoUringSyncCancelReg> && sizeof(IoUringSyncCancelReg) == 64);
  ok = MatchOffsets<IoUringSyncCancelReg>(std::array<std::size_t, 7>{0, 8, 12, 16, 32, 33, 40}) && ok;
  static_assert(uarch::NoPadding<IoUringProbeOp> && sizeof(IoUringProbeOp) == 8);
  ok = MatchOffsets<IoUringProbeOp>(std::array<std::size_t, 4>{0, 1, 2, 4}) && ok;
  static_assert(uarch::NoPadding<IoUringProbe> && sizeof(IoUringProbe) == 16);
  ok = MatchOffsets<IoUringProbe>(std::array<std::size_t, 4>{0, 1, 2, 4}) && ok;
  static_assert(uarch::NoPadding<IoUringZcrxRqe> && sizeof(IoUringZcrxRqe) == 16 && uarch::GprPair<IoUringZcrxRqe>);
  ok = MatchOffsets<IoUringZcrxRqe>(std::array<std::size_t, 3>{0, 8, 12}) && ok;
  static_assert(uarch::NoPadding<IoUringZcrxOffsets> && sizeof(IoUringZcrxOffsets) == 32);
  ok = MatchOffsets<IoUringZcrxOffsets>(std::array<std::size_t, 5>{0, 4, 8, 12, 16}) && ok;
  static_assert(uarch::NoPadding<IoUringZcrxIfqReg> && sizeof(IoUringZcrxIfqReg) == 96);
  ok = MatchOffsets<IoUringZcrxIfqReg>(std::array<std::size_t, 10>{0, 4, 8, 12, 16, 24, 32, 64, 68, 72}) && ok;
  static_assert(uarch::NoPadding<IoUringRegionDesc> && sizeof(IoUringRegionDesc) == 64);
  ok = MatchOffsets<IoUringRegionDesc>(std::array<std::size_t, 6>{0, 8, 16, 20, 24, 32}) && ok;
  static_assert(uarch::NoPadding<IoUringMemRegionReg> && sizeof(IoUringMemRegionReg) == 32);
  ok = MatchOffsets<IoUringMemRegionReg>(std::array<std::size_t, 3>{0, 8, 16}) && ok;
  static_assert(uarch::NoPadding<IoUringZcrxAreaReg> && sizeof(IoUringZcrxAreaReg) == 48);
  ok = MatchOffsets<IoUringZcrxAreaReg>(std::array<std::size_t, 6>{0, 8, 16, 24, 28, 32}) && ok;
  static_assert(uarch::NoPadding<IoUringNapi> && sizeof(IoUringNapi) == 16);
  ok = MatchOffsets<IoUringNapi>(std::array<std::size_t, 6>{0, 4, 5, 6, 8, 12}) && ok;
  static_assert(uarch::NoPadding<IoUringFileIndexRange> && sizeof(IoUringFileIndexRange) == 16);
  ok = MatchOffsets<IoUringFileIndexRange>(std::array<std::size_t, 3>{0, 4, 8}) && ok;
  static_assert(uarch::NoPadding<IoUringCloneBuffers> && sizeof(IoUringCloneBuffers) == 32);
  ok = MatchOffsets<IoUringCloneBuffers>(std::array<std::size_t, 6>{0, 4, 8, 12, 16, 20}) && ok;
  static_assert(uarch::NoPadding<IoUringClockRegister> && sizeof(IoUringClockRegister) == 16);
  ok = MatchOffsets<IoUringClockRegister>(std::array<std::size_t, 2>{0, 4}) && ok;
  static_assert(uarch::NoPadding<IoUringRestriction> && sizeof(IoUringRestriction) == 16);
  ok = MatchOffsets<IoUringRestriction>(std::array<std::size_t, 4>{0, 2, 3, 4}) && ok;
  static_assert(uarch::NoPadding<IoUringBufStatus> && sizeof(IoUringBufStatus) == 40);
  ok = MatchOffsets<IoUringBufStatus>(std::array<std::size_t, 3>{0, 4, 8}) && ok;
  static_assert(uarch::NoPadding<IoUringQueryHdr> && sizeof(IoUringQueryHdr) == 40);
  ok = MatchOffsets<IoUringQueryHdr>(std::array<std::size_t, 6>{0, 8, 16, 20, 24, 28}) && ok;
  static_assert(uarch::NoPadding<IoUringQueryOpcode> && sizeof(IoUringQueryOpcode) == 48);
  ok = MatchOffsets<IoUringQueryOpcode>(std::array<std::size_t, 8>{0, 4, 8, 16, 24, 32, 40, 44}) && ok;
  static_assert(uarch::NoPadding<FutexWaitv> && sizeof(FutexWaitv) == 24);
  ok = MatchOffsets<FutexWaitv>(std::array<std::size_t, 4>{0, 8, 16, 20}) && ok;
  static_assert(uarch::NoPadding<KernelTimespec> && sizeof(KernelTimespec) == 16);
  ok = MatchOffsets<KernelTimespec>(std::array<std::size_t, 2>{0, 8}) && ok;
  static_assert(uarch::RegisterPassable<FlatCqe> && uarch::GprPair<FlatCqe>);
  static_assert(uarch::RegisterPassable<IoUringBuf> && uarch::GprPair<IoUringBuf>);
  static_assert(uarch::RegisterPassable<KernelTimespec> && uarch::GprPair<KernelTimespec>);
  return ok;
}
static_assert(CheckLayouts());
}  // namespace liburing::abi
