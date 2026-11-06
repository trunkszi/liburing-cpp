#include <cstdio>
#include <liburing/caps/kernel_caps.hpp>
#include <liburing/config/proactor_config.hpp>
#include <liburing/core/proactor.hpp>
#include <liburing/core/route_dispatcher.hpp>
#include <liburing/ops/fs.hpp>
#include <liburing/ops/futex.hpp>
#include <liburing/ops/net.hpp>
#include <thread>

#include "../../support/host_kernel.hpp"
using namespace liburing;
int BasicModes() {
  ProactorConfig config;
  config.setup_flags = abi::kSetupSqPoll;
  if (config.Validate()) return 1;
  config.setup_flags = 0;
  config.mode = ProactorMode::IoPoll();
  config.enable_optimizations = false;
  if (config.Validate()) return 2;
  config.enable_optimizations = true;
  config.cq_entries = 1;
  if (config.Validate()) return 3;
  for (auto mode :
       {ProactorMode::EventDriven(), ProactorMode::SqPoll(), ProactorMode::IoPoll(), ProactorMode::SqPollIoPoll()}) {
    auto engine = Proactor::Builder().Entries(8).Mode(mode).BuildScripted();
    if (!engine) return 4;
    auto sink = MakeFnDispatcher([](const Delivery&) noexcept {});
    if (!engine->Tick(sink) || engine->Backend().Sq().Tail() != 0) return 5;
    if (mode.IsBusyPoll()) {
      auto wait = engine->WaitCompletions(1, std::nullopt, sink);
      if (wait || wait.error() != UringErrorCode::kBusy) return 6;
    }
    if (!engine->WaitCompletions(0, std::nullopt, sink)) return 7;
    auto rejected = engine->Arm(1, ops::PrepareRecv(-1, nullptr, 0));
    if (mode.UsesIoPoll() ? rejected.has_value() : !rejected.has_value()) return 8;
    if (rejected) {
      engine->Backend().ScriptCqe(*rejected, 0);
      if (!engine->Tick(sink)) return 9;
    }
    if (mode.UsesIoPoll()) {
      abi::KernelTimespec time{};
      std::uint32_t word = 0;
      if (engine->Arm(1, ops::PrepareTimeout(&time, 0)) || engine->Arm(1, ops::PrepareFutexWait(&word, 0)) ||
          engine->Arm(1, ops::PrepareAsyncCancel(kNullToken)) ||
          engine->ArmChain(1, ops::PrepareNop(), ops::PrepareRecv(-1, nullptr, 0)) || engine->InFlight() != 0)
        return 12;
    }
    if (!engine->Arm(1, ops::PrepareNop()) || !engine->Tick(sink) || !engine->ShutdownWith(sink)) return 10;
  }
  if (const int skip = test_support::SkipBelowKernel(6, 16); skip >= 0) return skip;
  auto kernel = Proactor::Builder().Entries(8).Build();
  if (!kernel || !kernel->Shutdown()) return 11;
  return 0;
}

