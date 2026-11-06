/** @file @brief KernelRing owning Linux io_uring descriptors/mappings and ScriptedRing deterministic test seam. */
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/error/uring_error.hpp>
#include <liburing/ring/completion_queue.hpp>
#include <liburing/ring/ring_geometry.hpp>
#include <liburing/ring/submission_queue.hpp>
#include <liburing/sys/mmap_region.hpp>
#include <liburing/sys/unique_fd.hpp>
#include <liburing/sys/uring_sys.hpp>
#include <span>
#include <utility>

namespace liburing::ring {

struct NoMmapRingStorage {
  sys::MmapRegion rings{};
  sys::MmapRegion sqes{};
};

[[nodiscard]] inline Result<NoMmapRingStorage> AllocateNoMmapRings(std::uint32_t entries, abi::IoUringParams& params) noexcept {
  if (entries == 0U || !std::has_single_bit(entries)) {
    return std::unexpected(UringErrorCode::kInvalid);
  }
  const std::size_t sqe_stride = ((params.flags & abi::kSetupSqe128) != 0U) ? 128U : 64U;
  const std::size_t cqe_stride = ((params.flags & abi::kSetupCqe32) != 0U) ? 32U : 16U;
  const std::uint32_t cq_entries =
      ((params.flags & abi::kSetupCqSize) != 0U && params.cq_entries != 0U) ? params.cq_entries : (entries * 2U);
  const std::size_t raw_ring_bytes =
      4096U + static_cast<std::size_t>(entries) * 4U + static_cast<std::size_t>(cq_entries) * cqe_stride;
  const std::size_t ring_bytes = (raw_ring_bytes + 4095U) & ~std::size_t{4095U};
  const std::size_t raw_sqe_bytes = static_cast<std::size_t>(entries) * sqe_stride;
  const std::size_t sqe_bytes = (raw_sqe_bytes + 4095U) & ~std::size_t{4095U};

  auto rings =
      sys::MmapRegion::Map(nullptr, ring_bytes, sys::kProtRead | sys::kProtWrite, sys::kMapPrivate | sys::kMapAnonymous, -1, 0);
  if (!rings) return std::unexpected(rings.error());
  auto sqes =
      sys::MmapRegion::Map(nullptr, sqe_bytes, sys::kProtRead | sys::kProtWrite, sys::kMapPrivate | sys::kMapAnonymous, -1, 0);
  if (!sqes) return std::unexpected(sqes.error());

  params.flags |= abi::kSetupNoMmap;
  params.sq_off.user_addr = sys::Address(sqes->bytes().data());
  params.cq_off.user_addr = sys::Address(rings->bytes().data());
  return NoMmapRingStorage{.rings = std::move(*rings), .sqes = std::move(*sqes)};
}

class KernelRing final {
  sys::UniqueFd ring_fd_{};
  sys::RegisteredRingFd registered_fd_{};
  NoMmapRingStorage no_mmap_storage_{};
  sys::RingMappings mappings_{};
  abi::IoUringParams params_{};
  SqCursor sq_{};
  CqCursor cq_{};

 public:
  KernelRing() noexcept = default;
  KernelRing(const KernelRing&) = delete;
  KernelRing& operator=(const KernelRing&) = delete;
  KernelRing(KernelRing&&) noexcept = default;
  KernelRing& operator=(KernelRing&&) noexcept = default;

