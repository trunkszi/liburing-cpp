#pragma once
#include <sched.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <expected>
#include <liburing/caps/kernel_caps.hpp>
#include <liburing/core/drain.hpp>
#include <liburing/ring/ring.hpp>
#include <optional>
#include <type_traits>
namespace liburing {
enum class DriveMode : std::uint8_t { kPoll, kWait };
struct EventDrivenConfig {
  bool no_iowait = false;
  std::uint32_t batch_wait_usec = 0;
};
struct SqPollConfig {
  std::uint32_t thread_idle_ms = 1000;
  std::optional<std::uint32_t> thread_cpu = std::nullopt;
};
struct OpcodeMask {
  std::array<uarch::Bitmap64, 4> words{};
  [[nodiscard]] constexpr bool Contains(std::uint8_t opcode) const noexcept { return words[opcode >> 6].test(opcode & 63); }
};
struct OpcodeSet {
  static constexpr OpcodeMask kIoPoll = [] consteval {
    OpcodeMask result;
    for (auto opcode : {abi::kOpNop, abi::kOpReadv, abi::kOpWritev, abi::kOpReadFixed, abi::kOpWriteFixed, abi::kOpRead,
                        abi::kOpWrite, abi::kOpReadvFixed, abi::kOpWritevFixed, abi::kOpUringCmd, abi::kOpUringCmd128})
      result.words[opcode >> 6] = result.words[opcode >> 6].set(opcode & 63);
    return result;
  }();
};
static_assert(uarch::NoPadding<OpcodeMask> && sizeof(OpcodeMask) == 32);
enum class ProactorModeKind : std::uint8_t { kEventDriven, kSqPoll, kIoPoll, kSqPollIoPoll };
struct ProactorMode {
  ProactorModeKind kind{ProactorModeKind::kEventDriven};
  EventDrivenConfig event_driven{};
  SqPollConfig sq_poll{};
  [[nodiscard]] static constexpr ProactorMode EventDriven(EventDrivenConfig cfg = {}) noexcept {
    return {ProactorModeKind::kEventDriven, cfg, {}};
  }
  [[nodiscard]] static constexpr ProactorMode SqPoll(SqPollConfig cfg = {}) noexcept {
    return {ProactorModeKind::kSqPoll, {}, cfg};
  }
  [[nodiscard]] static constexpr ProactorMode IoPoll() noexcept { return {ProactorModeKind::kIoPoll, {}, {}}; }
  [[nodiscard]] static constexpr ProactorMode SqPollIoPoll(SqPollConfig cfg = {}) noexcept {
    return {ProactorModeKind::kSqPollIoPoll, {}, cfg};
  }
  [[nodiscard]] constexpr bool IsBusyPoll() const noexcept { return kind != ProactorModeKind::kEventDriven; }
  [[nodiscard]] constexpr bool UsesSqPoll() const noexcept {
    return kind == ProactorModeKind::kSqPoll || kind == ProactorModeKind::kSqPollIoPoll;
  }
  [[nodiscard]] constexpr bool UsesIoPoll() const noexcept {
    return kind == ProactorModeKind::kIoPoll || kind == ProactorModeKind::kSqPollIoPoll;
  }
  [[nodiscard]] constexpr bool AdmitsOpcode(std::uint8_t opcode) const noexcept {
    return !UsesIoPoll() || OpcodeSet::kIoPoll.Contains(opcode);
  }
};
struct ProactorConfig {
  std::uint32_t entries = 256;
  std::optional<std::uint32_t> cq_entries = std::nullopt;
  ProactorMode mode = ProactorMode::EventDriven();
  RingMode geometry = RingMode::kStandard64_16;
  std::optional<std::chrono::nanoseconds> shutdown_stall_timeout = std::nullopt;
  std::uint32_t setup_flags = 0;
  bool enable_optimizations = true;
  [[nodiscard]] constexpr Result<void> Validate() const noexcept {
    if (static_cast<unsigned>(geometry) > 2 || entries == 0 || (cq_entries && *cq_entries < entries) ||
        (setup_flags & ~(abi::kSetupClamp | abi::kSetupSubmitAll | abi::kSetupNoSqArray)) != 0 ||
        (mode.IsBusyPoll() && !enable_optimizations) || static_cast<unsigned>(mode.kind) > 3 ||
        (shutdown_stall_timeout && shutdown_stall_timeout->count() < 0))
      return std::unexpected(UringErrorCode::kInvalid);
    return {};
  }
  [[nodiscard]] Result<void> ValidateAffinity() const noexcept {
    if (!mode.UsesSqPoll() || !mode.sq_poll.thread_cpu) return {};
    const auto cpu = *mode.sq_poll.thread_cpu;
    if (cpu >= CPU_SETSIZE) return std::unexpected(UringErrorCode::kInvalid);
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return std::unexpected(UringErrorCode::kInvalid);
    if (CPU_COUNT(&allowed) == 1 && CPU_ISSET(cpu, &allowed)) return std::unexpected(UringErrorCode::kInvalid);
    return {};
  }
};
namespace detail {
template <class Open>
[[nodiscard]] auto NegotiateSetup(const ProactorConfig& config, Open&& open) noexcept
    -> std::invoke_result_t<Open&, abi::IoUringParams> {
  using Return = std::invoke_result_t<Open&, abi::IoUringParams>;
  std::array<std::uint32_t, 4> ladder{};
  std::size_t count = 1;
  if (config.mode.UsesSqPoll())
    ladder[0] = abi::kSetupSqPoll | abi::kSetupSingleIssuer | (config.mode.sq_poll.thread_cpu ? abi::kSetupSqAff : 0U) |
                (config.mode.UsesIoPoll() ? abi::kSetupIoPoll : 0U);
  else if (config.mode.UsesIoPoll()) {
    ladder = {abi::kSetupIoPoll | abi::kSetupSingleIssuer | abi::kSetupDeferTaskrun | abi::kSetupTaskrunFlag,
              abi::kSetupIoPoll | abi::kSetupSingleIssuer, abi::kSetupIoPoll, 0};
    count = 3;
  } else if (config.enable_optimizations) {
    ladder = {abi::kSetupSingleIssuer | abi::kSetupCoopTaskrun | abi::kSetupDeferTaskrun | abi::kSetupTaskrunFlag,
              abi::kSetupSingleIssuer | abi::kSetupCoopTaskrun | abi::kSetupTaskrunFlag,
              abi::kSetupCoopTaskrun | abi::kSetupTaskrunFlag, 0};
    count = 4;
  }
  UringErrorCode error = UringErrorCode::kInvalid;
  for (std::size_t i = 0; i < count; ++i) {
    for (unsigned modifiers = config.enable_optimizations ? 2U : 1U; modifiers != 0; --modifiers) {
      abi::IoUringParams params{};
      params.flags = ladder[i] | config.setup_flags | ring::ModeSetupFlags(config.geometry) |
                     (modifiers == 2 ? abi::kSetupSubmitAll | abi::kSetupNoSqArray : 0U);
      if (config.cq_entries) {
        params.flags |= abi::kSetupCqSize;
        params.cq_entries = *config.cq_entries;
      }
      params.sq_thread_idle = config.mode.sq_poll.thread_idle_ms;
      params.sq_thread_cpu = config.mode.sq_poll.thread_cpu.value_or(0);
      auto ring = open(params);
      if (ring) return ring;
      error = ring.error();
      if (error != UringErrorCode::kInvalid && error != UringErrorCode::kNotSupported) return Return{std::unexpected(error)};
    }
  }
  return Return{std::unexpected(error)};
}
[[nodiscard]] inline Result<KernelRing> OpenConfiguredRing(const ProactorConfig& config) noexcept {
  auto version = caps::VerifyKernelVersion();
  if (!version) return std::unexpected(version.error());
  return NegotiateSetup(
      config, [&](abi::IoUringParams params) noexcept { return KernelRing::CreateWithParams(config.entries, params); });
}
struct LoopRequest {
  bool submit = true;
  std::uint32_t minimum = 0;
  std::optional<std::chrono::nanoseconds> timeout = std::nullopt;
  bool harvest_count = false;
};
struct ConfiguredTag {};
template <class Ring>
[[nodiscard]] Result<std::uint32_t> EnterConfigured(Ring& ring, const ProactorConfig& config, std::uint32_t minimum,
                                                    std::optional<std::chrono::nanoseconds> timeout) noexcept {
  auto& sq = ring.Sq();
  const auto published = sq.UnflushedCount() != 0 ? sq.FlushTail() : 0U;
  const bool already_published = published != 0 || sq.FlushedTail() != sq.Head();
  if (config.mode.UsesSqPoll()) {
    if (published != 0) std::atomic_thread_fence(std::memory_order_seq_cst);
    if constexpr (std::same_as<Ring, KernelRing>) {
      if (sq.NeedsWakeup() && already_published) {
        auto result = ToResult(sys::SysIoUringEnter(ring.Fd(), 0, 0, abi::kEnterSqWakeup, nullptr, 0));
        if (!result && result.error() != UringErrorCode::kInterrupted && result.error() != UringErrorCode::kAgain &&
            result.error() != UringErrorCode::kBusy)
          return std::unexpected(result.error());
      }
      sq.CompleteSubmit(0, true);
      return published;
    }
  }
  abi::KernelTimespec time{};
  abi::IoUringGeteventsArg arg{};
  std::uint32_t flags = config.mode.UsesIoPoll() ? abi::kEnterGetevents : 0;
  std::uint32_t features = 0;
  if constexpr (requires { ring.Params(); }) features = ring.Params().features;
  if (config.mode.event_driven.no_iowait && (features & abi::kFeatNoIowait) != 0) flags |= abi::kEnterNoIowait;
  if (timeout) {
    time = {timeout->count() / 1000000000, timeout->count() % 1000000000};
    arg.ts = sys::Address(&time);
  }
  if ((features & abi::kFeatMinTimeout) != 0) arg.min_wait_usec = config.mode.event_driven.batch_wait_usec;
  if (timeout || arg.min_wait_usec != 0) flags |= abi::kEnterExtArg;
  if (timeout) flags |= abi::kEnterGetevents;
  auto entered = [&]() noexcept {
    if constexpr (requires { ring.SubmitAndWait(minimum, flags, &arg, sizeof(arg)); })
      return ring.SubmitAndWait(minimum, flags, (flags & abi::kEnterExtArg) ? &arg : nullptr,
                                (flags & abi::kEnterExtArg) ? sizeof(arg) : 0);
    else
      return ring.SubmitAndWait(minimum);
  }();
  if (!entered) {
    if (already_published && (entered.error() == UringErrorCode::kInterrupted || entered.error() == UringErrorCode::kAgain ||
                              entered.error() == UringErrorCode::kBusy))
      return published;
    if (entered.error() == UringErrorCode::kTimerExpired) return std::unexpected(UringErrorCode::kTimedOut);
    return std::unexpected(entered.error());
  }
  return published;
}
template <class Ring>
[[nodiscard]] Result<void> PollKernelOnly(Ring& ring, std::uint32_t minimum = 0,
                                          std::optional<std::chrono::nanoseconds> timeout = std::nullopt) noexcept {
  abi::KernelTimespec time{};
  abi::IoUringGeteventsArg arg{};
  auto flags = abi::kEnterGetevents;
  if (timeout) {
    time = {timeout->count() / 1000000000, timeout->count() % 1000000000};
    arg.ts = sys::Address(&time);
    flags |= abi::kEnterExtArg;
  }
  auto polled = [&]() noexcept -> Result<std::uint32_t> {
    if constexpr (requires { ring.PollKernelCompletions(minimum, flags, &arg, sizeof(arg)); }) {
      return ring.PollKernelCompletions(minimum, flags, timeout ? &arg : nullptr, timeout ? sizeof(arg) : 0);
    } else if constexpr (std::same_as<Ring, ScriptedRing>) {
      if (timeout) {
        static_cast<void>(sys::Syscall6(sys::kSysNanosleep, sys::Address(&time), 0));
        return std::unexpected(UringErrorCode::kTimedOut);
      }
      return 0U;
    } else
      return std::unexpected(UringErrorCode::kNotSupported);
  }();
  if (!polled) {
    if (polled.error() == UringErrorCode::kInterrupted || polled.error() == UringErrorCode::kAgain ||
        polled.error() == UringErrorCode::kBusy)
      return {};
    if (polled.error() == UringErrorCode::kTimerExpired) return std::unexpected(UringErrorCode::kTimedOut);
    return std::unexpected(polled.error());
  }
  return {};
}
template <class Derived>
class ProactorLoops {
 protected:
  ProactorConfig config_{};
  bool bounded_shutdown_{};
  ProactorLoops() noexcept = default;
  explicit ProactorLoops(ProactorConfig config) noexcept : config_(config) {}

