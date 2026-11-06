#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <liburing/core/dispatcher.hpp>
#include <liburing/core/proactor.hpp>
#include <liburing/io/socket_io.hpp>
#include <span>
#include <string_view>
#include <vector>

#include "../../support/host_kernel.hpp"

namespace {

using liburing::Delivery;
using liburing::DriveMode;
using liburing::Proactor;
using liburing::UringErrorCode;
using liburing::io::BasicCompletionSocketIo;
using liburing::io::CompletionSocketIo;
using liburing::io::ReceiveMode;
using liburing::io::SocketEvent;
using liburing::io::SocketEventKind;
using liburing::io::SocketId;
using liburing::io::SocketIoConfig;
using liburing::sys::UniqueFd;

struct EventCollector {
  CompletionSocketIo& io;
  std::vector<SocketEvent> events{};

  [[nodiscard]] constexpr bool CanRoute(std::uint8_t id) const noexcept { return id == io.Config().subsystem_id; }

  [[nodiscard]] liburing::Result<void> DispatchBatch(std::span<const Delivery> deliveries) noexcept {
    for (const auto& d : deliveries) {
      io.TranslateDelivery(d, [&](SocketEvent&& ev) noexcept { events.push_back(std::move(ev)); });
    }
    return {};
  }
};

[[nodiscard]] bool CreateUnixSocketPair(UniqueFd& left, UniqueFd& right) noexcept {
  int sv[2]{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
    return false;
  }
  auto l = UniqueFd::Adopt(sv[0]);
  auto r = UniqueFd::Adopt(sv[1]);
  if (!l || !r) return false;
  left = std::move(*l);
  right = std::move(*r);
  return true;
}

[[nodiscard]] bool CreateTcpLoopbackPair(UniqueFd& client_out, UniqueFd& server_out) noexcept {
  const int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (lfd < 0) return false;
  auto listener = UniqueFd::Adopt(lfd);
  if (!listener) return false;

  int one = 1;
  static_cast<void>(::setsockopt(listener->get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(listener->get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) return false;
  if (::listen(listener->get(), 1) != 0) return false;

  socklen_t len = sizeof(addr);
  if (::getsockname(listener->get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;

  const int cfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (cfd < 0) return false;
  auto client = UniqueFd::Adopt(cfd);
  if (!client) return false;
  if (::connect(client->get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) return false;

  const int sfd = ::accept4(listener->get(), nullptr, nullptr, SOCK_CLOEXEC);
  if (sfd < 0) return false;
  auto server = UniqueFd::Adopt(sfd);
  if (!server) return false;

  client_out = std::move(*client);
  server_out = std::move(*server);
  return true;
}

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool socket_id_generation_rejects_stale() noexcept {
  if (const int gate = liburing::test_support::SkipBelowKernel(6, 16); gate >= 0) return gate == 0;
  auto proactor = Proactor::Create(64U);
  if (!proactor) return false;

  using SmallSocketIo = BasicCompletionSocketIo<2, 4, 4>;
  auto io = SmallSocketIo::Create(*proactor, SocketIoConfig{.subsystem_id = 1U, .receive_group = 1U, .buffer_len = 512U});
  if (!io) return false;

  UniqueFd left;
  UniqueFd right;
  if (!CreateUnixSocketPair(left, right)) return false;

  const int expected_left_fd = left.get();
  auto first_id = io->AdoptSocket(std::move(left));
  if (!first_id) return false;
  if (first_id->generation == 0U) return false;
  auto raw = io->RawFd(*first_id);
  if (!raw || *raw != expected_left_fd) return false;

  // Close first_id via io_uring CloseSocket and drive completion.
  auto close_tok = io->CloseSocket(*proactor, *first_id);
  if (!close_tok) return false;
  // While closing, RawFd and Receive on first_id must be rejected.
  if (io->RawFd(*first_id).has_value()) return false;
  if (io->Receive(*proactor, *first_id, ReceiveMode::kOnce).has_value()) return false;

  bool closed_seen = false;
  struct SmallCollector {
    SmallSocketIo& sub;
    bool& seen;
    SocketId expected;
    [[nodiscard]] constexpr bool CanRoute(std::uint8_t id) const noexcept { return id == sub.Config().subsystem_id; }
    [[nodiscard]] liburing::Result<void> DispatchBatch(std::span<const Delivery> deliveries) noexcept {
      for (const auto& d : deliveries) {
        sub.TranslateDelivery(d, [&](const SmallSocketIo::Event& ev) noexcept {
          if (ev.kind == SocketEventKind::kClosed && ev.socket == expected) {
            seen = true;
          }
        });
      }
      return {};
    }
  } collector{*io, closed_seen, *first_id};

  while (!closed_seen) {
    if (!proactor->Drive(DriveMode::kWait, collector)) return false;
  }

  // After close, first_id is stale; adopting two more sockets reuses the slot with an incremented generation.
  UniqueFd s2_l;
  UniqueFd s2_r;
  if (!CreateUnixSocketPair(s2_l, s2_r)) return false;
  auto second_id = io->AdoptSocket(std::move(right));
  auto third_id = io->AdoptSocket(std::move(s2_l));
  if (!second_id || !third_id) return false;

  // One of second_id / third_id reused first_id->slot with generation == first_id->generation + 1.
  const SocketId reused = (second_id->slot == first_id->slot) ? *second_id : *third_id;
  if (reused.slot != first_id->slot || reused.generation <= first_id->generation || reused.Raw() == first_id->Raw()) {
    return false;
  }
  // Stale first_id must still be rejected by RawFd, Receive, Send, and CloseSocket!
  if (io->RawFd(*first_id).has_value()) return false;
  if (io->Receive(*proactor, *first_id, ReceiveMode::kOnce).has_value()) return false;
  const std::array<std::byte, 4> dummy{};
  if (io->Send(*proactor, *first_id, dummy).has_value()) return false;
  if (io->CloseSocket(*proactor, *first_id).has_value()) return false;

  return proactor->DrainBlocking(collector).has_value();
}

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool socket_io_once_and_multishot_recv() noexcept {
  if (const int gate = liburing::test_support::SkipBelowKernel(6, 16); gate >= 0) return gate == 0;
  auto proactor = Proactor::Create(64U);
  if (!proactor) return false;

  auto io = CompletionSocketIo::Create(*proactor, SocketIoConfig{.subsystem_id = 2U, .receive_group = 2U, .buffer_len = 1024U});
  if (!io) return false;

  UniqueFd left_fd;
  UniqueFd right_fd;
  if (!CreateUnixSocketPair(left_fd, right_fd)) return false;

  auto left = io->AdoptSocket(std::move(left_fd));
  auto right = io->AdoptSocket(std::move(right_fd));
  if (!left || !right) return false;

  // 1. ReceiveMode::kOnce on right, Send "hello-once" from left.
  if (!io->Receive(*proactor, *right, ReceiveMode::kOnce)) return false;
  constexpr std::string_view msg_once = "hello-once";
  if (!io->Send(*proactor, *left, std::as_bytes(std::span(msg_once)))) return false;

  EventCollector collector{*io};
  std::string received_once;
  std::size_t sent_bytes = 0;
  for (int step = 0; step < 32 && (received_once != msg_once || sent_bytes != msg_once.size()); ++step) {
    if (!proactor->Drive(DriveMode::kWait, collector)) return false;
    for (auto& ev : collector.events) {
      if (ev.kind == SocketEventKind::kReceived && ev.socket == *right && ev.data.has_value()) {
        const auto bytes = ev.data->Span();
        received_once.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        // Dropping ev.data releases the BufferLease back to the ProvidedBufferRing.
        ev.data.reset();
      } else if (ev.kind == SocketEventKind::kSent && ev.socket == *left) {
        sent_bytes += ev.bytes;
      }
    }
    collector.events.clear();
  }
  if (received_once != msg_once || sent_bytes != msg_once.size()) return false;

  // 2. ReceiveMode::kMultishot on left: send two separate messages from right and receive both via multishot.
  if (!io->Receive(*proactor, *left, ReceiveMode::kMultishot)) return false;
  constexpr std::string_view part_1 = "multi-1:";
  constexpr std::string_view part_2 = "multi-2!";
  if (!io->Send(*proactor, *right, std::as_bytes(std::span(part_1)))) return false;

  std::string received_multi;
  bool sent_part2 = false;
  for (int step = 0; step < 64 && received_multi != "multi-1:multi-2!"; ++step) {
    if (!proactor->Drive(DriveMode::kWait, collector)) return false;
    for (auto& ev : collector.events) {
      if (ev.kind == SocketEventKind::kReceived && ev.socket == *left && ev.data.has_value()) {
        const auto bytes = ev.data->Span();
        received_multi.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ev.data.reset();
        if (!sent_part2 && received_multi == part_1) {
          sent_part2 = true;
          if (!io->Send(*proactor, *right, std::as_bytes(std::span(part_2)))) return false;
        }
      }
    }
    collector.events.clear();
  }
  if (received_multi != "multi-1:multi-2!") return false;

  return proactor->DrainBlocking(collector).has_value();
}

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool socket_io_send_and_send_zc_lifecycle() noexcept {
  if (const int gate = liburing::test_support::SkipBelowKernel(6, 16); gate >= 0) return gate == 0;
  auto proactor = Proactor::Create(64U);
  if (!proactor) return false;

  auto io = CompletionSocketIo::Create(*proactor, SocketIoConfig{.subsystem_id = 3U, .receive_group = 3U, .buffer_len = 4096U});
  if (!io) return false;

  UniqueFd client_fd;
  UniqueFd server_fd;
  if (!CreateTcpLoopbackPair(client_fd, server_fd)) return false;

  auto client = io->AdoptSocket(std::move(client_fd));
  auto server = io->AdoptSocket(std::move(server_fd));
  if (!client || !server) return false;

  if (!io->Receive(*proactor, *server, ReceiveMode::kOnce)) return false;
  constexpr std::string_view payload = "zero-copy-tcp-payload";
  auto zc_op = io->SendZeroCopy(*proactor, *client, std::as_bytes(std::span(payload)), MSG_NOSIGNAL);
  if (!zc_op) return false;

  EventCollector collector{*io};
  bool saw_sent = false;
  bool saw_send_released = false;
  std::string server_received;

  for (int step = 0; step < 64 && (!saw_sent || !saw_send_released || server_received != payload); ++step) {
    if (!proactor->Drive(DriveMode::kWait, collector)) return false;
    for (auto& ev : collector.events) {
      if (ev.kind == SocketEventKind::kSent && ev.socket == *client && ev.operation == *zc_op) {
        saw_sent = (ev.bytes == payload.size());
      } else if (ev.kind == SocketEventKind::kSendReleased && ev.socket == *client && ev.operation == *zc_op) {
        saw_send_released = true;
      } else if (ev.kind == SocketEventKind::kReceived && ev.socket == *server && ev.data.has_value()) {
        const auto span = ev.data->Span();
        server_received.assign(reinterpret_cast<const char*>(span.data()), span.size());
        ev.data.reset();
      }
    }
    collector.events.clear();
  }
  if (!saw_sent || !saw_send_released || server_received != payload) return false;

  return proactor->DrainBlocking(collector).has_value();
}

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool socket_io_enobufs_and_eof_events() noexcept {
  if (const int gate = liburing::test_support::SkipBelowKernel(6, 16); gate >= 0) return gate == 0;
  auto proactor = Proactor::Create(64U);
  if (!proactor) return false;

  // Use a 2-buffer receive ring so holding 2 leases exhausts the provided buffer ring (-ENOBUFS).
  using TinyRecvIo = BasicCompletionSocketIo<4, 2, 4>;
  auto io = TinyRecvIo::Create(*proactor, SocketIoConfig{.subsystem_id = 4U, .receive_group = 4U, .buffer_len = 256U});
  if (!io) return false;

  UniqueFd left_fd;
  UniqueFd right_fd;
  if (!CreateUnixSocketPair(left_fd, right_fd)) return false;

  auto left = io->AdoptSocket(std::move(left_fd));
  auto right = io->AdoptSocket(std::move(right_fd));
  if (!left || !right) return false;

  std::vector<TinyRecvIo::Event> held_events;
  struct TinyCollector {
    TinyRecvIo& sub;
    std::vector<TinyRecvIo::Event>& out;
    [[nodiscard]] constexpr bool CanRoute(std::uint8_t id) const noexcept { return id == sub.Config().subsystem_id; }
    [[nodiscard]] liburing::Result<void> DispatchBatch(std::span<const Delivery> deliveries) noexcept {
      for (const auto& d : deliveries) {
        sub.TranslateDelivery(d, [&](TinyRecvIo::Event&& ev) noexcept { out.push_back(std::move(ev)); });
      }
      return {};
    }
  } collector{*io, held_events};

  constexpr std::string_view chunk = "chunk";
  // Consume both receive buffers WITHOUT dropping their BufferLeases.
  for (int i = 0; i < 2; ++i) {
    if (!io->Receive(*proactor, *right, ReceiveMode::kOnce)) return false;
    if (!io->Send(*proactor, *left, std::as_bytes(std::span(chunk)))) return false;
    std::size_t got_recv = 0;
    while (got_recv == 0) {
      if (!proactor->Drive(DriveMode::kWait, collector)) return false;
      for (const auto& ev : held_events) {
        if (ev.kind == SocketEventKind::kReceived) ++got_recv;
      }
    }
    // Keep only the kReceived events (so their BufferLeases stay alive!).
    std::erase_if(held_events, [](const TinyRecvIo::Event& e) { return e.kind != SocketEventKind::kReceived; });
  }
  if (held_events.size() != 2U) return false;

  // 3rd receive while both BufferLeases are still held must produce kBufferUnavailable (-ENOBUFS)!
  if (!io->Receive(*proactor, *right, ReceiveMode::kOnce)) return false;
  if (!io->Send(*proactor, *left, std::as_bytes(std::span(chunk)))) return false;
  bool saw_enobufs = false;
  for (int step = 0; step < 32 && !saw_enobufs; ++step) {
    if (!proactor->Drive(DriveMode::kWait, collector)) return false;
    for (const auto& ev : held_events) {
      if (ev.kind == SocketEventKind::kBufferUnavailable && ev.socket == *right) {
        saw_enobufs = true;
      }
    }
  }
  if (!saw_enobufs) return false;

  // Release the 2 held BufferLeases -> buffers automatically return to the ProvidedBufferRing!
  held_events.clear();

  // Now Receive succeeds for the pending 3rd chunk!
  if (!io->Receive(*proactor, *right, ReceiveMode::kOnce)) return false;
  bool got_third = false;
  for (int step = 0; step < 32 && !got_third; ++step) {
    if (!proactor->Drive(DriveMode::kWait, collector)) return false;
    for (auto& ev : held_events) {
      if (ev.kind == SocketEventKind::kReceived && ev.socket == *right) {
        got_third = true;
      }
    }
    held_events.clear();
  }
  if (!got_third) return false;

  // Close left socket and verify right socket receives kReadClosed (EOF)!
  if (!io->CloseSocket(*proactor, *left)) return false;
  if (!io->Receive(*proactor, *right, ReceiveMode::kOnce)) return false;
  bool saw_left_closed = false;
  bool saw_right_eof = false;
  for (int step = 0; step < 32 && (!saw_left_closed || !saw_right_eof); ++step) {
    if (!proactor->Drive(DriveMode::kWait, collector)) return false;
    for (const auto& ev : held_events) {
      if (ev.kind == SocketEventKind::kClosed && ev.socket == *left) saw_left_closed = true;
      if (ev.kind == SocketEventKind::kReadClosed && ev.socket == *right) saw_right_eof = true;
    }
    held_events.clear();
  }
  if (!saw_left_closed || !saw_right_eof) return false;

  return proactor->DrainBlocking(collector).has_value();
}

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool socket_io_close_cancels_inflight_recv() noexcept {
  if (const int gate = liburing::test_support::SkipBelowKernel(6, 16); gate >= 0) return gate == 0;
  auto proactor = Proactor::Create(64U);
  if (!proactor) return false;

  auto io = CompletionSocketIo::Create(*proactor, SocketIoConfig{.subsystem_id = 5U, .receive_group = 5U, .buffer_len = 512U});
  if (!io) return false;

  UniqueFd left_fd;
  UniqueFd right_fd;
  if (!CreateUnixSocketPair(left_fd, right_fd)) return false;

  auto id = io->AdoptSocket(std::move(left_fd));
  if (!id) return false;

  EventCollector collector{*io};
  if (!io->Receive(*proactor, *id, ReceiveMode::kMultishot)) return false;
  if (!proactor->Drive(DriveMode::kPoll, collector)) return false;

  if (!io->CloseSocket(*proactor, *id)) return false;
  bool saw_cancelled = false;
  bool saw_closed = false;
  for (int i = 0; i < 32 && (!saw_closed || proactor->InFlight() != 0U); ++i) {
    if (!proactor->Drive(DriveMode::kPoll, collector)) return false;
    for (const auto& ev : collector.events) {
      if (ev.kind == SocketEventKind::kCancelled && ev.socket == *id) saw_cancelled = true;
      if (ev.kind == SocketEventKind::kClosed && ev.socket == *id) saw_closed = true;
    }
    collector.events.clear();
    if (!saw_closed || proactor->InFlight() != 0U) ::usleep(500);
  }
  if (!saw_cancelled || !saw_closed || io->SocketCount() != 0U || io->OperationCount() != 0U || proactor->InFlight() != 0U) {
    return false;
  }

  static_cast<void>(::fcntl(right_fd.get(), F_SETFL, O_NONBLOCK));
  char c = 0;
  const auto r = ::read(right_fd.get(), &c, 1U);
  if (r != 0) return false;

  return proactor->DrainBlocking(collector).has_value();
}

// NOLINTNEXTLINE(readability-identifier-naming)
[[nodiscard]] bool socket_io_inflight_destroy_retains_arena() noexcept {
  if (const int gate = liburing::test_support::SkipBelowKernel(6, 16); gate >= 0) return gate == 0;
  auto proactor = Proactor::Create(64U);
  if (!proactor) return false;

  UniqueFd left_fd;
  UniqueFd right_fd;
  if (!CreateUnixSocketPair(left_fd, right_fd)) return false;

  int small = 4096;
  static_cast<void>(::setsockopt(left_fd.get(), SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)));
  static_cast<void>(::fcntl(left_fd.get(), F_SETFL, O_NONBLOCK));
  std::array<char, 4096> fill{};
  while (::write(left_fd.get(), fill.data(), fill.size()) > 0) {
  }
  static_cast<void>(::fcntl(left_fd.get(), F_SETFL, 0));

  struct RawSink {
    std::int32_t last_res{0};
    int seen{0};
    [[nodiscard]] static constexpr bool CanRoute(std::uint8_t id) noexcept { return id == 6U; }
    [[nodiscard]] liburing::Result<void> DispatchBatch(std::span<const Delivery> deliveries) noexcept {
      for (const auto& d : deliveries) {
        ++seen;
        last_res = d.completion.result;
      }
      return {};
    }
  } sink{};

  {
    auto io =
        CompletionSocketIo::Create(*proactor, SocketIoConfig{.subsystem_id = 6U, .receive_group = 6U, .buffer_len = 512U});
    if (!io) return false;

    const int dup_fd = ::fcntl(left_fd.get(), F_DUPFD_CLOEXEC, 0);
    auto adopted_dup = UniqueFd::Adopt(dup_fd);
    if (!adopted_dup) return false;
    auto id = io->AdoptSocket(std::move(*adopted_dup));
    if (!id) return false;

    constexpr std::string_view payload = "PAYLOAD";
    if (!io->Send(*proactor, *id, std::as_bytes(std::span(payload)))) return false;
    if (!proactor->Drive(DriveMode::kPoll, sink)) return false;
    if (io->OperationCount() != 1U || proactor->InFlight() != 1U) return false;
    // Destroy io while its Send is still parked in the kernel.
  }

  std::array<char, 16384> drain_buf{};
  static_cast<void>(::fcntl(right_fd.get(), F_SETFL, O_NONBLOCK));
  while (::read(right_fd.get(), drain_buf.data(), drain_buf.size()) > 0) {
  }

  for (int i = 0; i < 50 && proactor->InFlight() != 0U; ++i) {
    if (!proactor->Drive(DriveMode::kPoll, sink)) return false;
    if (proactor->InFlight() != 0U) ::usleep(500);
  }
  if (proactor->InFlight() != 0U || sink.seen != 1 || sink.last_res != 7) return false;

  return proactor->DrainBlocking(sink).has_value();
}

[[nodiscard]] bool SocketIoRejectsIoPoll() noexcept {
  for (auto mode : {liburing::ProactorMode::IoPoll(), liburing::ProactorMode::SqPollIoPoll()}) {
    auto proactor = Proactor::Builder().Entries(8).Mode(mode).BuildScripted();
    if (!proactor) return false;
    auto io = CompletionSocketIo::Create(*proactor);
    if (io || io.error() != UringErrorCode::kInvalid || proactor->InFlight() != 0U) return false;
    if (!proactor->Shutdown()) return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string_view name(argv[1]);
    if (name == "socket_io_rejects_iopoll") return SocketIoRejectsIoPoll() ? 0 : 1;
    if (name == "socket_id_generation_rejects_stale") return socket_id_generation_rejects_stale() ? 0 : 1;
    if (name == "socket_io_once_and_multishot_recv") return socket_io_once_and_multishot_recv() ? 0 : 1;
    if (name == "socket_io_send_and_send_zc_lifecycle") return socket_io_send_and_send_zc_lifecycle() ? 0 : 1;
    if (name == "socket_io_enobufs_and_eof_events") return socket_io_enobufs_and_eof_events() ? 0 : 1;
    if (name == "socket_io_close_cancels_inflight_recv") return socket_io_close_cancels_inflight_recv() ? 0 : 1;
    if (name == "socket_io_inflight_destroy_retains_arena") return socket_io_inflight_destroy_retains_arena() ? 0 : 1;
    return 2;
  }
  return SocketIoRejectsIoPoll() && socket_id_generation_rejects_stale() && socket_io_once_and_multishot_recv() &&
                 socket_io_send_and_send_zc_lifecycle() && socket_io_enobufs_and_eof_events() &&
                 socket_io_close_cancels_inflight_recv() && socket_io_inflight_destroy_retains_arena()
             ? 0
             : 1;
}
