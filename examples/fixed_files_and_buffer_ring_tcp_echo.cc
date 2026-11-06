#include <arpa/inet.h>
#include <sys/socket.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <liburing/liburing.hpp>
#include <optional>

using namespace liburing;
int main() {
  if (!caps::VerifyKernelVersion(6, 16).has_value()) {
    std::fputs("skip: host kernel is below 6.16\n", stderr);
    return 0;
  }
  auto listener = sys::UniqueFd::Adopt(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  auto client = sys::UniqueFd::Adopt(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!listener || !client) return 1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listener->get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(listener->get(), 1) != 0)
    return 2;
  socklen_t length = sizeof(address);
  if (::getsockname(listener->get(), reinterpret_cast<sockaddr*>(&address), &length) != 0 ||
      ::connect(client->get(), reinterpret_cast<const sockaddr*>(&address), length) != 0)
    return 3;

  // Backing storage outlives both the selected-buffer ring and the Proactor.
  std::array<std::byte, 8 * 512> backing{};
  auto engine = Proactor::Builder().Entries(16).Build();
  if (!engine) return 4;
  resources::FixedFileTable<> files;
  if (!files.RegisterSparseFiles(engine->Backend(), 64)) return 5;
  auto slot = files.AllocateSlot();
  auto buffers = resources::ProvidedBufferRing::CreateKernelMapped(engine->Backend(), 8, 512, 1, backing);
  if (!slot || !buffers) return 6;
  Completion last{};
  auto dispatcher = MakeFnDispatcher([&](const Delivery& delivery) noexcept { last = delivery.completion; });
  auto wait = [&](UserDataToken token) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (last.token != token && std::chrono::steady_clock::now() < deadline) {
      auto result = engine->WaitCompletions(1, std::chrono::milliseconds(50), dispatcher);
      if (!result && result.error() != UringErrorCode::kTimedOut) return false;
    }
    return last.token == token && last.result >= 0;
  };
  bool success = true;
  auto accepted = engine->Arm(1, ops::PrepareAcceptDirect(listener->get(), nullptr, nullptr, *slot, 0));
  success = accepted && wait(*accepted);
  constexpr std::array<char, 5> message{'e', 'c', 'h', 'o', '!'};
  if (success) success = ::send(client->get(), message.data(), message.size(), MSG_NOSIGNAL) == 5;
  std::optional<resources::BufferLease> held;
  if (success) {
    const auto receive = ops::Recv{{.fd = static_cast<std::int32_t>(*slot), .len = 512, .buffer_index = 1}}
                             .WithSqeFlags<abi::kSqeFixedFile | abi::kSqeBufferSelect>();
    auto token = engine->Arm(1, receive);
    success = token && wait(*token) && last.result == 5;
    if (success) {
      auto lease = buffers->AcquireLease(last.result, last.flags);
      success = lease.has_value();
      if (lease) held.emplace(std::move(*lease));
    }
  }
  if (success) {
    const auto send = ops::PrepareSend(static_cast<std::int32_t>(*slot), held->Bytes().data(), 5, MSG_NOSIGNAL)
                          .WithSqeFlags<abi::kSqeFixedFile>();
    auto token = engine->Arm(1, send);
    success = token && wait(*token) && last.result == 5;
    // A send borrows the lease until its terminal CQE.
    held.reset();
  }
  std::array<char, 5> reply{};
  if (success) {
    auto token = engine->Arm(1, ops::PrepareRecv(client->get(), reply.data(), 5, MSG_WAITALL));
    success = token && wait(*token) && last.result == 5 && reply == message;
  }
  held.reset();
  const auto unregistered_buffers = buffers->Unregister(engine->Backend());
  const auto unregistered_files = files.UnregisterFiles(engine->Backend());
  const auto drained = engine->ShutdownWith(dispatcher);
  if (!success || !drained || !unregistered_buffers || !unregistered_files) {
    std::printf("success=%d drain=%d buffers=%d files=%d last=%d\n", success, drained.has_value(),
                unregistered_buffers.has_value(), unregistered_files.has_value(), last.result);
    return 7;
  }
  std::puts("TCP echo: direct accepted descriptor + selected BufferLease, recycled after send completion");
}
