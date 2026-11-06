#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <liburing/liburing.hpp>

using namespace liburing;
namespace {
[[nodiscard]] bool RunMode(ProactorMode mode, const char* name) noexcept {
  auto engine = Proactor::Builder().Entries(16).Mode(mode).Build();
  if (!engine) {
    const auto error = engine.error().code();
    if (mode.IsBusyPoll() && (error == UringErrorCode::kPermissionDenied || error == UringErrorCode::kNotSupported ||
                              error == UringErrorCode::kInvalid)) {
      std::printf("%s unavailable: %s\n", name, ErrorName(error).data());
      return true;
    }
    return false;
  }
  auto sink = MakeFnDispatcher([](const Delivery&) noexcept {});
  bool success =
      engine->Config().mode.kind == mode.kind && engine->Tick(sink).has_value() && engine->PollCompletions(sink).has_value();
  if (mode.IsBusyPoll()) {
    auto blocked = engine->WaitCompletions(1, std::nullopt, sink);
    success &= !blocked && blocked.error() == UringErrorCode::kBusy;
  }
  if (!mode.UsesIoPoll()) {
    unsigned completed = 0;
    auto counter = MakeFnDispatcher([&](const Delivery& delivery) noexcept {
      if (delivery.completion.result == 0) ++completed;
    });
    success &= engine->Arm(1, ops::PrepareNop()).has_value() && engine->Submit().has_value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (success && completed == 0 && std::chrono::steady_clock::now() < deadline) {
      auto result =
          mode.IsBusyPoll() ? engine->Tick(counter) : engine->WaitCompletions(1, std::chrono::milliseconds(50), counter);
      success = result || result.error() == UringErrorCode::kTimedOut;
    }
    success &= completed == 1;
  }
  const auto negotiated_flags = engine->Backend().Params().flags;
  const auto shutdown = engine->ShutdownWith(sink);
  std::printf("%s: negotiated flags=0x%x, shutdown=%s\n", name, negotiated_flags, shutdown ? "ok" : "failed");
  return success && shutdown;
}
[[nodiscard]] bool DirectBuffers() noexcept {
  char path[] = "/tmp/liburing-cpp-example-XXXXXX";
  auto temporary = sys::UniqueFd::Adopt(::mkstemp(path));
  if (!temporary) return false;
  auto file = io::DirectFile::Open(path, io::DirectAccess::kReadWrite);
  const int removed = ::unlink(path);
  if (removed != 0) return false;
  if (!file) {
    if (file.error() == UringErrorCode::kInvalid || file.error() == UringErrorCode::kNotSupported) {
      std::puts("DirectFile transfer skipped: /tmp filesystem does not support O_DIRECT");
      return true;
    }
    return false;
  }
  const auto supports_poll = file->SupportsPolledIo();
  if (!supports_poll) return false;
  const auto mode = *supports_poll ? ProactorMode::IoPoll() : ProactorMode::EventDriven();
  auto engine = Proactor::Builder().Entries(16).Mode(mode).Build();
  if (!engine) return false;
  auto pool = resources::FixedBufferPool::Create(engine->Backend(), 2, 4096);
  if (!pool) return false;
  auto buffer = pool->Checkout();
  if (!buffer) return false;
  buffer->Bytes()[0] = 42;
  auto write = file->PrepareWriteFixed(*pool, *buffer, 4096, 0);
  if (!write) return false;
  auto token = engine->Arm(1, *write);
  if (!token) {
    if (!pool->CompleteInFlight(*buffer)) std::fputs("buffer tracking rollback failed\n", stderr);
    return false;
  }
  bool completed = false;
  bool success = true;
  auto dispatcher = MakeFnDispatcher([&](const Delivery& delivery) noexcept {
    const auto recycled = pool->CompleteInFlight(*buffer);
    success &= delivery.completion.result == 4096 && recycled.has_value();
    completed = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (success && !completed && std::chrono::steady_clock::now() < deadline) {
    auto progress = engine->Tick(dispatcher);
    success = progress.has_value();
  }
  const auto shutdown = engine->ShutdownWith(dispatcher);
  if (pool->HasPendingDma()) pool->ReleaseOwnershipOnStall();
  std::printf("DirectFile + FixedBufferPool: %s transfer\n",
              *supports_poll ? "IOPOLL" : "event-driven (device has no io_poll)");
  return success && completed && shutdown;
}
}  // namespace
int main() {
  if (!caps::VerifyKernelVersion(6, 16).has_value()) {
    std::fputs("skip: host kernel is below 6.16\n", stderr);
    return 0;
  }
  if (!RunMode(ProactorMode::EventDriven(), "EventDriven") || !RunMode(ProactorMode::SqPoll(), "SqPoll") ||
      !RunMode(ProactorMode::IoPoll(), "IoPoll") || !RunMode(ProactorMode::SqPollIoPoll(), "SqPollIoPoll") || !DirectBuffers())
    return 1;
}
