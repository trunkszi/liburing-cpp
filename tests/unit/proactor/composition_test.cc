#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <liburing/liburing.hpp>

#include "../../support/host_kernel.hpp"
using namespace liburing;
struct DnsSubsystem {
  static constexpr std::uint8_t kSubsystemId = 1;
  Proactor* engine{};
  int server{-1};
  int client{-1};
  char query{'Q'};
  char request{};
  char response{'R'};
  char answer{};
  std::size_t count{};
  bool okay{true};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept {
    return caps::RequiredCaps::For<abi::kOpSend, abi::kOpRecv>();
  }
  void OnCompletions(std::span<const Delivery> deliveries) noexcept {
    for (const auto& delivery : deliveries) {
      ++count;
      okay &= delivery.completion.result == 1;
      if (count == 2) {
        okay &= engine->ArmOneShot(1, ops::PrepareRecv(client, &answer, 1)).has_value();
        okay &= engine->ArmOneShot(1, ops::PrepareSend(server, &response, 1)).has_value();
      }
    }
  }
};
struct RpcSubsystem {
  static constexpr std::uint8_t kSubsystemId = 2;
  std::size_t count{};
  bool okay{true};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept {
    return caps::RequiredCaps::For<abi::kOpSend, abi::kOpRecv>();
  }
  void OnCompletions(std::span<const Delivery> deliveries) noexcept {
    for (const auto& delivery : deliveries) {
      ++count;
      okay &= delivery.completion.result == 3;
    }
  }
};
struct FileSubsystem {
  static constexpr std::uint8_t kSubsystemId = 3;
  Proactor* engine{};
  int fd{-1};
  std::array<char, 4> write{'d', 'a', 't', 'a'};
  std::array<char, 4> read{};
  std::size_t count{};
  bool okay{true};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept {
    return caps::RequiredCaps::For<abi::kOpWrite, abi::kOpRead, abi::kOpFsync>();
  }
  void OnCompletions(std::span<const Delivery> deliveries) noexcept {
    for (const auto& delivery : deliveries) {
      ++count;
      okay &= delivery.completion.result == (count == 3 ? 0 : 4);
      if (count == 1)
        okay &= engine->ArmOneShot(3, ops::PrepareRead(fd, read.data(), static_cast<std::uint32_t>(read.size()))).has_value();
      if (count == 2) okay &= engine->ArmOneShot(3, ops::PrepareFsync(fd)).has_value();
    }
  }
};
struct TimerSubsystem {
  static constexpr std::uint8_t kSubsystemId = 4;
  std::size_t count{};
  bool okay{true};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept {
    return caps::RequiredCaps::For<abi::kOpTimeout>();
  }
  void OnCompletion(const Delivery& delivery) noexcept {
    ++count;
    okay &= delivery.completion.result == -static_cast<int>(UringErrorCode::kTimerExpired);
  }
};
struct FutexSubsystem {
  static constexpr std::uint8_t kSubsystemId = 5;
  Proactor* engine{};
  std::uint32_t word{};
  std::size_t count{};
  bool okay{true};
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept {
    return caps::RequiredCaps::For<abi::kOpFutexWait, abi::kOpFutexWake>();
  }
  void OnCompletions(std::span<const Delivery> deliveries) noexcept {
    for (const auto& delivery : deliveries) {
      ++count;
      okay &= delivery.completion.result >= 0;
    }
  }
};
[[nodiscard]] bool ConnectUdpPair(int server, int client) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sockaddr_in b = a;
  if (bind(server, reinterpret_cast<sockaddr*>(&a), sizeof(a)) || bind(client, reinterpret_cast<sockaddr*>(&b), sizeof(b)))
    return false;
  socklen_t length = sizeof(a);
  if (getsockname(server, reinterpret_cast<sockaddr*>(&a), &length) ||
      getsockname(client, reinterpret_cast<sockaddr*>(&b), &length))
    return false;
  return connect(server, reinterpret_cast<sockaddr*>(&b), sizeof(b)) == 0 &&
         connect(client, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
}
int main() {
  if (const int gate = test_support::SkipBelowKernel(6, 16); gate >= 0) return gate;
  constexpr auto required =
      StaticDispatcher<DnsSubsystem, RpcSubsystem, FileSubsystem, TimerSubsystem, FutexSubsystem>::RequiredCaps();
  auto created = Proactor::Create(32, RingMode::kStandard64_16, required);
  if (!created) return 1;
  auto& engine = *created;
  auto server = sys::UniqueFd::Adopt(socket(AF_INET, SOCK_DGRAM, 0));
  auto client = sys::UniqueFd::Adopt(socket(AF_INET, SOCK_DGRAM, 0));
  int streams[2]{};
  if (!server || !client || !ConnectUdpPair(server->get(), client->get()) || socketpair(AF_UNIX, SOCK_STREAM, 0, streams))
    return 2;
  auto stream_a = sys::UniqueFd::Adopt(streams[0]);
  auto stream_b = sys::UniqueFd::Adopt(streams[1]);
  auto file = sys::UniqueFd::Adopt(static_cast<std::int32_t>(sys::Syscall6(319, sys::Address("composition"), 0)));
  if (!stream_a || !stream_b || !file) return 3;
  DnsSubsystem dns{&engine, server->get(), client->get()};
  RpcSubsystem rpc;
  FileSubsystem storage;
  storage.engine = &engine;
  storage.fd = file->get();
  TimerSubsystem timer;
  FutexSubsystem futex;
  futex.engine = &engine;
  StaticDispatcher dispatcher{dns, rpc, storage, timer, futex};
  std::array<char, 3> sent{'r', 'p', 'c'};
  std::array<char, 3> received{};
  abi::KernelTimespec timeout{0, 1000000};
  bool armed = engine.ArmOneShot(1, ops::PrepareRecv(dns.server, &dns.request, 1)).has_value();
  armed &= engine.ArmOneShot(1, ops::PrepareSend(dns.client, &dns.query, 1)).has_value();
  armed &= engine.ArmOneShot(2, ops::PrepareRecv(stream_b->get(), received.data(), received.size())).has_value();
  armed &= engine.ArmOneShot(2, ops::PrepareSend(stream_a->get(), sent.data(), sent.size())).has_value();
  armed &= engine.ArmOneShot(3, ops::PrepareWrite(file->get(), storage.write.data(), storage.write.size())).has_value();
  armed &= engine.ArmOneShot(4, ops::PrepareTimeout(&timeout, 0)).has_value();
  armed &= engine.ArmOneShot(5, ops::PrepareFutexWait(&futex.word, 0)).has_value();
  armed &= engine.ArmOneShot(5, ops::PrepareFutexWake(&futex.word, 1)).has_value();
  if (!armed) return 4;
  for (std::size_t step = 0; engine.InFlight() != 0 && step < 100000; ++step)
    if (!engine.Drive(DriveMode::kPoll, dispatcher)) return 5;
  if (engine.InFlight() != 0) return 6;
  if (dns.count != 4 || rpc.count != 2 || storage.count != 3 || timer.count != 1 || futex.count != 2) return 7;
  if (!dns.okay || !rpc.okay || !storage.okay || !timer.okay || !futex.okay) {
    std::fprintf(stderr, "subsystem results: DNS=%d RPC=%d File=%d Timer=%d Futex=%d\n", dns.okay, rpc.okay, storage.okay,
                 timer.okay, futex.okay);
    return 8;
  }
  if (dns.request != 'Q' || dns.answer != 'R' || received != sent || storage.read != storage.write) return 9;
  storage.count = 0;
  storage.okay = true;
  SingleDispatcher single{storage};
  if (!engine.ArmOneShot(3, ops::PrepareWrite(file->get(), storage.write.data(), storage.write.size()))) return 10;
  while (engine.InFlight() != 0)
    if (!engine.Drive(DriveMode::kWait, single)) return 11;
  if (storage.count != 3 || !storage.okay) return 12;
  DynamicDispatcher dynamic;
  if (!dynamic.Register(Describe(rpc)) || !dynamic.Register(Describe(timer))) return 13;
  if (!engine.ArmOneShot(4, ops::PrepareTimeout(&timeout, 0))) return 14;
  while (engine.InFlight() != 0)
    if (!engine.Drive(DriveMode::kWait, dynamic)) return 15;
  if (timer.count != 2) return 16;
  if (!engine.BeginDrain() || !engine.Drive(DriveMode::kPoll, dispatcher) || !engine.Close()) return 17;
  return 0;
}