struct CountingRing {
  ScriptedRing ring{8, 16};
  unsigned enters{};
  UringErrorCode fail{UringErrorCode::kInvalid};
  bool reject{};
  std::uint32_t last_flags{};
  unsigned sync_cancels{};
  bool bounded_cancel{true};
  unsigned private_waits{};
  bool valid_wait_slice{true};
  [[nodiscard]] Result<std::uint32_t> PollKernelCompletions(std::uint32_t minimum, std::uint32_t flags, const void* arg,
                                                            std::size_t size) noexcept {
    ++private_waits;
    if (minimum != 1 || flags != (abi::kEnterGetevents | abi::kEnterExtArg) || size != sizeof(abi::IoUringGeteventsArg) ||
        !arg) {
      valid_wait_slice = false;
      return 0U;
    }
    const auto* wait = static_cast<const abi::IoUringGeteventsArg*>(arg);
    const auto* time = reinterpret_cast<const abi::KernelTimespec*>(static_cast<std::uintptr_t>(wait->ts));
    valid_wait_slice &= time && time->tv_sec == 0 && time->tv_nsec > 0 && time->tv_nsec <= 10000000;
    if (time) std::this_thread::sleep_for(std::chrono::nanoseconds(time->tv_nsec));
    return std::unexpected(UringErrorCode::kTimerExpired);
  }
  [[nodiscard]] auto& Sq() noexcept { return ring.Sq(); }
  [[nodiscard]] const auto& Sq() const noexcept { return ring.Sq(); }
  [[nodiscard]] auto& Cq() noexcept { return ring.Cq(); }
  [[nodiscard]] auto CqMode() const noexcept { return ring.CqMode(); }
  [[nodiscard]] const auto& Params() const noexcept { return ring.Params(); }
  [[nodiscard]] Result<std::uint32_t> SubmitAndWait(std::uint32_t minimum, std::uint32_t flags = 0, const void* arg = nullptr,
                                                    std::size_t size = 0) noexcept {
    ++enters;
    last_flags = flags;
    if (reject) return std::unexpected(fail);
    return ring.SubmitAndWait(minimum, flags, arg, size);
  }
  [[nodiscard]] auto Submit() noexcept { return SubmitAndWait(0); }
  [[nodiscard]] auto Register(std::uint32_t opcode, const void* arg, std::uint32_t size) noexcept {
    if (opcode == abi::kRegisterSyncCancel) {
      ++sync_cancels;
      const auto* cancel = static_cast<const abi::IoUringSyncCancelReg*>(arg);
      bounded_cancel &= cancel->timeout.tv_sec == 0 && cancel->timeout.tv_nsec == 0;
    }
    return ring.Register(opcode, arg, size);
  }
};
int IdleAndPublication() {
  BasicProactor<CountingRing> engine{std::in_place};
  unsigned count = 0;
  auto sink = MakeFnDispatcher([&](const Delivery&) noexcept { ++count; });
  for (int i = 0; i < 32; ++i)
    if (!engine.Tick(sink)) return 20;
  if (engine.Backend().enters != 0 || engine.Backend().Sq().SharedTail() != 0) return 21;
  if (!engine.Arm(1, ops::PrepareNop())) return 22;
  auto submitted = engine.Submit();
  if (!submitted || *submitted != 1 || engine.Backend().enters != 1) return 23;
  if (!engine.PollCompletions(sink) || engine.Backend().enters != 1 || count != 1) return 24;
  auto token = engine.Arm(1, ops::PrepareNop());
  if (!token) return 25;
  engine.Backend().reject = true;
  engine.Backend().fail = UringErrorCode::kAgain;
  submitted = engine.Submit();
  if (!submitted || *submitted != 1 || engine.Backend().Sq().UnflushedCount() != 0) return 26;
  submitted = engine.Submit();
  if (!submitted || *submitted != 0) return 29;
  engine.Backend().reject = false;
  if (!engine.Tick(sink) || engine.InFlight() != 0 || count != 2) return 27;
  engine.Backend().ring.ScriptCqe(kNullToken, 0);
  auto ignored = engine.PollCompletions(sink);
  if (!ignored || *ignored != 1 || count != 2) return 73;
  return engine.Shutdown() ? 0 : 28;
}
int ShutdownAndWait() {
  auto engine = Proactor::Builder().Entries(8).Mode(ProactorMode::IoPoll()).BuildScripted();
  if (!engine) return 30;
  engine->Backend().SetAutoCompleteNops(false);
  auto token = engine->Arm(1, ops::PrepareRead(-1, 0, 0));
  if (!token) return 31;
  auto sink = MakeFnDispatcher([](const Delivery&) noexcept {});
  const auto unpublished = engine->Backend().Sq().UnflushedCount();
  auto wait = engine->WaitCompletions(1, std::chrono::milliseconds(1), sink);
  if (wait || wait.error() != UringErrorCode::kBusy || engine->Backend().Sq().UnflushedCount() != unpublished) return 32;
  auto stalled = engine->ShutdownWith(sink, std::chrono::nanoseconds(0));
  if (stalled || stalled.error() != UringErrorCode::kTimedOut || !engine->Contains(*token)) return 33;
  engine->Backend().ScriptCqe(*token, 0);
  if (!engine->ShutdownWith(sink)) return 34;
  if (const int skip = test_support::SkipBelowKernel(6, 16); skip >= 0) return skip;
  auto kernel = Proactor::Builder().Entries(8).EnableOptimizations(false).Build();
  if (!kernel) return 35;
  auto timed = kernel->WaitCompletions(1, std::chrono::milliseconds(1), sink);
  if (timed || timed.error() != UringErrorCode::kTimedOut) return 36;
  return kernel->Shutdown() ? 0 : 37;
}
int BoundedFallback() {
  BasicProactor<CountingRing> engine{std::in_place};
  engine.Backend().ring.SetAutoCompleteNops(false);
  engine.Backend().ring.SetConsumeSqOnSubmit(false);
  std::array<UserDataToken, 8> tokens{};
  for (auto& token : tokens) {
    auto armed = engine.Arm(1, ops::PrepareNop());
    if (!armed) return 70;
    token = *armed;
  }
  auto sink = MakeFnDispatcher([](const Delivery&) noexcept {});
  auto result = engine.ShutdownWith(sink, std::chrono::nanoseconds(0));
  if (result || result.error() != UringErrorCode::kTimedOut || !engine.Backend().bounded_cancel ||
      engine.Backend().sync_cancels != 8)
    return 71;
  engine.Backend().ring.AdvanceSqHead(8);
  for (auto token : tokens) engine.Backend().ring.ScriptCqe(token, -125);
  return engine.ShutdownWith(sink) ? 0 : 72;
}
int AffinityAndKernelModes() {
  if (const int skip = test_support::SkipBelowKernel(6, 16); skip >= 0) return skip;
  cpu_set_t original;
  if (sched_getaffinity(0, sizeof(original), &original) != 0) return 40;
  struct Restore {
    cpu_set_t mask;
    ~Restore() { static_cast<void>(sched_setaffinity(0, sizeof(mask), &mask)); }
  } restore{original};
  unsigned first = 0;
  while (first < CPU_SETSIZE && !CPU_ISSET(first, &original)) ++first;
  if (first == CPU_SETSIZE) return 41;
  cpu_set_t sole;
  CPU_ZERO(&sole);
  CPU_SET(first, &sole);
  if (sched_setaffinity(0, sizeof(sole), &sole) != 0) return 42;
  auto rejected = Proactor::Builder().Entries(8).Mode(ProactorMode::SqPoll({1000, first})).Build();
  if (rejected || rejected.error().code() != UringErrorCode::kInvalid) return 43;
  if (sched_setaffinity(0, sizeof(original), &original) != 0) return 44;
  ProactorConfig config;
  config.mode = ProactorMode::SqPoll({1000, first});
  if (CPU_COUNT(&original) > 1 && !config.ValidateAffinity()) return 45;
  const auto sq_mode = CPU_COUNT(&original) > 1 ? ProactorMode::SqPoll(SqPollConfig{1000, first}) : ProactorMode::SqPoll();
  for (auto mode : {sq_mode, ProactorMode::IoPoll(), ProactorMode::SqPollIoPoll()}) {
    auto engine = Proactor::Builder().Entries(8).Mode(mode).Build();
    if (!engine) return 46;
    const auto flags = engine->Backend().Params().flags;
    if (mode.UsesSqPoll() != ((flags & abi::kSetupSqPoll) != 0) || mode.UsesIoPoll() != ((flags & abi::kSetupIoPoll) != 0))
      return 47;
    auto sink = MakeFnDispatcher([](const Delivery&) noexcept {});
    if (!engine->Arm(1, ops::PrepareNop())) return 48;
    for (unsigned spin = 0; engine->InFlight() != 0 && spin < 100000; ++spin)
      if (!engine->Tick(sink)) return 49;
    if (!engine->Shutdown()) return 50;
  }
  return 0;
}
int Negotiation() {
  ProactorConfig config;
  std::array<std::uint32_t, 8> seen{};
  unsigned count = 0;
  auto result = detail::NegotiateSetup(config, [&](abi::IoUringParams params) noexcept -> Result<std::uint32_t> {
    seen[count++] = params.flags;
    if (count == 8) return params.flags;
    return std::unexpected(UringErrorCode::kInvalid);
  });
  if (!result || *result != 0 || count != 8) return 60;
  const auto optimal = abi::kSetupSingleIssuer | abi::kSetupCoopTaskrun | abi::kSetupDeferTaskrun | abi::kSetupTaskrunFlag;
  if (seen[0] != (optimal | abi::kSetupSubmitAll | abi::kSetupNoSqArray) || seen[1] != optimal ||
      seen[3] != (abi::kSetupSingleIssuer | abi::kSetupCoopTaskrun | abi::kSetupTaskrunFlag) ||
      seen[5] != (abi::kSetupCoopTaskrun | abi::kSetupTaskrunFlag) || seen[7] != 0)
    return 61;
  count = 0;
  result = detail::NegotiateSetup(config, [&](abi::IoUringParams) noexcept -> Result<std::uint32_t> {
    ++count;
    return std::unexpected(UringErrorCode::kPermissionDenied);
  });
  if (result || count != 1) return 62;
  config.mode = ProactorMode::IoPoll();
  count = 0;
  result = detail::NegotiateSetup(config, [&](abi::IoUringParams params) noexcept -> Result<std::uint32_t> {
    ++count;
    if ((params.flags & abi::kSetupIoPoll) == 0) return std::unexpected(UringErrorCode::kIo);
    if (count == 6) return params.flags;
    return std::unexpected(UringErrorCode::kInvalid);
  });
  return result && *result == abi::kSetupIoPoll && count == 6 ? 0 : 63;
}
int CheckFlaggedPoll(Proactor& engine) {
  unsigned seen = 0;
  auto sink = MakeFnDispatcher([&](const Delivery&) noexcept { ++seen; });
  abi::KernelTimespec time{0, 1000000};
  if (!engine.Arm(1, ops::PrepareTimeout(&time, 0)) || !engine.Submit()) return 81;
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (!engine.Arm(1, ops::PrepareNop())) return 82;
  const auto tail = engine.Backend().Sq().SharedTail();
  for (int i = 0; i < 20 && seen == 0; ++i) {
    if (!engine.PollCompletions(sink)) return 83;
    if (seen == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool poll_ok = seen == 1 && engine.Backend().Sq().UnflushedCount() == 1 && engine.Backend().Sq().SharedTail() == tail;
  if (!engine.Tick(sink) || !engine.ShutdownWith(sink)) return 84;
  return poll_ok && seen == 2 ? 0 : 85;
}
int FlaggedPoll() {
  if (const int skip = test_support::SkipBelowKernel(6, 16); skip >= 0) return skip;
  auto engine = Proactor::Builder().Entries(8).Build();
  if (!engine) return 80;
  return CheckFlaggedPoll(*engine);
}
int RegisteredFlaggedPoll() {
  auto backend = KernelRing::CreateNoMmap(8, RingMode::kStandard64_16, abi::kSetupRegisteredFdOnly | abi::kSetupTaskrunFlag);
  if (!backend) return 86;
  Proactor engine{std::move(*backend)};
  if (engine.Backend().Fd() != -1 || !engine.Backend().HasRegisteredFd()) return 87;
  return CheckFlaggedPoll(engine);
}
struct FlagRing {
  std::uint32_t head{}, tail{}, flags{}, dropped{};
  std::array<abi::FlatSqe, 8> sqes{};
  ring::SqCursor sq{&head, &tail, 7, 8, &flags, &dropped, sqes.data(), RingMode::kStandard64_16};
  ScriptedRing completions{8, 16};
  UserDataToken pending{};
  unsigned submits{}, polls{};
  bool interrupt{};
  [[nodiscard]] auto& Sq() noexcept { return sq; }
  [[nodiscard]] const auto& Sq() const noexcept { return sq; }
  [[nodiscard]] auto& Cq() noexcept { return completions.Cq(); }
  [[nodiscard]] auto CqMode() const noexcept { return completions.CqMode(); }
  [[nodiscard]] const auto& Params() const noexcept { return completions.Params(); }
  [[nodiscard]] Result<std::uint32_t> SubmitAndWait(std::uint32_t, std::uint32_t = 0, const void* = nullptr,
                                                    std::size_t = 0) noexcept {
    ++submits;
    const auto count = sq.ConsumePendingSubmitCount();
    head = tail;
    return count;
  }
  [[nodiscard]] auto Submit() noexcept { return SubmitAndWait(0); }
  [[nodiscard]] auto Register(std::uint32_t op, const void* arg, std::uint32_t count) noexcept {
    return completions.Register(op, arg, count);
  }
  [[nodiscard]] Result<std::uint32_t> PollKernelCompletions(std::uint32_t minimum, std::uint32_t enter_flags, const void*,
                                                            std::size_t) noexcept {
    ++polls;
    if (minimum != 0 || enter_flags != abi::kEnterGetevents) return std::unexpected(UringErrorCode::kInvalid);
    if (interrupt) return std::unexpected(UringErrorCode::kInterrupted);
    flags = 0;
    completions.ScriptCqe(pending, 0);
    return 0U;
  }
};
int PollFlagsAndBounds() {
  for (auto flag : {abi::kSqTaskrun, abi::kSqCqOverflow}) {
    BasicProactor<FlagRing> engine{std::in_place};
    auto first = engine.Arm(1, ops::PrepareNop());
    if (!first || !engine.Submit()) return 90;
    auto second = engine.Arm(1, ops::PrepareNop());
    if (!second) return 91;
    engine.Backend().flags = flag;
    engine.Backend().pending = *first;
    engine.Backend().interrupt = true;
    unsigned seen = 0;
    auto sink = MakeFnDispatcher([&](const Delivery&) noexcept { ++seen; });
    auto interrupted = engine.PollCompletions(sink);
    if (!interrupted || *interrupted != 0 || engine.Backend().submits != 1 || engine.Backend().Sq().SharedTail() != 1)
      return 92;
    engine.Backend().interrupt = false;
    auto delivered = engine.PollCompletions(sink);
    if (!delivered || *delivered != 1 || seen != 1 || engine.Backend().submits != 1 ||
        engine.Backend().Sq().UnflushedCount() != 1)
      return 93;
    auto oversized = engine.WaitCompletions(17, std::chrono::nanoseconds(0), sink);
    if (oversized || oversized.error() != UringErrorCode::kInvalid || engine.Backend().submits != 1) return 94;
    if (!engine.Submit()) return 95;
    engine.Backend().completions.ScriptCqe(*second, 0);
    if (!engine.PollCompletions(sink) || !engine.Shutdown()) return 96;
  }
  auto small = Proactor::Builder().Entries(2).CqEntries(4).BuildScripted();
  auto sink = MakeFnDispatcher([](const Delivery&) noexcept {});
  if (!small) return 97;
  auto oversized = small->WaitCompletions(5, std::chrono::nanoseconds(0), sink);
  if (oversized || oversized.error() != UringErrorCode::kInvalid || !small->Shutdown()) return 97;
  BasicProactor<CountingRing> stalled{std::in_place};
  auto token = stalled.Arm(1, ops::PrepareRead(-1, 0, 0));
  if (!token) return 98;
  auto shutdown = stalled.ShutdownWith(sink, std::chrono::milliseconds(35));
  if (shutdown || shutdown.error() != UringErrorCode::kTimedOut || stalled.Backend().private_waits == 0 ||
      !stalled.Backend().valid_wait_slice)
    return 99;
  stalled.Backend().ring.ScriptCqe(*token, -125);
  stalled.Backend().ring.ScriptCqe(stalled.InternalCancelToken(), 0);
  return stalled.ShutdownWith(sink) ? 0 : 100;
}
int main() {
  if (auto result = PollFlagsAndBounds()) return result;
  if (auto result = FlaggedPoll()) return result;
  if (auto result = RegisteredFlaggedPoll()) return result;
  if (auto result = BasicModes()) return result;
  if (auto result = IdleAndPublication()) return result;
  if (auto result = ShutdownAndWait()) return result;
  if (auto result = BoundedFallback()) return result;
  if (auto result = AffinityAndKernelModes()) return result;
  return Negotiation();
}