  [[nodiscard]] static Result<KernelRing> CreateWithParams(std::uint32_t entries, abi::IoUringParams params,
                                                           bool register_ring_fd = false,
                                                           NoMmapRingStorage no_mmap = {}) noexcept {
    if (entries == 0U) return std::unexpected(UringErrorCode::kInvalid);
    const bool registered_only = (params.flags & abi::kSetupRegisteredFdOnly) != 0U;
    if (registered_only && (params.flags & abi::kSetupNoMmap) == 0U) {
      return std::unexpected(UringErrorCode::kInvalid);
    }

    auto setup = ToResult(sys::SysIoUringSetup(entries, &params));
    if (!setup) return std::unexpected(setup.error());

    KernelRing ring;
    ring.params_ = params;
    ring.no_mmap_storage_ = std::move(no_mmap);

    if (registered_only) {
      auto reg_fd = sys::RegisteredRingFd::Adopt(static_cast<std::uint32_t>(*setup));
      if (!reg_fd) return std::unexpected(reg_fd.error());
      ring.registered_fd_ = std::move(*reg_fd);
    } else {
      auto ufd = sys::UniqueFd::Adopt(static_cast<std::int32_t>(*setup));
      if (!ufd) return std::unexpected(ufd.error());
      ring.ring_fd_ = std::move(*ufd);
      if (register_ring_fd) {
        auto reg_fd = sys::RegisteredRingFd::Register(ring.ring_fd_.get());
        if (!reg_fd) return std::unexpected(reg_fd.error());
        ring.registered_fd_ = std::move(*reg_fd);
      }
    }

    if ((params.flags & abi::kSetupNoMmap) != 0U) {
      const sys::NoMmapMemory memory{
          .rings = ring.no_mmap_storage_.rings.bytes(),
          .sqes = ring.no_mmap_storage_.sqes.bytes(),
      };
      auto mapped = sys::RingMappings::Map(ring.ring_fd_.get(), ring.params_, &memory);
      if (!mapped) return std::unexpected(mapped.error());
      ring.mappings_ = std::move(*mapped);
    } else {
      auto mapped = sys::RingMappings::Map(ring.ring_fd_.get(), ring.params_);
      if (!mapped) return std::unexpected(mapped.error());
      ring.mappings_ = std::move(*mapped);
    }

    ring.sq_ = SqCursor::FromMappedRing(ring.mappings_.sq(), ring.mappings_.sqes(), ring.params_);
    ring.cq_ = CqCursor::FromMappedRing(ring.mappings_.cq(), ring.params_);
    return ring;
  }

  [[nodiscard]] static Result<KernelRing> Create(std::uint32_t entries, RingMode mode = RingMode::kStandard64_16,
                                                 std::uint32_t extra_setup_flags = 0U, bool register_ring_fd = false) noexcept {
    if (mode != RingMode::kStandard64_16 || extra_setup_flags != 0U) {
      abi::IoUringParams params{};
      params.flags = BuildSingleIssuerFlags(mode, extra_setup_flags);
      return CreateWithParams(entries, params, register_ring_fd);
    }
    // Kernels before 6.6 reject NO_SQARRAY; kernels before 6.1 reject DEFER_TASKRUN.
    static_assert(BuildSingleIssuerFlags(RingMode::kStandard64_16) ==
                  (abi::kSetupSingleIssuer | abi::kSetupCoopTaskrun | abi::kSetupTaskrunFlag | abi::kSetupDeferTaskrun |
                   abi::kSetupNoSqArray));
    constexpr std::uint32_t single_issuer_ladder[] = {
        abi::kSetupSingleIssuer | abi::kSetupCoopTaskrun | abi::kSetupTaskrunFlag | abi::kSetupDeferTaskrun |
            abi::kSetupNoSqArray,
        abi::kSetupSingleIssuer | abi::kSetupCoopTaskrun | abi::kSetupTaskrunFlag | abi::kSetupDeferTaskrun,
        abi::kSetupSingleIssuer | abi::kSetupCoopTaskrun | abi::kSetupTaskrunFlag,
        abi::kSetupSingleIssuer,
    };
    Result<KernelRing> opened = std::unexpected(UringErrorCode::kInvalid);
    for (const std::uint32_t flags : single_issuer_ladder) {
      abi::IoUringParams params{};
      params.flags = flags;
      opened = CreateWithParams(entries, params, register_ring_fd);
      if (opened || opened.error() != UringErrorCode::kInvalid) return opened;
    }
    return opened;
  }