 private:
  [[nodiscard]] Derived& Self() noexcept { return static_cast<Derived&>(*this); }

 public:
  [[nodiscard]] const ProactorConfig& Config() const noexcept { return config_; }
  [[nodiscard]] Result<std::uint32_t> Submit() noexcept {
    auto& self = Self();
    if (self.driving_ || self.in_callback_dispatch_) return std::unexpected(UringErrorCode::kReentrantDrive);
    if (self.Stage() == DrainStage::kClosed) return 0;
    auto replay = self.Replay();
    auto submitted = EnterConfigured(self.Backend(), config_, 0, std::nullopt);
    if (!replay) return std::unexpected(replay.error());
    return submitted;
  }
  template <class Dispatcher>
  [[nodiscard]] Result<std::uint32_t> PollCompletions(Dispatcher&& dispatcher) noexcept {
    auto result = Self().Drive(DriveMode::kPoll, dispatcher, LoopRequest{false, 0, std::nullopt, true});
    if (!result) return std::unexpected(result.error());
    return static_cast<std::uint32_t>(*result);
  }
  template <class Dispatcher>
  [[nodiscard]] Result<std::uint32_t> Tick(Dispatcher&& dispatcher) noexcept {
    auto& self = Self();
    if (self.driving_ || self.in_callback_dispatch_) return std::unexpected(UringErrorCode::kReentrantDrive);
    if (self.InFlight() == 0 && self.DeferredCount() == 0 && self.internal_in_flight_ == 0 &&
        self.Backend().Sq().UnflushedCount() == 0 && self.Backend().Cq().Available() == 0 &&
        self.Stage() == DrainStage::kRunning && self.state_->controls.Empty())
      return 0;
    auto result = self.Drive(DriveMode::kPoll, dispatcher, LoopRequest{true, 0, std::nullopt, true});
    if (!result) return std::unexpected(result.error());
    return static_cast<std::uint32_t>(*result);
  }
  template <class Dispatcher>
  [[nodiscard]] Result<std::uint32_t> WaitCompletions(std::uint32_t minimum, std::optional<std::chrono::nanoseconds> timeout,
                                                      Dispatcher&& dispatcher) noexcept {
    if (minimum > Derived::kBatch || minimum > Self().Backend().Cq().RingEntries())
      return std::unexpected(UringErrorCode::kInvalid);
    if (config_.mode.IsBusyPoll() && (minimum != 0 || timeout)) return std::unexpected(UringErrorCode::kBusy);
    if (timeout && timeout->count() < 0) return std::unexpected(UringErrorCode::kInvalid);
    if (minimum == 0 && !timeout) return Tick(dispatcher);
    auto result = Self().Drive(DriveMode::kPoll, dispatcher, LoopRequest{true, minimum, timeout, true});
    if (!result) return std::unexpected(result.error());
    return static_cast<std::uint32_t>(*result);
  }
  template <class Dispatcher>
  [[nodiscard]] Result<void> ShutdownWith(Dispatcher&& dispatcher,
                                          std::optional<std::chrono::nanoseconds> stall = std::nullopt) noexcept {
    auto& self = Self();
    if (self.driving_ || self.in_callback_dispatch_) return std::unexpected(UringErrorCode::kReentrantDrive);
    if (self.Stage() == DrainStage::kClosed) return {};
    auto budget = stall.value_or(config_.shutdown_stall_timeout.value_or(
        config_.mode.UsesIoPoll() ? std::chrono::seconds(5) : std::chrono::milliseconds(320)));
    if (budget.count() < 0) return std::unexpected(UringErrorCode::kInvalid);
    // A bounded shutdown cannot wait indefinitely inside synchronous cancellation.
    struct Reset {
      bool& flag;
      ~Reset() noexcept { flag = false; }
    } reset{bounded_shutdown_};
    bounded_shutdown_ = true;
    auto started = self.BeginDrain();
    if (!started) return started;
    auto last_progress = std::chrono::steady_clock::now();
    while (self.Stage() != DrainStage::kQuiescent) {
      const auto before = self.Backend().Cq().SharedHead();
      auto progress = Tick(dispatcher);
      if (!progress && progress.error() != UringErrorCode::kNoEntry) return std::unexpected(progress.error());
      const bool harvested = (progress && *progress != 0) || self.Backend().Cq().SharedHead() != before;
      if (harvested) last_progress = std::chrono::steady_clock::now();
      if (self.Stage() == DrainStage::kQuiescent) break;
      const auto elapsed = std::chrono::steady_clock::now() - last_progress;
      if (elapsed >= budget) return std::unexpected(UringErrorCode::kTimedOut);
      if (!harvested && config_.mode.kind == ProactorModeKind::kEventDriven && self.Backend().Sq().UnflushedCount() == 0) {
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(budget - elapsed);
        auto waited = PollKernelOnly(self.Backend(), 1, std::min(remaining, std::chrono::nanoseconds(10000000)));
        if (!waited && waited.error() != UringErrorCode::kTimedOut) return waited;
      }
    }
    return self.Close();
  }
  [[nodiscard]] Result<void> Shutdown() noexcept {
    struct Ignore {
      [[nodiscard]] Result<void> DispatchBatch(std::span<const Delivery>) noexcept { return {}; }
    } sink;
    return ShutdownWith(sink);
  }
};
}  // namespace detail
template <class RingBackend, unsigned LogCapacity, unsigned LogDeferredCapacity>
class BasicProactor;
class ProactorBuilder {
  ProactorConfig config_{};

