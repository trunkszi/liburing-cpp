#include <sys/socket.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <liburing/liburing.hpp>
#include <span>

using namespace liburing;
int main() {
  if (!caps::VerifyKernelVersion(6, 16).has_value()) {
    std::fputs("skip: host kernel is below 6.16\n", stderr);
    return 0;
  }
  int descriptors[2]{};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) != 0) return 1;
  auto left_fd = sys::UniqueFd::Adopt(descriptors[0]);
  auto right_fd = sys::UniqueFd::Adopt(descriptors[1]);
  auto engine = Proactor::Builder().Entries(32).Build();
  if (!engine || !left_fd || !right_fd) return 2;
  auto sockets = CompletionSocketIo::Create(*engine, SocketIoConfig{.subsystem_id = 3, .receive_group = 3});
  if (!sockets) return 3;
  auto left = sockets->AdoptSocket(std::move(*left_fd));
  auto right = sockets->AdoptSocket(std::move(*right_fd));
  if (!left || !right) return 4;
  std::array<std::byte, 4> received{};
  std::size_t received_count = 0;
  unsigned sent = 0;
  unsigned closed = 0;
  bool valid = true;
  auto events = [&](const SocketEvent& event) noexcept {
    if (event.kind == SocketEventKind::kReceived) {
      const auto bytes = event.data->Bytes();
      if (bytes.size() > received.size() - received_count) {
        valid = false;
        return;
      }
      std::memcpy(received.data() + received_count, bytes.data(), bytes.size());
      received_count += bytes.size();
    }
    if (event.kind == SocketEventKind::kSent) ++sent;
    if (event.kind == SocketEventKind::kClosed) ++closed;
    if (event.kind == SocketEventKind::kFailed || event.kind == SocketEventKind::kBufferUnavailable) {
      std::printf("socket error: %s\n", ErrorName(event.error).data());
      valid = false;
    }
  };
  struct Dispatcher {
    CompletionSocketIo& sockets;
    decltype(events)& emit;
    [[nodiscard]] Result<void> DispatchBatch(std::span<const Delivery> batch) noexcept {
      sockets.TranslateBatch(batch, emit);
      return {};
    }
  } dispatcher{*sockets, events};
  constexpr std::array<std::byte, 4> message{std::byte{'p'}, std::byte{'i'}, std::byte{'n'}, std::byte{'g'}};
  bool success = sockets->Receive(*engine, *right, io::ReceiveMode::kMultishot).has_value() &&
                 sockets->Send(*engine, *left, message, MSG_NOSIGNAL).has_value();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (success && valid && (received_count != message.size() || sent != 1) && std::chrono::steady_clock::now() < deadline) {
    auto progress = engine->WaitCompletions(1, std::chrono::milliseconds(50), dispatcher);
    success = progress || progress.error() == UringErrorCode::kTimedOut;
  }
  success &= valid && received == message && sent == 1;
  const auto close_left = sockets->CloseSocket(*engine, *left);
  const auto close_right = sockets->CloseSocket(*engine, *right);
  while (close_left && close_right && closed != 2 && std::chrono::steady_clock::now() < deadline) {
    auto progress = engine->WaitCompletions(1, std::chrono::milliseconds(50), dispatcher);
    if (!progress && progress.error() != UringErrorCode::kTimedOut) break;
  }
  const auto shutdown = engine->ShutdownWith(dispatcher);
  if (!success || !close_left || !close_right || closed != 2 || !shutdown) {
    std::printf("received=%zu sent=%u closed=%u valid=%d success=%d shutdown=%d\n", received_count, sent, closed, valid,
                success, shutdown.has_value());
    return 5;
  }
  std::puts("CompletionSocketIo: multishot receive, retained send, TranslateBatch and drained socket close");
}