  [[nodiscard]] static Result<KernelRing> CreateNoMmap(std::uint32_t entries, RingMode mode = RingMode::kStandard64_16,
                                                       std::uint32_t extra_setup_flags = 0U,
                                                       bool register_ring_fd = false) noexcept {
    abi::IoUringParams params{};
    params.flags = BuildSingleIssuerFlags(mode, extra_setup_flags | abi::kSetupNoMmap);
    auto storage = AllocateNoMmapRings(entries, params);
    if (!storage) return std::unexpected(storage.error());
    return CreateWithParams(entries, params, register_ring_fd, std::move(*storage));
  }

  [[nodiscard]] SqCursor& Sq() noexcept { return sq_; }
  [[nodiscard]] const SqCursor& Sq() const noexcept { return sq_; }
  [[nodiscard]] CqCursor& Cq() noexcept { return cq_; }
  [[nodiscard]] const CqCursor& Cq() const noexcept { return cq_; }
  [[nodiscard]] const abi::IoUringParams& Params() const noexcept { return params_; }
  [[nodiscard]] SqRingMode SqMode() const noexcept { return sq_.SqMode(); }
  [[nodiscard]] CqRingMode CqMode() const noexcept { return cq_.CqMode(); }
  [[nodiscard]] RingMode Mode() const noexcept { return sq_.Mode(); }
  [[nodiscard]] std::int32_t Fd() const noexcept { return ring_fd_.get(); }
  [[nodiscard]] bool HasRegisteredFd() const noexcept { return registered_fd_.valid(); }

  [[nodiscard]] Result<void> ResizeRings(std::uint32_t sq_entries, std::uint32_t cq_entries = 0U) const noexcept {
    static_cast<void>(sq_entries);
    static_cast<void>(cq_entries);
    if ((params_.flags & abi::kSetupNoMmap) == 0U) {
      // Resizing without NO_MMAP user buffers invalidates the mmap ring cursors.
      return std::unexpected(UringErrorCode::kNotSupported);
    }
    return std::unexpected(UringErrorCode::kNotSupported);
  }

  [[nodiscard]] Result<void> RegisterRingFd() noexcept {
    if (registered_fd_.valid()) return {};
    if (!ring_fd_.valid()) return std::unexpected(UringErrorCode::kBadRing);
    auto reg = sys::RegisteredRingFd::Register(ring_fd_.get());
    if (!reg) return std::unexpected(reg.error());
    registered_fd_ = std::move(*reg);
    return {};
  }

  [[nodiscard]] Result<std::uint32_t> PollKernelCompletions(std::uint32_t min_complete = 0U,
                                                            std::uint32_t flags = abi::kEnterGetevents,
                                                            const void* arg = nullptr, std::size_t arg_size = 0U) noexcept {
    std::int32_t target_fd = ring_fd_.get();
    flags |= abi::kEnterGetevents;
    if (registered_fd_.valid()) {
      target_fd = static_cast<std::int32_t>(registered_fd_.index());
      flags |= abi::kEnterRegisteredRing;
    }
    auto entered = ToResult(sys::SysIoUringEnter(target_fd, 0U, min_complete, flags, arg, arg_size));
    if (!entered) return std::unexpected(entered.error());
    return static_cast<std::uint32_t>(*entered);
  }