 public:
  ProactorBuilder& Entries(std::uint32_t value) noexcept {
    config_.entries = value;
    return *this;
  }
  ProactorBuilder& CqEntries(std::uint32_t value) noexcept {
    config_.cq_entries = value;
    return *this;
  }
  ProactorBuilder& Mode(ProactorMode value) noexcept {
    config_.mode = value;
    return *this;
  }
  ProactorBuilder& Geometry(RingMode value) noexcept {
    config_.geometry = value;
    return *this;
  }
  ProactorBuilder& ShutdownStallTimeout(std::chrono::nanoseconds value) noexcept {
    config_.shutdown_stall_timeout = value;
    return *this;
  }
  ProactorBuilder& SetupFlags(std::uint32_t value) noexcept {
    config_.setup_flags = value;
    return *this;
  }
  ProactorBuilder& EnableOptimizations(bool value) noexcept {
    config_.enable_optimizations = value;
    return *this;
  }
  template <class Engine = BasicProactor<KernelRing, 11, 8>>
  [[nodiscard]] std::expected<Engine, SyscallError> Build() const noexcept {
    return Engine::CreateConfigured(config_);
  }
  template <class Engine = BasicProactor<ScriptedRing, 11, 8>>
  [[nodiscard]] std::expected<Engine, SyscallError> BuildScripted() const noexcept {
    return Engine::CreateConfigured(config_);
  }
};
}  // namespace liburing
