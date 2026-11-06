// SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT
// Linux 7.0 UAPI constants and flat descriptor representations.
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <liburing/abi/time.hpp>
#include <span>
namespace liburing::abi {
static_assert(std::endian::native == std::endian::little, "flat SQE narrow aliases require little endian");
inline constexpr std::uint32_t kSqeFixedFileBit = 0;
inline constexpr std::uint32_t kSqeIoDrainBit = kSqeFixedFileBit + 1;
inline constexpr std::uint32_t kSqeIoLinkBit = kSqeIoDrainBit + 1;
inline constexpr std::uint32_t kSqeIoHardlinkBit = kSqeIoLinkBit + 1;
inline constexpr std::uint32_t kSqeAsyncBit = kSqeIoHardlinkBit + 1;
inline constexpr std::uint32_t kSqeBufferSelectBit = kSqeAsyncBit + 1;
inline constexpr std::uint32_t kSqeCqeSkipSuccessBit = kSqeBufferSelectBit + 1;
inline constexpr std::uint32_t kOpNop = 0;
inline constexpr std::uint32_t kOpReadv = kOpNop + 1;
inline constexpr std::uint32_t kOpWritev = kOpReadv + 1;
inline constexpr std::uint32_t kOpFsync = kOpWritev + 1;
inline constexpr std::uint32_t kOpReadFixed = kOpFsync + 1;
inline constexpr std::uint32_t kOpWriteFixed = kOpReadFixed + 1;
inline constexpr std::uint32_t kOpPollAdd = kOpWriteFixed + 1;
inline constexpr std::uint32_t kOpPollRemove = kOpPollAdd + 1;
inline constexpr std::uint32_t kOpSyncFileRange = kOpPollRemove + 1;
inline constexpr std::uint32_t kOpSendmsg = kOpSyncFileRange + 1;
inline constexpr std::uint32_t kOpRecvmsg = kOpSendmsg + 1;
inline constexpr std::uint32_t kOpTimeout = kOpRecvmsg + 1;
inline constexpr std::uint32_t kOpTimeoutRemove = kOpTimeout + 1;
inline constexpr std::uint32_t kOpAccept = kOpTimeoutRemove + 1;
inline constexpr std::uint32_t kOpAsyncCancel = kOpAccept + 1;
inline constexpr std::uint32_t kOpLinkTimeout = kOpAsyncCancel + 1;
inline constexpr std::uint32_t kOpConnect = kOpLinkTimeout + 1;
inline constexpr std::uint32_t kOpFallocate = kOpConnect + 1;
inline constexpr std::uint32_t kOpOpenat = kOpFallocate + 1;
inline constexpr std::uint32_t kOpClose = kOpOpenat + 1;
inline constexpr std::uint32_t kOpFilesUpdate = kOpClose + 1;
inline constexpr std::uint32_t kOpStatx = kOpFilesUpdate + 1;
inline constexpr std::uint32_t kOpRead = kOpStatx + 1;
inline constexpr std::uint32_t kOpWrite = kOpRead + 1;
inline constexpr std::uint32_t kOpFadvise = kOpWrite + 1;
inline constexpr std::uint32_t kOpMadvise = kOpFadvise + 1;
inline constexpr std::uint32_t kOpSend = kOpMadvise + 1;
inline constexpr std::uint32_t kOpRecv = kOpSend + 1;
inline constexpr std::uint32_t kOpOpenat2 = kOpRecv + 1;
inline constexpr std::uint32_t kOpEpollCtl = kOpOpenat2 + 1;
inline constexpr std::uint32_t kOpSplice = kOpEpollCtl + 1;
inline constexpr std::uint32_t kOpProvideBuffers = kOpSplice + 1;
inline constexpr std::uint32_t kOpRemoveBuffers = kOpProvideBuffers + 1;
inline constexpr std::uint32_t kOpTee = kOpRemoveBuffers + 1;
inline constexpr std::uint32_t kOpShutdown = kOpTee + 1;
inline constexpr std::uint32_t kOpRenameat = kOpShutdown + 1;
inline constexpr std::uint32_t kOpUnlinkat = kOpRenameat + 1;
inline constexpr std::uint32_t kOpMkdirat = kOpUnlinkat + 1;
inline constexpr std::uint32_t kOpSymlinkat = kOpMkdirat + 1;
inline constexpr std::uint32_t kOpLinkat = kOpSymlinkat + 1;
inline constexpr std::uint32_t kOpMsgRing = kOpLinkat + 1;
inline constexpr std::uint32_t kOpFsetxattr = kOpMsgRing + 1;
inline constexpr std::uint32_t kOpSetxattr = kOpFsetxattr + 1;
inline constexpr std::uint32_t kOpFgetxattr = kOpSetxattr + 1;
inline constexpr std::uint32_t kOpGetxattr = kOpFgetxattr + 1;
inline constexpr std::uint32_t kOpSocket = kOpGetxattr + 1;
inline constexpr std::uint32_t kOpUringCmd = kOpSocket + 1;
inline constexpr std::uint32_t kOpSendZc = kOpUringCmd + 1;
inline constexpr std::uint32_t kOpSendmsgZc = kOpSendZc + 1;
inline constexpr std::uint32_t kOpReadMultishot = kOpSendmsgZc + 1;
inline constexpr std::uint32_t kOpWaitid = kOpReadMultishot + 1;
inline constexpr std::uint32_t kOpFutexWait = kOpWaitid + 1;
inline constexpr std::uint32_t kOpFutexWake = kOpFutexWait + 1;
inline constexpr std::uint32_t kOpFutexWaitv = kOpFutexWake + 1;
inline constexpr std::uint32_t kOpFixedFdInstall = kOpFutexWaitv + 1;
inline constexpr std::uint32_t kOpFtruncate = kOpFixedFdInstall + 1;
inline constexpr std::uint32_t kOpBind = kOpFtruncate + 1;
inline constexpr std::uint32_t kOpListen = kOpBind + 1;
inline constexpr std::uint32_t kOpRecvZc = kOpListen + 1;
inline constexpr std::uint32_t kOpEpollWait = kOpRecvZc + 1;
inline constexpr std::uint32_t kOpReadvFixed = kOpEpollWait + 1;
inline constexpr std::uint32_t kOpWritevFixed = kOpReadvFixed + 1;
inline constexpr std::uint32_t kOpPipe = kOpWritevFixed + 1;
inline constexpr std::uint32_t kOpNop128 = kOpPipe + 1;
inline constexpr std::uint32_t kOpUringCmd128 = kOpNop128 + 1;
inline constexpr std::uint32_t kOpLast = kOpUringCmd128 + 1;
inline constexpr std::uint32_t kMsgData = 0;
inline constexpr std::uint32_t kMsgSendFd = kMsgData + 1;
inline constexpr std::uint32_t kRegisterBuffers = 0;
inline constexpr std::uint32_t kUnregisterBuffers = 1;
inline constexpr std::uint32_t kRegisterFiles = 2;
inline constexpr std::uint32_t kUnregisterFiles = 3;
inline constexpr std::uint32_t kRegisterEventfd = 4;
inline constexpr std::uint32_t kUnregisterEventfd = 5;
inline constexpr std::uint32_t kRegisterFilesUpdate = 6;
inline constexpr std::uint32_t kRegisterEventfdAsync = 7;
inline constexpr std::uint32_t kRegisterProbe = 8;
inline constexpr std::uint32_t kRegisterPersonality = 9;
inline constexpr std::uint32_t kUnregisterPersonality = 10;
inline constexpr std::uint32_t kRegisterRestrictions = 11;
inline constexpr std::uint32_t kRegisterEnableRings = 12;
inline constexpr std::uint32_t kRegisterFiles2 = 13;
inline constexpr std::uint32_t kRegisterFilesUpdate2 = 14;
inline constexpr std::uint32_t kRegisterBuffers2 = 15;
inline constexpr std::uint32_t kRegisterBuffersUpdate = 16;
inline constexpr std::uint32_t kRegisterIoWqAff = 17;
inline constexpr std::uint32_t kUnregisterIoWqAff = 18;
inline constexpr std::uint32_t kRegisterIoWqMaxWorkers = 19;
inline constexpr std::uint32_t kRegisterRingFds = 20;
inline constexpr std::uint32_t kUnregisterRingFds = 21;
inline constexpr std::uint32_t kRegisterPbufRing = 22;
inline constexpr std::uint32_t kUnregisterPbufRing = 23;
inline constexpr std::uint32_t kRegisterSyncCancel = 24;
inline constexpr std::uint32_t kRegisterFileAllocRange = 25;
inline constexpr std::uint32_t kRegisterPbufStatus = 26;
inline constexpr std::uint32_t kRegisterNapi = 27;
inline constexpr std::uint32_t kUnregisterNapi = 28;
inline constexpr std::uint32_t kRegisterClock = 29;
inline constexpr std::uint32_t kRegisterCloneBuffers = 30;
inline constexpr std::uint32_t kRegisterSendMsgRing = 31;
inline constexpr std::uint32_t kRegisterZcrxIfq = 32;
inline constexpr std::uint32_t kRegisterResizeRings = 33;
inline constexpr std::uint32_t kRegisterMemRegion = 34;
inline constexpr std::uint32_t kRegisterQuery = 35;
inline constexpr std::uint32_t kRegisterZcrxCtrl = 36;
inline constexpr std::uint32_t kRegisterBpfFilter = 37;
inline constexpr std::uint32_t kRegisterLast = kRegisterBpfFilter + 1;
inline constexpr std::uint32_t kRegisterUseRegisteredRing = 1U << 31;
inline constexpr std::uint32_t kMemRegionTypeUser = 1;
inline constexpr std::uint32_t kMemRegionRegWaitArg = 1;
inline constexpr std::uint32_t kRegisterSrcRegistered = (1U << 0);
inline constexpr std::uint32_t kRegisterDstReplace = (1U << 1);
inline constexpr std::uint32_t kPbufRingMmap = 1;
inline constexpr std::uint32_t kPbufRingInc = 2;
inline constexpr std::uint32_t kIoUringNapiRegisterOp = 0;
inline constexpr std::uint32_t kIoUringNapiStaticAddId = 1;
inline constexpr std::uint32_t kIoUringNapiStaticDelId = 2;
inline constexpr std::uint32_t kIoUringNapiTrackingDynamic = 0;
inline constexpr std::uint32_t kIoUringNapiTrackingStatic = 1;
inline constexpr std::uint32_t kIoUringNapiTrackingInactive = 255;
inline constexpr std::uint32_t kRestrictionRegisterOp = 0;
inline constexpr std::uint32_t kRestrictionSqeOp = 1;
inline constexpr std::uint32_t kRestrictionSqeFlagsAllowed = 2;
inline constexpr std::uint32_t kRestrictionSqeFlagsRequired = 3;
inline constexpr std::uint32_t kRestrictionLast = kRestrictionSqeFlagsRequired + 1;
inline constexpr std::uint32_t kRegWaitTs = (1U << 0);
inline constexpr std::uint32_t kZcrxAreaDmabuf = 1;
inline constexpr std::uint32_t kRwAttrFlagPi = (1U << 0);
inline constexpr std::uint32_t kFileIndexAlloc = (~0U);
inline constexpr std::uint32_t kSqeFixedFile = (1U << kSqeFixedFileBit);
inline constexpr std::uint32_t kSqeIoDrain = (1U << kSqeIoDrainBit);
inline constexpr std::uint32_t kSqeIoLink = (1U << kSqeIoLinkBit);
inline constexpr std::uint32_t kSqeIoHardlink = (1U << kSqeIoHardlinkBit);
inline constexpr std::uint32_t kSqeAsync = (1U << kSqeAsyncBit);
inline constexpr std::uint32_t kSqeBufferSelect = (1U << kSqeBufferSelectBit);
inline constexpr std::uint32_t kSqeCqeSkipSuccess = (1U << kSqeCqeSkipSuccessBit);
inline constexpr std::uint32_t kSetupIoPoll = (1U << 0);
inline constexpr std::uint32_t kSetupSqPoll = (1U << 1);
inline constexpr std::uint32_t kSetupSqAff = (1U << 2);
inline constexpr std::uint32_t kSetupCqSize = (1U << 3);
inline constexpr std::uint32_t kSetupClamp = (1U << 4);
inline constexpr std::uint32_t kSetupAttachWq = (1U << 5);
inline constexpr std::uint32_t kSetupRDisabled = (1U << 6);
inline constexpr std::uint32_t kSetupSubmitAll = (1U << 7);
inline constexpr std::uint32_t kSetupCoopTaskrun = (1U << 8);
inline constexpr std::uint32_t kSetupTaskrunFlag = (1U << 9);
inline constexpr std::uint32_t kSetupSqe128 = (1U << 10);
inline constexpr std::uint32_t kSetupCqe32 = (1U << 11);
inline constexpr std::uint32_t kSetupSingleIssuer = (1U << 12);
inline constexpr std::uint32_t kSetupDeferTaskrun = (1U << 13);
inline constexpr std::uint32_t kSetupNoMmap = (1U << 14);
inline constexpr std::uint32_t kSetupRegisteredFdOnly = (1U << 15);
inline constexpr std::uint32_t kSetupNoSqArray = (1U << 16);
inline constexpr std::uint32_t kSetupHybridIoPoll = (1U << 17);
inline constexpr std::uint32_t kSetupCqeMixed = (1U << 18);
inline constexpr std::uint32_t kSetupSqeMixed = (1U << 19);
inline constexpr std::uint32_t kSetupSqRewind = (1U << 20);
inline constexpr std::uint32_t kUringCmdFixed = (1U << 0);
inline constexpr std::uint32_t kUringCmdMultishot = (1U << 1);
inline constexpr std::uint32_t kUringCmdMask = (kUringCmdFixed | kUringCmdMultishot);
inline constexpr std::uint32_t kFsyncDatasync = (1U << 0);
inline constexpr std::uint32_t kTimeoutAbs = (1U << 0);
inline constexpr std::uint32_t kTimeoutUpdate = (1U << 1);
inline constexpr std::uint32_t kTimeoutBoottime = (1U << 2);
inline constexpr std::uint32_t kTimeoutRealtime = (1U << 3);
inline constexpr std::uint32_t kLinkTimeoutUpdate = (1U << 4);
inline constexpr std::uint32_t kTimeoutEtimeSuccess = (1U << 5);
inline constexpr std::uint32_t kTimeoutMultishot = (1U << 6);
inline constexpr std::uint32_t kTimeoutClockMask = (kTimeoutBoottime | kTimeoutRealtime);
inline constexpr std::uint32_t kTimeoutUpdateMask = (kTimeoutUpdate | kLinkTimeoutUpdate);
inline constexpr std::uint32_t kPollAddMulti = (1U << 0);
inline constexpr std::uint32_t kPollUpdateEvents = (1U << 1);
inline constexpr std::uint32_t kPollUpdateUserData = (1U << 2);
inline constexpr std::uint32_t kPollAddLevel = (1U << 3);
inline constexpr std::uint32_t kAsyncCancelAll = (1U << 0);
inline constexpr std::uint32_t kAsyncCancelFd = (1U << 1);
inline constexpr std::uint32_t kAsyncCancelAny = (1U << 2);
inline constexpr std::uint32_t kAsyncCancelFdFixed = (1U << 3);
inline constexpr std::uint32_t kAsyncCancelUserdata = (1U << 4);
inline constexpr std::uint32_t kAsyncCancelOp = (1U << 5);
inline constexpr std::uint32_t kRecvsendPollFirst = (1U << 0);
inline constexpr std::uint32_t kRecvMultishot = (1U << 1);
inline constexpr std::uint32_t kRecvsendFixedBuf = (1U << 2);
inline constexpr std::uint32_t kSendZcReportUsage = (1U << 3);
inline constexpr std::uint32_t kRecvsendBundle = (1U << 4);
inline constexpr std::uint32_t kSendVectorized = (1U << 5);
inline constexpr std::uint32_t kNotifUsageZcCopied = (1U << 31);
inline constexpr std::uint32_t kAcceptMultishot = (1U << 0);
inline constexpr std::uint32_t kAcceptDontwait = (1U << 1);
inline constexpr std::uint32_t kAcceptPollFirst = (1U << 2);
inline constexpr std::uint32_t kMsgRingCqeSkip = (1U << 0);
inline constexpr std::uint32_t kMsgRingFlagsPass = (1U << 1);
inline constexpr std::uint32_t kFixedFdNoCloexec = (1U << 0);
inline constexpr std::uint32_t kNopInjectResult = (1U << 0);
inline constexpr std::uint32_t kNopFile = (1U << 1);
inline constexpr std::uint32_t kNopFixedFile = (1U << 2);
inline constexpr std::uint32_t kNopFixedBuffer = (1U << 3);
inline constexpr std::uint32_t kNopTw = (1U << 4);
inline constexpr std::uint32_t kNopCqe32 = (1U << 5);
inline constexpr std::uint32_t kCqeBuffer = (1U << 0);
inline constexpr std::uint32_t kCqeMore = (1U << 1);
inline constexpr std::uint32_t kCqeSockNonempty = (1U << 2);
inline constexpr std::uint32_t kCqeNotif = (1U << 3);
inline constexpr std::uint32_t kCqeBufMore = (1U << 4);
inline constexpr std::uint32_t kCqeSkip = (1U << 5);
inline constexpr std::uint32_t kCqe32 = (1U << 15);
inline constexpr std::uint32_t kCqeBufferShift = 16;
inline constexpr std::uint64_t kOffSqRing = 0ULL;
inline constexpr std::uint64_t kOffCqRing = 0x8000000ULL;
inline constexpr std::uint64_t kOffSqes = 0x10000000ULL;
inline constexpr std::uint64_t kOffPbufRing = 0x80000000ULL;
inline constexpr std::uint32_t kOffPbufShift = 16;
inline constexpr std::uint64_t kOffMmapMask = 0xf8000000ULL;
inline constexpr std::uint32_t kSqNeedWakeup = (1U << 0);
inline constexpr std::uint32_t kSqCqOverflow = (1U << 1);
inline constexpr std::uint32_t kSqTaskrun = (1U << 2);
inline constexpr std::uint32_t kCqEventfdDisabled = (1U << 0);
inline constexpr std::uint32_t kEnterGetevents = (1U << 0);
inline constexpr std::uint32_t kEnterSqWakeup = (1U << 1);
inline constexpr std::uint32_t kEnterSqWait = (1U << 2);
inline constexpr std::uint32_t kEnterExtArg = (1U << 3);
inline constexpr std::uint32_t kEnterRegisteredRing = (1U << 4);
inline constexpr std::uint32_t kEnterAbsTimer = (1U << 5);
inline constexpr std::uint32_t kEnterExtArgReg = (1U << 6);
inline constexpr std::uint32_t kEnterNoIowait = (1U << 7);
inline constexpr std::uint32_t kFeatSingleMmap = (1U << 0);
inline constexpr std::uint32_t kFeatNoDrop = (1U << 1);
inline constexpr std::uint32_t kFeatSubmitStable = (1U << 2);
inline constexpr std::uint32_t kFeatRwCurPos = (1U << 3);
inline constexpr std::uint32_t kFeatCurPersonality = (1U << 4);
inline constexpr std::uint32_t kFeatFastPoll = (1U << 5);
inline constexpr std::uint32_t kFeatPoll32Bits = (1U << 6);
inline constexpr std::uint32_t kFeatSqPollNonfixed = (1U << 7);
inline constexpr std::uint32_t kFeatExtArg = (1U << 8);
inline constexpr std::uint32_t kFeatNativeWorkers = (1U << 9);
inline constexpr std::uint32_t kFeatRsrcTags = (1U << 10);
inline constexpr std::uint32_t kFeatCqeSkip = (1U << 11);
inline constexpr std::uint32_t kFeatLinkedFile = (1U << 12);
inline constexpr std::uint32_t kFeatRegRegRing = (1U << 13);
inline constexpr std::uint32_t kFeatRecvsendBundle = (1U << 14);
inline constexpr std::uint32_t kFeatMinTimeout = (1U << 15);
inline constexpr std::uint32_t kFeatRwAttr = (1U << 16);
inline constexpr std::uint32_t kFeatNoIowait = (1U << 17);
inline constexpr std::uint32_t kRsrcRegisterSparse = (1U << 0);
inline constexpr std::int32_t kRegisterFilesSkip = (-2);
inline constexpr std::uint32_t kIoUringOpSupported = (1U << 0);
inline constexpr std::uint32_t kTimestampHwShift = 16;
inline constexpr std::uint32_t kTimestampTypeShift = (kTimestampHwShift + 1);
inline constexpr std::uint32_t kCqeTstampHw = ((std::uint32_t)1 << kTimestampHwShift);
inline constexpr std::uint32_t kZcrxAreaShift = 48;
inline constexpr std::uint64_t kZcrxAreaMask = (~(((std::uint64_t)1 << kZcrxAreaShift) - 1));
struct FlatSqe {
  std::uint8_t opcode{};
  std::uint8_t flags{};
  std::uint16_t ioprio{};
  std::int32_t fd{};
  std::uint64_t offset{};
  std::uint64_t address{};
  std::uint32_t len{};
  std::uint32_t operation_flags{};
  std::uint64_t user_data{};
  std::uint16_t buffer_index{};
  std::uint16_t personality{};
  std::uint32_t file_or_splice{};
  std::uint64_t address3{};
  std::uint64_t extra{};
  constexpr std::uint64_t& off() noexcept { return offset; }
  [[nodiscard]] constexpr const std::uint64_t& off() const noexcept { return offset; }
  constexpr std::uint64_t& addr2() noexcept { return offset; }
  [[nodiscard]] constexpr const std::uint64_t& addr2() const noexcept { return offset; }
  constexpr std::uint64_t& addr() noexcept { return address; }
  [[nodiscard]] constexpr const std::uint64_t& addr() const noexcept { return address; }
  constexpr std::uint64_t& splice_off_in() noexcept { return address; }
  [[nodiscard]] constexpr const std::uint64_t& splice_off_in() const noexcept { return address; }
  constexpr std::uint64_t& addr3() noexcept { return address3; }
  [[nodiscard]] constexpr const std::uint64_t& addr3() const noexcept { return address3; }
  constexpr std::uint64_t& optval() noexcept { return address3; }
  [[nodiscard]] constexpr const std::uint64_t& optval() const noexcept { return address3; }
  constexpr std::uint32_t& op_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& op_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& rw_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& rw_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& fsync_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& fsync_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& poll32_events() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& poll32_events() const noexcept { return operation_flags; }
  constexpr std::uint32_t& sync_range_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& sync_range_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& msg_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& msg_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& timeout_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& timeout_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& accept_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& accept_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& cancel_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& cancel_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& open_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& open_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& statx_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& statx_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& fadvise_advice() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& fadvise_advice() const noexcept { return operation_flags; }
  constexpr std::uint32_t& splice_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& splice_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& rename_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& rename_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& unlink_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& unlink_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& hardlink_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& hardlink_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& xattr_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& xattr_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& msg_ring_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& msg_ring_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& uring_cmd_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& uring_cmd_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& waitid_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& waitid_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& futex_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& futex_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& install_fd_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& install_fd_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& nop_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& nop_flags() const noexcept { return operation_flags; }
  constexpr std::uint32_t& pipe_flags() noexcept { return operation_flags; }
  [[nodiscard]] constexpr const std::uint32_t& pipe_flags() const noexcept { return operation_flags; }
  constexpr std::uint16_t& buf_index() noexcept { return buffer_index; }
  [[nodiscard]] constexpr const std::uint16_t& buf_index() const noexcept { return buffer_index; }
  constexpr std::uint16_t& buf_group() noexcept { return buffer_index; }
  [[nodiscard]] constexpr const std::uint16_t& buf_group() const noexcept { return buffer_index; }
  constexpr std::uint32_t& file_index() noexcept { return file_or_splice; }
  [[nodiscard]] constexpr const std::uint32_t& file_index() const noexcept { return file_or_splice; }
  constexpr std::uint32_t& optlen() noexcept { return file_or_splice; }
  [[nodiscard]] constexpr const std::uint32_t& optlen() const noexcept { return file_or_splice; }
  constexpr std::uint32_t& zcrx_ifq_idx() noexcept { return file_or_splice; }
  [[nodiscard]] constexpr const std::uint32_t& zcrx_ifq_idx() const noexcept { return file_or_splice; }
  [[nodiscard]] constexpr std::uint32_t cmd_op() const noexcept { return static_cast<std::uint32_t>(offset); }
  constexpr void cmd_op(std::uint32_t v) noexcept {
    auto p = std::bit_cast<std::array<std::uint32_t, 2>>(offset);
    p[0] = v;
    offset = std::bit_cast<std::uint64_t>(p);
  }
  [[nodiscard]] constexpr std::int32_t splice_fd_in() const noexcept { return std::bit_cast<std::int32_t>(file_or_splice); }
  constexpr void splice_fd_in(std::int32_t v) noexcept { file_or_splice = std::bit_cast<std::uint32_t>(v); }
  [[nodiscard]] constexpr std::uint16_t addr_len() const noexcept { return static_cast<std::uint16_t>(file_or_splice); }
  constexpr void addr_len(std::uint16_t v) noexcept {
    auto p = std::bit_cast<std::array<std::uint16_t, 2>>(file_or_splice);
    p[0] = v;
    file_or_splice = std::bit_cast<std::uint32_t>(p);
  }
};
struct FlatSqe128 {
  FlatSqe sqe{};
  std::uint64_t extension[8]{};
  std::span<std::byte, 80> command() noexcept { return std::span<std::byte, 80>{reinterpret_cast<std::byte*>(this) + 48, 80}; }
  [[nodiscard]] std::span<const std::byte, 80> command() const noexcept {
    return std::span<const std::byte, 80>{reinterpret_cast<const std::byte*>(this) + 48, 80};
  }
};
struct FlatCqe {
  std::uint64_t user_data{};
  std::int32_t res{};
  std::uint32_t flags{};
};
struct FlatCqe32 {
  FlatCqe cqe{};
  std::uint64_t extension[2]{};
};
struct IoSqringOffsets {
  std::uint32_t head{};
  std::uint32_t tail{};
  std::uint32_t ring_mask{};
  std::uint32_t ring_entries{};
  std::uint32_t flags{};
  std::uint32_t dropped{};
  std::uint32_t array{};
  std::uint32_t resv1{};
  std::uint64_t user_addr{};
};
struct IoCqringOffsets {
  std::uint32_t head{};
  std::uint32_t tail{};
  std::uint32_t ring_mask{};
  std::uint32_t ring_entries{};
  std::uint32_t overflow{};
  std::uint32_t cqes{};
  std::uint32_t flags{};
  std::uint32_t resv1{};
  std::uint64_t user_addr{};
};
struct IoUringParams {
  std::uint32_t sq_entries{};
  std::uint32_t cq_entries{};
  std::uint32_t flags{};
  std::uint32_t sq_thread_cpu{};
  std::uint32_t sq_thread_idle{};
  std::uint32_t features{};
  std::uint32_t wq_fd{};
  std::uint32_t resv[3]{};
  IoSqringOffsets sq_off{};
  IoCqringOffsets cq_off{};
};
struct IoUringRsrcRegister {
  std::uint32_t nr{};
  std::uint32_t flags{};
  std::uint64_t resv2{};
  std::uint64_t data{};
  std::uint64_t tags{};
};
struct IoUringRsrcUpdate2 {
  std::uint32_t offset{};
  std::uint32_t resv{};
  std::uint64_t data{};
  std::uint64_t tags{};
  std::uint32_t nr{};
  std::uint32_t resv2{};
};
struct IoUringRsrcUpdate {
  std::uint32_t offset{};
  std::uint32_t resv{};
  std::uint64_t data{};
};
struct IoUringBuf {
  std::uint64_t addr{};
  std::uint32_t len{};
  std::uint16_t bid{};
  std::uint16_t resv{};
};
struct IoUringBufRing {
  std::uint64_t resv1{};
  std::uint32_t resv2{};
  std::uint16_t resv3{};
  std::uint16_t tail{};
};
struct IoUringBufReg {
  std::uint64_t ring_addr{};
  std::uint32_t ring_entries{};
  std::uint16_t bgid{};
  std::uint16_t flags{};
  std::uint32_t min_left{};
  std::uint32_t resv[5]{};
};
struct IoUringGeteventsArg {
  std::uint64_t sigmask{};
  std::uint32_t sigmask_sz{};
  std::uint32_t min_wait_usec{};
  std::uint64_t ts{};
};
struct IoUringSyncCancelReg {
  std::uint64_t addr{};
  std::int32_t fd{};
  std::uint32_t flags{};
  KernelTimespec timeout{};
  std::uint8_t opcode{};
  std::uint8_t pad[7]{};
  std::uint64_t pad2[3]{};
};
struct IoUringProbeOp {
  std::uint8_t op{};
  std::uint8_t resv{};
  std::uint16_t flags{};
  std::uint32_t resv2{};
};
struct IoUringProbe {
  std::uint8_t last_op{};
  std::uint8_t ops_len{};
  std::uint16_t resv{};
  std::uint32_t resv2[3]{};
};
struct IoUringZcrxRqe {
  std::uint64_t off{};
  std::uint32_t len{};
  std::uint32_t pad{};
};
struct IoUringZcrxOffsets {
  std::uint32_t head{};
  std::uint32_t tail{};
  std::uint32_t rqes{};
  std::uint32_t resv2{};
  std::uint64_t resv[2]{};
};
struct IoUringZcrxIfqReg {
  std::uint32_t if_idx{};
  std::uint32_t if_rxq{};
  std::uint32_t rq_entries{};
  std::uint32_t flags{};
  std::uint64_t area_ptr{};
  std::uint64_t region_ptr{};
  IoUringZcrxOffsets offsets{};
  std::uint32_t zcrx_id{};
  std::uint32_t rx_buf_len{};
  std::uint64_t resv[3]{};
};
struct IoUringRegionDesc {
  std::uint64_t user_addr{};
  std::uint64_t size{};
  std::uint32_t flags{};
  std::uint32_t id{};
  std::uint64_t mmap_offset{};
  std::uint64_t resv[4]{};
};
struct IoUringMemRegionReg {
  std::uint64_t region_uptr{};
  std::uint64_t flags{};
  std::uint64_t resv[2]{};
};
struct IoUringZcrxAreaReg {
  std::uint64_t addr{};
  std::uint64_t len{};
  std::uint64_t rq_area_token{};
  std::uint32_t flags{};
  std::uint32_t dmabuf_fd{};
  std::uint64_t resv2[2]{};
};
struct IoUringNapi {
  std::uint32_t busy_poll_to{};
  std::uint8_t prefer_busy_poll{};
  std::uint8_t opcode{};
  std::uint8_t pad[2]{};
  std::uint32_t op_param{};
  std::uint32_t resv{};
};
struct IoUringFileIndexRange {
  std::uint32_t off{};
  std::uint32_t len{};
  std::uint64_t resv{};
};
struct IoUringCloneBuffers {
  std::uint32_t src_fd{};
  std::uint32_t flags{};
  std::uint32_t src_off{};
  std::uint32_t dst_off{};
  std::uint32_t nr{};
  std::uint32_t pad[3]{};
};
struct IoUringClockRegister {
  std::uint32_t clockid{};
  std::uint32_t resv[3]{};
};
struct IoUringRestriction {
  std::uint16_t opcode{};
  std::uint8_t operation{};
  std::uint8_t resv{};
  std::uint32_t resv2[3]{};
};
struct IoUringBufStatus {
  std::uint32_t buf_group{};
  std::uint32_t head{};
  std::uint32_t resv[8]{};
};
struct IoUringQueryHdr {
  std::uint64_t next_entry{};
  std::uint64_t query_data{};
  std::uint32_t query_op{};
  std::uint32_t size{};
  std::int32_t result{};
  std::uint32_t resv[3]{};
};
struct IoUringQueryOpcode {
  std::uint32_t nr_request_opcodes{};
  std::uint32_t nr_register_opcodes{};
  std::uint64_t feature_flags{};
  std::uint64_t ring_setup_flags{};
  std::uint64_t enter_flags{};
  std::uint64_t sqe_flags{};
  std::uint32_t nr_query_opcodes{};
  std::uint32_t pad{};
};
}  // namespace liburing::abi