  [[nodiscard]] Result<std::uint32_t> SubmitAndWait(std::uint32_t min_complete, std::uint32_t extra_enter_flags = 0U,
                                                    const void* arg = nullptr, std::size_t arg_size = 0U) noexcept {
    const std::uint32_t to_submit = sq_.PrepareSubmit();
    if ((params_.flags & abi::kSetupSqPoll) != 0U) {
      std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    std::uint32_t flags = extra_enter_flags;
    if (min_complete > 0U || (params_.flags & abi::kSetupDeferTaskrun) != 0U) {
      flags |= abi::kEnterGetevents;
    }
    if (sq_.NeedsWakeup()) {
      flags |= abi::kEnterSqWakeup;
    }
    std::int32_t target_fd = ring_fd_.get();
    if (registered_fd_.valid()) {
      target_fd = static_cast<std::int32_t>(registered_fd_.index());
      flags |= abi::kEnterRegisteredRing;
    }
    auto entered = ToResult(sys::SysIoUringEnter(target_fd, to_submit, min_complete, flags, arg, arg_size));
    if (!entered) {
      sq_.SyncSubmittedWithHead();
      return std::unexpected(entered.error());
    }
    sq_.CompleteSubmit(static_cast<std::uint32_t>(*entered), (params_.flags & abi::kSetupSqPoll) != 0U);
    return static_cast<std::uint32_t>(*entered);
  }

  [[nodiscard]] Result<std::uint32_t> Submit() noexcept { return SubmitAndWait(0U); }

  [[nodiscard]] Result<std::int64_t> Register(std::uint32_t opcode, const void* arg, std::uint32_t nr_args) noexcept {
    std::int32_t target_fd = ring_fd_.get();
    std::uint32_t effective_opcode = opcode;
    if (registered_fd_.valid()) {
      target_fd = static_cast<std::int32_t>(registered_fd_.index());
      effective_opcode |= abi::kRegisterUseRegisteredRing;
    }
    return ToResult(sys::SysIoUringRegister(target_fd, effective_opcode, arg, nr_args));
  }
};

/**
 * Deterministic in-memory ring satisfying the same operational interface as KernelRing.
 * Enables testing SQ backpressure, prefix-pad NOP injection, out-of-order ZC notifications,
 * F_SKIP, F_32, and CQ overflow without kernel scheduling nondeterminism.
 */
class ScriptedRing final {
  static constexpr std::size_t kMaxEntries = 64U;

  std::uint32_t sq_head_{0U};
  std::uint32_t sq_tail_{0U};
  std::uint32_t sq_flags_{0U};
  std::uint32_t sq_dropped_{0U};
  std::uint32_t cq_head_{0U};
  std::uint32_t cq_tail_{0U};
  std::uint32_t cq_overflow_{0U};
  std::uint32_t cq_flags_{0U};
  std::array<abi::FlatSqe, kMaxEntries * 2U> sqes_{};
  std::array<abi::FlatCqe, kMaxEntries * 2U> cqes_{};
  abi::IoUringParams params_{};
  SqCursor sq_{};
  CqCursor cq_{};
  bool auto_complete_nops_{true};
  bool consume_sq_on_submit_{true};
  std::int32_t simulated_fd_{0};
  bool has_registered_fd_{true};
  std::uint16_t next_personality_{1U};
  std::uint32_t last_register_opcode_{0U};

  constexpr void PushRawCqe(const abi::FlatCqe& cqe) noexcept {
    const std::uint32_t idx = cq_tail_ & cq_.Mask();
    if (cq_.CqMode() == CqRingMode::kCq32) {
      cqes_[idx * 2U] = cqe;
      cqes_[idx * 2U + 1U] = abi::FlatCqe{};
    } else {
      cqes_[idx] = cqe;
    }
    ++cq_tail_;
  }

 public:
  explicit constexpr ScriptedRing(std::uint32_t sq_entries = 8U, std::uint32_t cq_entries = 16U,
                                  RingMode mode = RingMode::kStandard64_16) noexcept {
    const std::uint32_t valid_sq =
        (sq_entries == 0U || sq_entries > kMaxEntries || !std::has_single_bit(sq_entries)) ? 8U : sq_entries;
    const std::uint32_t valid_cq =
        (cq_entries == 0U || cq_entries > kMaxEntries || !std::has_single_bit(cq_entries)) ? 16U : cq_entries;
    params_.sq_entries = valid_sq;
    params_.cq_entries = valid_cq;
    params_.flags = BuildSingleIssuerFlags(mode);
    sq_ = SqCursor(&sq_head_, &sq_tail_, valid_sq - 1U, valid_sq, &sq_flags_, &sq_dropped_, sqes_.data(), mode);
    cq_ = CqCursor(&cq_head_, &cq_tail_, valid_cq - 1U, valid_cq, &cq_overflow_, &cq_flags_, cqes_.data(), mode);
  }

  [[nodiscard]] constexpr SqCursor& Sq() noexcept { return sq_; }
  [[nodiscard]] constexpr const SqCursor& Sq() const noexcept { return sq_; }
  [[nodiscard]] constexpr CqCursor& Cq() noexcept { return cq_; }
  [[nodiscard]] constexpr const CqCursor& Cq() const noexcept { return cq_; }
  [[nodiscard]] constexpr const abi::IoUringParams& Params() const noexcept { return params_; }
  [[nodiscard]] constexpr SqRingMode SqMode() const noexcept { return sq_.SqMode(); }
  [[nodiscard]] constexpr CqRingMode CqMode() const noexcept { return cq_.CqMode(); }
  [[nodiscard]] constexpr RingMode Mode() const noexcept { return sq_.Mode(); }
  [[nodiscard]] constexpr std::int32_t Fd() const noexcept { return simulated_fd_; }
  [[nodiscard]] constexpr bool HasRegisteredFd() const noexcept { return has_registered_fd_; }

  [[nodiscard]] constexpr Result<void> ResizeRings(std::uint32_t sq_entries, std::uint32_t cq_entries = 0U) noexcept {
    if (sq_entries == 0U || sq_entries > kMaxEntries || !std::has_single_bit(sq_entries))
      return std::unexpected(UringErrorCode::kInvalid);
    cq_entries = cq_entries == 0U ? sq_entries * 2U : cq_entries;
    if (cq_entries == 0U || cq_entries > kMaxEntries || !std::has_single_bit(cq_entries))
      return std::unexpected(UringErrorCode::kInvalid);
    if (sq_.UnflushedCount() != 0U || sq_head_ != sq_tail_ || cq_head_ != cq_tail_)
      return std::unexpected(UringErrorCode::kBusy);
    const auto mode = Mode();
    sq_head_ = sq_tail_ = cq_head_ = cq_tail_ = 0U;
    params_.sq_entries = sq_entries;
    params_.cq_entries = cq_entries;
    sq_ = SqCursor(&sq_head_, &sq_tail_, sq_entries - 1U, sq_entries, &sq_flags_, &sq_dropped_, sqes_.data(), mode);
    cq_ = CqCursor(&cq_head_, &cq_tail_, cq_entries - 1U, cq_entries, &cq_overflow_, &cq_flags_, cqes_.data(), mode);
    return {};
  }

  constexpr void SetAutoCompleteNops(bool enabled) noexcept { auto_complete_nops_ = enabled; }
  constexpr void SetConsumeSqOnSubmit(bool enabled) noexcept { consume_sq_on_submit_ = enabled; }

  constexpr void AdvanceSqHead(std::uint32_t count) noexcept { sq_head_ += count; }

  constexpr void SetSimulatedOverflow(std::uint32_t overflow_count) noexcept {
    cq_overflow_ = overflow_count;
    if (overflow_count > 0U) {
      sq_flags_ |= abi::kSqCqOverflow;
    } else {
      sq_flags_ &= ~abi::kSqCqOverflow;
    }
  }

  constexpr void ScriptCqe(UserDataToken token, std::int32_t res, std::uint32_t flags = 0U) noexcept {
    PushRawCqe(abi::FlatCqe{.user_data = token.raw(), .res = res, .flags = flags});
  }

  constexpr void ScriptCqe32(UserDataToken token, std::int32_t res, std::uint32_t flags, std::uint64_t extra0,
                             std::uint64_t extra1) noexcept {
    const abi::FlatCqe ext{
        .user_data = extra0,
        .res = static_cast<std::int32_t>(extra1 & 0xffffffffULL),
        .flags = static_cast<std::uint32_t>(extra1 >> 32U),
    };
    if (cq_.CqMode() == CqRingMode::kCq32) {
      const std::uint32_t idx = cq_tail_ & cq_.Mask();
      cqes_[idx * 2U] = abi::FlatCqe{.user_data = token.raw(), .res = res, .flags = flags | abi::kCqe32};
      cqes_[idx * 2U + 1U] = ext;
      ++cq_tail_;
    } else {
      const std::uint32_t idx = cq_tail_ & cq_.Mask();
      cqes_[idx] = abi::FlatCqe{.user_data = token.raw(), .res = res, .flags = flags | abi::kCqe32};
      cqes_[(idx + 1U) & cq_.Mask()] = ext;
      cq_tail_ += 2U;
    }
  }

  constexpr void ScriptSkipCqe() noexcept { PushRawCqe(abi::FlatCqe{.user_data = 0U, .res = 0, .flags = abi::kCqeSkip}); }

  // Scripts an adversarial out-of-order zero-copy completion where F_NOTIF arrives before the primary CQE.
  constexpr void ScriptZcOutOfOrder(UserDataToken token, std::int32_t primary_res) noexcept {
    ScriptCqe(token, 0, abi::kCqeNotif);
    ScriptCqe(token, primary_res, abi::kCqeMore);
  }

  [[nodiscard]] constexpr Result<std::uint32_t> SubmitAndWait(std::uint32_t min_complete, std::uint32_t extra_enter_flags = 0U,
                                                              const void* arg = nullptr, std::size_t arg_size = 0U) noexcept {
    static_cast<void>(min_complete);
    static_cast<void>(extra_enter_flags);
    static_cast<void>(arg);
    static_cast<void>(arg_size);

    const std::uint32_t prev_head = sq_head_;
    const std::uint32_t submitted = sq_.ConsumePendingSubmitCount();
    if (!consume_sq_on_submit_) {
      return submitted;
    }

    std::uint32_t cursor = prev_head;
    while (cursor != sq_tail_) {
      const abi::FlatSqe& sqe = sq_.SlotAt(cursor);
      const bool is_128 = (sq_.SqMode() == SqRingMode::kSqMixed) && Is128ByteOpcode(sqe.opcode);
      const std::uint32_t step = is_128 ? 2U : 1U;
      if (auto_complete_nops_ &&
          (sqe.opcode == static_cast<std::uint8_t>(abi::kOpNop) || sqe.opcode == static_cast<std::uint8_t>(abi::kOpNop128))) {
        if ((sqe.flags & abi::kSqeCqeSkipSuccess) == 0U) {
          const std::int32_t res = ((sqe.nop_flags() & abi::kNopInjectResult) != 0U) ? static_cast<std::int32_t>(sqe.len) : 0;
          if ((sqe.nop_flags() & abi::kNopCqe32) != 0U) {
            ScriptCqe32(UserDataToken::from_raw(sqe.user_data), res, 0U, sqe.address, sqe.offset);
          } else {
            ScriptCqe(UserDataToken::from_raw(sqe.user_data), res, 0U);
          }
        }
      }
      cursor += step;
    }
    sq_head_ = sq_tail_;
    return submitted;
  }

  [[nodiscard]] constexpr Result<std::uint32_t> Submit() noexcept { return SubmitAndWait(0U); }

  [[nodiscard]] constexpr Result<std::int64_t> Register(std::uint32_t opcode, const void* arg, std::uint32_t nr_args) noexcept {
    static_cast<void>(arg);
    static_cast<void>(nr_args);
    last_register_opcode_ = opcode & ~abi::kRegisterUseRegisteredRing;
    if (last_register_opcode_ == abi::kRegisterPersonality) {
      return static_cast<std::int64_t>(next_personality_++);
    }
    return 0;
  }

  [[nodiscard]] constexpr std::uint32_t LastRegisterOpcode() const noexcept { return last_register_opcode_; }
};

}  // namespace liburing::ring

namespace liburing {
using ring::AllocateNoMmapRings;
using ring::KernelRing;
using ring::NoMmapRingStorage;
using ring::ScriptedRing;
}  // namespace liburing
