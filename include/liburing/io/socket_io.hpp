/**
 * @file socket_io.hpp
 * @brief Out-of-the-box completion-driven socket subsystem with generation-tagged socket slots,
 *        buffer-selected receive ring management, and zero-copy send notification tracking.
 */
#pragma once

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/caps/required_caps.hpp>
#include <liburing/core/completion.hpp>
#include <liburing/core/operation_slab.hpp>
#include <liburing/core/proactor.hpp>
#include <liburing/core/subsystem.hpp>
#include <liburing/core/user_data_token.hpp>
#include <liburing/error/uring_error.hpp>
#include <liburing/ops/core.hpp>
#include <liburing/ops/fs.hpp>
#include <liburing/ops/net.hpp>
#include <liburing/resources/provided_buffer_ring.hpp>
#include <liburing/sys/mmap_region.hpp>
#include <liburing/sys/unique_fd.hpp>
#include <liburing/sys/uring_sys.hpp>
#include <new>
#include <optional>
#include <span>
#include <utility>

namespace liburing::core {
using OpToken = liburing::UserDataToken;
using liburing::Completion;
using liburing::Delivery;
using liburing::OperationKey;
using liburing::UserDataToken;
}  // namespace liburing::core

namespace liburing::io {
using OpToken = liburing::UserDataToken;
template <class T, class E = UringErrorCode>
using Result = std::expected<T, E>;

struct SocketId {
  std::uint32_t slot{0};
  std::uint32_t generation{0};
  [[nodiscard]] constexpr std::uint64_t Raw() const noexcept {
    return (static_cast<std::uint64_t>(generation) << 32U) | static_cast<std::uint64_t>(slot);
  }
  friend constexpr bool operator==(SocketId, SocketId) noexcept = default;
};

struct SocketIoConfig {
  std::uint8_t subsystem_id{1};
  std::uint16_t receive_group{1};
  std::uint16_t buffer_count{64};
  std::uint32_t buffer_len{4096};
  std::uint16_t send_buffer_count{64};
  std::uint32_t send_buffer_len{4096};
};

enum class ReceiveMode : std::uint8_t { kOnce = 0, kMultishot = 1 };
enum class SocketEventKind : std::uint8_t {
  kReceived = 0,
  kBufferUnavailable = 1,
  kReadClosed = 2,
  kSent = 3,
  kSendReleased = 4,
  kClosed = 5,
  kFailed = 6,
  kCancelled = 7,
  kCanceled = 7,
};

template <std::size_t MaxRecvBuffers = 64>
struct BasicSocketEvent {
  SocketEventKind kind{SocketEventKind::kFailed};
  SocketId socket{};
  core::OpToken operation{};
  std::optional<resources::BufferLease> data{};
  std::size_t bytes{0};
  bool more{false};
  UringErrorCode error{};
};
using SocketEvent = BasicSocketEvent<64>;

namespace detail {
inline constexpr std::uint32_t kNullOpIndex = 0xffffffffU;
enum class SocketSlotState : std::uint8_t { kFree = 0, kActive = 1, kDrainingInFlight = 2, kClosing = 3 };
enum class TrackedOpKind : std::uint8_t { kNone = 0, kReceive = 1, kSend = 2, kSendZeroCopy = 3, kClose = 4 };

struct SocketSlot {
  int fd{-1};
  std::uint32_t generation{1};
  std::uint32_t next_free{0};
  std::uint32_t head_op{kNullOpIndex};
  std::uint32_t active_ops{0};
  core::OpToken close_token{};
  UringErrorCode close_error{};
  SocketSlotState state{SocketSlotState::kFree};
  bool close_cqe_seen{false};
};

struct TrackedSocketOp {
  core::OpToken token{};
  SocketId socket{};
  std::uint32_t prev_op{kNullOpIndex};
  std::uint32_t next_op{kNullOpIndex};
  std::int32_t primary_res{0};
  std::uint16_t send_slot{0};
  TrackedOpKind kind{TrackedOpKind::kNone};
  bool primary_done{false};
  bool notif_done{false};
  bool active{false};
};

[[nodiscard]] constexpr std::size_t AlignUp(std::size_t value, std::size_t align) noexcept {
  return (value + (align - 1U)) & ~(align - 1U);
}

[[nodiscard]] constexpr UringErrorCode ErrnoToUringError(int err) noexcept {
  if (err <= 0) return UringErrorCode::kUnmappedErrno;
  return SyscallError{static_cast<std::uint16_t>(err)}.code();
}

}  // namespace detail

template <std::size_t MaxSockets = 64, std::size_t MaxRecvBuffers = 64, std::size_t MaxSendBuffers = 64>
  requires(MaxSockets > 0 && MaxRecvBuffers > 0 && std::has_single_bit(MaxRecvBuffers) && MaxSendBuffers > 0 &&
           MaxSendBuffers <= 65535U)
class BasicCompletionSocketIo {
 public:
  static constexpr std::size_t kMaxTrackedOps = 4096U;
  using Event = BasicSocketEvent<MaxRecvBuffers>;

 private:
  struct ArenaHeader {
    sys::MmapRegion self_mapping{};
    sys::UniqueFd dup_ring_fd{};
    std::array<detail::SocketSlot, MaxSockets> sockets{};
    std::array<abi::IoUringBuf, MaxRecvBuffers> in_memory_descriptors{};
    resources::ProvidedBufferRing recv_ring{};
    std::array<detail::TrackedSocketOp, kMaxTrackedOps> ops{};
    std::array<std::uint16_t, MaxSendBuffers> free_send_slots{};
  };

  ArenaHeader* header_{nullptr};
  std::byte* recv_storage_{nullptr};
  std::byte* send_storage_{nullptr};
  SocketIoConfig config_{};
  std::uint32_t free_socket_head_{0};
  std::uint32_t active_sockets_{0};
  std::uint32_t active_ops_{0};
  std::uint16_t free_send_top_{0};
  bool kernel_registered_ring_{false};

  constexpr BasicCompletionSocketIo() noexcept = default;

  [[nodiscard]] static constexpr Event MakeEvent(SocketEventKind kind, SocketId socket, core::OpToken op,
                                                 std::size_t bytes = 0U, bool more = false, UringErrorCode err = {}) noexcept {
    return Event{
        .kind = kind, .socket = socket, .operation = op, .data = std::nullopt, .bytes = bytes, .more = more, .error = err};
  }

  void ReleaseArenaResources() noexcept {
    if (header_ == nullptr) return;
    // If kernel operations are still in flight, retain the mapped arena and buffer ring so pending
    // kernel DMA never reads unmapped or reused memory.
    if (active_ops_ > 0U) {
      header_ = nullptr;
      recv_storage_ = nullptr;
      send_storage_ = nullptr;
      active_sockets_ = 0U;
      active_ops_ = 0U;
      free_send_top_ = 0U;
      kernel_registered_ring_ = false;
      return;
    }

    if (kernel_registered_ring_ && header_->dup_ring_fd.valid()) {
      const abi::IoUringBufReg unreg{.bgid = config_.receive_group};
      static_cast<void>(sys::SysIoUringRegister(header_->dup_ring_fd.get(), abi::kUnregisterPbufRing, &unreg, 1U));
      kernel_registered_ring_ = false;
    }
    for (std::size_t i = 0; i < MaxSockets; ++i) {
      if (header_->sockets[i].state != detail::SocketSlotState::kFree && header_->sockets[i].fd >= 0) {
        ::close(header_->sockets[i].fd);
        header_->sockets[i].fd = -1;
        header_->sockets[i].state = detail::SocketSlotState::kFree;
      }
    }
    sys::MmapRegion self_map = std::move(header_->self_mapping);
    header_->~ArenaHeader();
    header_ = nullptr;
    recv_storage_ = nullptr;
    send_storage_ = nullptr;
  }

  [[nodiscard]] constexpr detail::SocketSlot* LookupActiveSocket(SocketId id) noexcept {
    if (header_ == nullptr || id.slot >= MaxSockets) return nullptr;
    auto& slot = header_->sockets[id.slot];
    return (slot.state == detail::SocketSlotState::kActive && slot.generation == id.generation && slot.fd >= 0) ? &slot
                                                                                                                : nullptr;
  }
  [[nodiscard]] constexpr const detail::SocketSlot* LookupActiveSocket(SocketId id) const noexcept {
    if (header_ == nullptr || id.slot >= MaxSockets) return nullptr;
    const auto& slot = header_->sockets[id.slot];
    return (slot.state == detail::SocketSlotState::kActive && slot.generation == id.generation && slot.fd >= 0) ? &slot
                                                                                                                : nullptr;
  }
  [[nodiscard]] constexpr Result<std::uint16_t> AcquireSendSlot() noexcept {
    if (free_send_top_ == 0U) return std::unexpected(UringErrorCode::kNoBufferSpace);
    --free_send_top_;
    return header_->free_send_slots[free_send_top_];
  }
  constexpr void ReleaseSendSlot(std::uint16_t slot) noexcept {
    if (header_ != nullptr && free_send_top_ < MaxSendBuffers) {
      header_->free_send_slots[free_send_top_] = slot;
      ++free_send_top_;
    }
  }
  [[nodiscard]] constexpr std::span<std::byte> SendBufferMut(std::uint16_t slot, std::size_t len) noexcept {
    const std::size_t offset = static_cast<std::size_t>(slot) * static_cast<std::size_t>(config_.send_buffer_len);
    return {send_storage_ + offset, len};
  }

  constexpr void InsertTrackedOp(core::OpToken token, SocketId socket, detail::TrackedOpKind kind,
                                 std::uint16_t send_slot) noexcept {
    const std::uint32_t idx = Slot(token);
    if (idx >= kMaxTrackedOps || socket.slot >= MaxSockets) return;
    auto& entry = header_->ops[idx];
    if (entry.active) EraseTrackedOp(idx);

    auto& sock_slot = header_->sockets[socket.slot];
    const std::uint32_t old_head = sock_slot.head_op;
    entry = detail::TrackedSocketOp{.token = token,
                                    .socket = socket,
                                    .prev_op = detail::kNullOpIndex,
                                    .next_op = old_head,
                                    .send_slot = send_slot,
                                    .kind = kind,
                                    .active = true};
    if (old_head != detail::kNullOpIndex && old_head < kMaxTrackedOps) {
      header_->ops[old_head].prev_op = idx;
    }
    sock_slot.head_op = idx;
    ++sock_slot.active_ops;
    ++active_ops_;
  }

  constexpr void EraseTrackedOp(std::uint32_t idx) noexcept {
    if (idx >= kMaxTrackedOps) return;
    auto& entry = header_->ops[idx];
    if (!entry.active) return;

    const SocketId socket = entry.socket;
    const std::uint32_t prev = entry.prev_op;
    const std::uint32_t next = entry.next_op;
    if (prev != detail::kNullOpIndex && prev < kMaxTrackedOps) {
      header_->ops[prev].next_op = next;
    } else if (socket.slot < MaxSockets && header_->sockets[socket.slot].head_op == idx) {
      header_->sockets[socket.slot].head_op = next;
    }
    if (next != detail::kNullOpIndex && next < kMaxTrackedOps) {
      header_->ops[next].prev_op = prev;
    }
    if (socket.slot < MaxSockets && header_->sockets[socket.slot].active_ops > 0U) {
      --header_->sockets[socket.slot].active_ops;
    }
    entry = detail::TrackedSocketOp{};
    if (active_ops_ > 0U) --active_ops_;
  }

  constexpr void FinishCloseSlot(SocketId id) noexcept {
    if (header_ == nullptr || id.slot >= MaxSockets) return;
    auto& slot = header_->sockets[id.slot];
    if (slot.generation != id.generation || slot.state == detail::SocketSlotState::kFree) return;
    slot.fd = -1;
    slot.head_op = detail::kNullOpIndex;
    slot.active_ops = 0U;
    slot.close_token = {};
    slot.close_error = {};
    slot.close_cqe_seen = false;
    slot.state = detail::SocketSlotState::kFree;
    slot.generation = (slot.generation + 1U == 0U) ? 1U : (slot.generation + 1U);
    slot.next_free = free_socket_head_;
    free_socket_head_ = id.slot;
    if (active_sockets_ > 0U) --active_sockets_;
  }

  template <class EmitFn>
  void TryCompleteSocketClose(SocketId id, EmitFn& emit) noexcept {
    if (header_ == nullptr || id.slot >= MaxSockets) return;
    auto& slot = header_->sockets[id.slot];
    if (slot.generation != id.generation) return;
    if ((slot.state == detail::SocketSlotState::kClosing || slot.state == detail::SocketSlotState::kDrainingInFlight) &&
        slot.close_cqe_seen && slot.active_ops == 0U) {
      const core::OpToken close_tok = slot.close_token;
      const UringErrorCode close_err = slot.close_error;
      FinishCloseSlot(id);
      if (close_err != UringErrorCode{}) {
        emit(MakeEvent(SocketEventKind::kFailed, id, close_tok, 0U, false, close_err));
      } else {
        emit(MakeEvent(SocketEventKind::kClosed, id, close_tok));
      }
    }
  }

 public:
  BasicCompletionSocketIo(const BasicCompletionSocketIo&) = delete;
  BasicCompletionSocketIo& operator=(const BasicCompletionSocketIo&) = delete;

  constexpr BasicCompletionSocketIo(BasicCompletionSocketIo&& other) noexcept
      : header_(std::exchange(other.header_, nullptr)),
        recv_storage_(std::exchange(other.recv_storage_, nullptr)),
        send_storage_(std::exchange(other.send_storage_, nullptr)),
        config_(other.config_),
        free_socket_head_(other.free_socket_head_),
        active_sockets_(std::exchange(other.active_sockets_, 0U)),
        active_ops_(std::exchange(other.active_ops_, 0U)),
        free_send_top_(std::exchange(other.free_send_top_, 0U)),
        kernel_registered_ring_(std::exchange(other.kernel_registered_ring_, false)) {}

  constexpr BasicCompletionSocketIo& operator=(BasicCompletionSocketIo&& other) noexcept {
    if (this != &other) {
      ReleaseArenaResources();
      header_ = std::exchange(other.header_, nullptr);
      recv_storage_ = std::exchange(other.recv_storage_, nullptr);
      send_storage_ = std::exchange(other.send_storage_, nullptr);
      config_ = other.config_;
      free_socket_head_ = other.free_socket_head_;
      active_sockets_ = std::exchange(other.active_sockets_, 0U);
      active_ops_ = std::exchange(other.active_ops_, 0U);
      free_send_top_ = std::exchange(other.free_send_top_, 0U);
      kernel_registered_ring_ = std::exchange(other.kernel_registered_ring_, false);
    }
    return *this;
  }

  ~BasicCompletionSocketIo() noexcept { ReleaseArenaResources(); }

  template <class ProactorType>
  [[nodiscard]] static Result<BasicCompletionSocketIo> Create(ProactorType& proactor, SocketIoConfig cfg = {}) noexcept {
    if (proactor.Config().mode.UsesIoPoll()) return std::unexpected(UringErrorCode::kInvalid);
    if constexpr (requires { ProactorType::kCapacity; }) {
      static_assert(ProactorType::kCapacity <= kMaxTrackedOps,
                    "Proactor capacity exceeds CompletionSocketIo tracked operation table");
    }
    cfg.buffer_count = static_cast<std::uint16_t>(MaxRecvBuffers);
    cfg.send_buffer_count = static_cast<std::uint16_t>(MaxSendBuffers);
    if (cfg.buffer_len == 0U || cfg.send_buffer_len == 0U) {
      return std::unexpected(UringErrorCode::kInvalid);
    }

    const std::size_t header_bytes = detail::AlignUp(sizeof(ArenaHeader), 64U);
    const std::size_t recv_bytes = detail::AlignUp(MaxRecvBuffers * static_cast<std::size_t>(cfg.buffer_len), 64U);
    const std::size_t send_bytes = detail::AlignUp(MaxSendBuffers * static_cast<std::size_t>(cfg.send_buffer_len), 64U);
    const std::size_t total_bytes = header_bytes + recv_bytes + send_bytes;

    auto region = sys::MmapRegion::Map(nullptr, total_bytes, sys::kProtRead | sys::kProtWrite,
                                       sys::kMapPrivate | sys::kMapAnonymous, -1, 0U);
    if (!region) return std::unexpected(region.error());

    BasicCompletionSocketIo io{};
    auto* base = region->bytes().data();
    io.header_ = ::new (static_cast<void*>(base)) ArenaHeader();
    io.header_->self_mapping = std::move(*region);
    io.recv_storage_ = base + header_bytes;
    io.send_storage_ = base + header_bytes + recv_bytes;
    io.config_ = cfg;
    io.free_socket_head_ = 0U;
    io.active_sockets_ = 0U;
    io.active_ops_ = 0U;

    for (std::size_t i = 0; i < MaxSockets; ++i) {
      io.header_->sockets[i] = detail::SocketSlot{
          .fd = -1,
          .generation = 1U,
          .next_free = (i + 1U < MaxSockets) ? static_cast<std::uint32_t>(i + 1U) : static_cast<std::uint32_t>(MaxSockets),
          .state = detail::SocketSlotState::kFree,
      };
    }

    for (std::size_t i = 0; i < MaxSendBuffers; ++i) {
      io.header_->free_send_slots[i] = static_cast<std::uint16_t>(MaxSendBuffers - 1U - i);
    }
    io.free_send_top_ = static_cast<std::uint16_t>(MaxSendBuffers);

    const std::span<std::byte> recv_span{io.recv_storage_, MaxRecvBuffers * static_cast<std::size_t>(cfg.buffer_len)};
    if constexpr (requires { proactor.Backend().Fd(); }) {
      const int bfd = proactor.Backend().Fd();
      if (bfd >= 0) {
        const int dup_fd = ::fcntl(bfd, F_DUPFD_CLOEXEC, 0);
        if (dup_fd < 0) return std::unexpected(detail::ErrnoToUringError(errno));
        auto adopted_dup = sys::UniqueFd::Adopt(dup_fd);
        if (!adopted_dup) return std::unexpected(adopted_dup.error());

        auto ring =
            resources::ProvidedBufferRing::CreateKernelMapped(proactor.Backend(), static_cast<std::uint32_t>(MaxRecvBuffers),
                                                              cfg.buffer_len, cfg.receive_group, recv_span, false);
        if (!ring) return std::unexpected(ring.error());
        io.header_->dup_ring_fd = std::move(*adopted_dup);
        io.header_->recv_ring = std::move(*ring);
        io.kernel_registered_ring_ = true;
        return io;
      }
    }

    auto ring = resources::ProvidedBufferRing::CreateInMemory(std::span<abi::IoUringBuf>{io.header_->in_memory_descriptors},
                                                              recv_span, cfg.buffer_len, cfg.receive_group, false);
    if (!ring) return std::unexpected(ring.error());
    io.header_->recv_ring = std::move(*ring);
    return io;
  }

  template <class ProactorType>
  [[nodiscard]] Result<void> Unregister(ProactorType& proactor) noexcept {
    if (header_ == nullptr || !kernel_registered_ring_) return {};
    if (active_ops_ > 0U) return std::unexpected(UringErrorCode::kBusy);
    kernel_registered_ring_ = false;
    static_cast<void>(header_->dup_ring_fd.reset());
    if constexpr (requires { header_->recv_ring.Unregister(proactor.Backend()); }) {
      return header_->recv_ring.Unregister(proactor.Backend());
    } else {
      return {};
    }
  }

  template <class ProactorType>
  void CancelAll(ProactorType& proactor) noexcept {
    if (header_ == nullptr) return;
    for (std::size_t i = 0; i < kMaxTrackedOps; ++i) {
      if (header_->ops[i].active) {
        static_cast<void>(proactor.Cancel(header_->ops[i].token));
      }
    }
  }

  [[nodiscard]] constexpr const SocketIoConfig& Config() const noexcept { return config_; }
  [[nodiscard]] constexpr std::size_t SocketCount() const noexcept { return active_sockets_; }
  [[nodiscard]] constexpr std::size_t OperationCount() const noexcept { return active_ops_; }
  [[nodiscard]] constexpr resources::ProvidedBufferRing& RecvRing() noexcept { return header_->recv_ring; }
  [[nodiscard]] constexpr const resources::ProvidedBufferRing& RecvRing() const noexcept { return header_->recv_ring; }

  [[nodiscard]] static constexpr caps::RequiredCaps RequiredCaps() noexcept {
    return caps::RequiredCaps::For<abi::kOpRecv, abi::kOpSend, abi::kOpSendZc, abi::kOpClose>();
  }

  [[nodiscard]] Result<SocketId> AdoptSocket(sys::UniqueFd fd) noexcept {
    if (!fd.valid()) return std::unexpected(UringErrorCode::kBadFd);
    if (header_ == nullptr || free_socket_head_ >= MaxSockets) {
      return std::unexpected(UringErrorCode::kNoSpace);
    }

    const std::uint32_t idx = free_socket_head_;
    auto& slot = header_->sockets[idx];
    free_socket_head_ = slot.next_free;
    slot.fd = fd.release();
    slot.head_op = detail::kNullOpIndex;
    slot.active_ops = 0U;
    slot.close_token = {};
    slot.close_error = {};
    slot.close_cqe_seen = false;
    slot.state = detail::SocketSlotState::kActive;
    if (slot.generation == 0U) slot.generation = 1U;
    ++active_sockets_;
    return SocketId{.slot = idx, .generation = slot.generation};
  }

  [[nodiscard]] Result<SocketId> Insert(sys::UniqueFd fd) noexcept { return AdoptSocket(std::move(fd)); }

  [[nodiscard]] constexpr Result<int> RawFd(SocketId id) const noexcept {
    const auto* slot = LookupActiveSocket(id);
    if (slot == nullptr) return std::unexpected(UringErrorCode::kBadFd);
    return slot->fd;
  }

  template <class ProactorType>
  [[nodiscard]] Result<core::OpToken> Receive(ProactorType& proactor, SocketId id, ReceiveMode mode) noexcept {
    auto* slot = LookupActiveSocket(id);
    if (slot == nullptr) return std::unexpected(UringErrorCode::kBadFd);

    Result<core::OpToken> armed{std::unexpected(UringErrorCode::kInvalid)};
    if (mode == ReceiveMode::kOnce) {
      const auto op = ops::Recv{{.fd = slot->fd,
                                 .address = 0ULL,
                                 .len = config_.buffer_len,
                                 .operation_flags = 0U,
                                 .buffer_index = config_.receive_group}}
                          .template WithSqeFlags<abi::kSqeBufferSelect>();
      armed = proactor.Arm(config_.subsystem_id, op);
    } else {
      const auto op = ops::PrepareRecvMultishot(slot->fd, nullptr, 0U, 0U, config_.receive_group);
      armed = proactor.Arm(config_.subsystem_id, op);
    }

    if (!armed) return std::unexpected(armed.error());
    InsertTrackedOp(*armed, id, detail::TrackedOpKind::kReceive, 0U);
    return *armed;
  }

  template <class ProactorType>
  [[nodiscard]] Result<core::OpToken> Send(ProactorType& proactor, SocketId id, std::span<const std::byte> payload,
                                           int msg_flags = 0) noexcept {
    auto* slot = LookupActiveSocket(id);
    if (slot == nullptr) return std::unexpected(UringErrorCode::kBadFd);
    if (payload.size() > config_.send_buffer_len) return std::unexpected(UringErrorCode::kOverflow);

    auto send_slot = AcquireSendSlot();
    if (!send_slot) return std::unexpected(send_slot.error());

    auto buf = SendBufferMut(*send_slot, payload.size());
    if (!payload.empty()) std::memcpy(buf.data(), payload.data(), payload.size());

    const auto op =
        ops::PrepareSend(slot->fd, buf.data(), static_cast<std::uint32_t>(buf.size()), static_cast<std::uint32_t>(msg_flags));
    auto armed = proactor.Arm(config_.subsystem_id, op);
    if (!armed) {
      ReleaseSendSlot(*send_slot);
      return std::unexpected(armed.error());
    }

    InsertTrackedOp(*armed, id, detail::TrackedOpKind::kSend, *send_slot);
    return *armed;
  }

  template <class ProactorType>
  [[nodiscard]] Result<core::OpToken> SendZeroCopy(ProactorType& proactor, SocketId id, std::span<const std::byte> payload,
                                                   int msg_flags = 0) noexcept {
    auto* slot = LookupActiveSocket(id);
    if (slot == nullptr) return std::unexpected(UringErrorCode::kBadFd);
    if (payload.size() > config_.send_buffer_len) return std::unexpected(UringErrorCode::kOverflow);

    auto send_slot = AcquireSendSlot();
    if (!send_slot) return std::unexpected(send_slot.error());

    auto buf = SendBufferMut(*send_slot, payload.size());
    if (!payload.empty()) std::memcpy(buf.data(), payload.data(), payload.size());

    const auto op = ops::PrepareSendZc(slot->fd, buf.data(), static_cast<std::uint32_t>(buf.size()),
                                       static_cast<std::uint32_t>(msg_flags), 0U);
    auto armed = proactor.Arm(config_.subsystem_id, op);
    if (!armed) {
      ReleaseSendSlot(*send_slot);
      return std::unexpected(armed.error());
    }

    InsertTrackedOp(*armed, id, detail::TrackedOpKind::kSendZeroCopy, *send_slot);
    return *armed;
  }

  template <class ProactorType>
  [[nodiscard]] Result<core::OpToken> CloseSocket(ProactorType& proactor, SocketId id) noexcept {
    auto* slot = LookupActiveSocket(id);
    if (slot == nullptr) return std::unexpected(UringErrorCode::kBadFd);

    slot->state = (slot->active_ops > 0U) ? detail::SocketSlotState::kDrainingInFlight : detail::SocketSlotState::kClosing;
    const auto op = ops::PrepareClose(slot->fd);
    auto armed = proactor.Arm(config_.subsystem_id, op);
    if (!armed) {
      slot->state = detail::SocketSlotState::kActive;
      return std::unexpected(armed.error());
    }

    // Cancel every armed operation on this socket so multishot receives release their kernel file
    // reference and drain before the slot is recycled.
    std::uint32_t cur = slot->head_op;
    while (cur != detail::kNullOpIndex && cur < kMaxTrackedOps) {
      const auto& tracked = header_->ops[cur];
      const std::uint32_t next = tracked.next_op;
      if (tracked.active) static_cast<void>(proactor.Cancel(tracked.token));
      cur = next;
    }

    slot->state = detail::SocketSlotState::kClosing;
    slot->close_cqe_seen = false;
    slot->close_error = {};
    slot->close_token = *armed;
    InsertTrackedOp(*armed, id, detail::TrackedOpKind::kClose, 0U);
    return *armed;
  }

  template <class EmitFn>
    requires std::invocable<EmitFn&, Event>
  void TranslateDelivery(const core::Delivery& d, EmitFn&& emit) noexcept {
    const core::OpToken d_tok = d.completion.token;
    if (header_ == nullptr || Subsystem(d_tok) != config_.subsystem_id) return;

    const std::uint32_t idx = Slot(d_tok);
    if (idx >= kMaxTrackedOps) return;
    auto& tracked = header_->ops[idx];
    if (!tracked.active || tracked.token != d_tok) return;

    const core::OpToken token = tracked.token;
    const SocketId socket = tracked.socket;
    const detail::TrackedOpKind kind = tracked.kind;
    const std::uint16_t send_slot = tracked.send_slot;
    const std::int32_t res = d.completion.result;
    const std::uint32_t flags = d.completion.flags;
    const bool more = (flags & abi::kCqeMore) != 0U;
    const bool notif = (flags & abi::kCqeNotif) != 0U;

    switch (kind) {
      case detail::TrackedOpKind::kReceive: {
        if (!more) EraseTrackedOp(idx);
        if (res < 0) {
          const int err = -res;
          if (err == ENOBUFS) {
            emit(MakeEvent(SocketEventKind::kBufferUnavailable, socket, token, 0U, more, UringErrorCode::kNoBufferSpace));
          } else if (err == ECANCELED) {
            emit(MakeEvent(SocketEventKind::kCancelled, socket, token, 0U, more, UringErrorCode::kCanceled));
          } else {
            emit(MakeEvent(SocketEventKind::kFailed, socket, token, 0U, more, detail::ErrnoToUringError(err)));
          }
          if (!more) TryCompleteSocketClose(socket, emit);
          return;
        }
        if (res == 0) {
          if ((flags & abi::kCqeBuffer) != 0U) {
            auto lease = header_->recv_ring.AcquireLease(0, flags);
            static_cast<void>(lease);
          }
          emit(MakeEvent(SocketEventKind::kReadClosed, socket, token, 0U, more));
          if (!more) TryCompleteSocketClose(socket, emit);
          return;
        }
        const auto bytes = static_cast<std::size_t>(res);
        auto lease = header_->recv_ring.AcquireLease(res, flags);
        if (!lease) {
          emit(MakeEvent(SocketEventKind::kFailed, socket, token, 0U, more, lease.error()));
          if (!more) TryCompleteSocketClose(socket, emit);
          return;
        }
        emit(Event{.kind = SocketEventKind::kReceived,
                   .socket = socket,
                   .operation = token,
                   .data = std::move(*lease),
                   .bytes = bytes,
                   .more = more});
        if (!more) TryCompleteSocketClose(socket, emit);
        return;
      }

      case detail::TrackedOpKind::kSend: {
        EraseTrackedOp(idx);
        ReleaseSendSlot(send_slot);
        if (res < 0) {
          const int err = -res;
          const auto ev_kind = (err == ECANCELED) ? SocketEventKind::kCancelled : SocketEventKind::kFailed;
          emit(MakeEvent(ev_kind, socket, token, 0U, false, detail::ErrnoToUringError(err)));
        } else {
          emit(MakeEvent(SocketEventKind::kSent, socket, token, static_cast<std::size_t>(res)));
        }
        TryCompleteSocketClose(socket, emit);
        return;
      }

      case detail::TrackedOpKind::kSendZeroCopy: {
        if (notif) {
          tracked.notif_done = true;
          if (!tracked.primary_done) return;
          EraseTrackedOp(idx);
          ReleaseSendSlot(send_slot);
          emit(MakeEvent(SocketEventKind::kSendReleased, socket, token));
          TryCompleteSocketClose(socket, emit);
          return;
        }

        tracked.primary_done = true;
        tracked.primary_res = res;
        const bool wait_for_notif = more && !tracked.notif_done;
        if (!wait_for_notif) {
          EraseTrackedOp(idx);
          ReleaseSendSlot(send_slot);
        }

        if (res < 0) {
          const int err = -res;
          const auto ev_kind = (err == ECANCELED) ? SocketEventKind::kCancelled : SocketEventKind::kFailed;
          emit(MakeEvent(ev_kind, socket, token, 0U, wait_for_notif, detail::ErrnoToUringError(err)));
          if (!wait_for_notif && more) emit(MakeEvent(SocketEventKind::kSendReleased, socket, token));
          if (!wait_for_notif) TryCompleteSocketClose(socket, emit);
          return;
        }

        emit(MakeEvent(SocketEventKind::kSent, socket, token, static_cast<std::size_t>(res), wait_for_notif));
        if (!wait_for_notif) {
          emit(MakeEvent(SocketEventKind::kSendReleased, socket, token));
          TryCompleteSocketClose(socket, emit);
        }
        return;
      }

      case detail::TrackedOpKind::kClose: {
        EraseTrackedOp(idx);
        if (socket.slot < MaxSockets && header_->sockets[socket.slot].generation == socket.generation) {
          auto& slot = header_->sockets[socket.slot];
          slot.close_cqe_seen = true;
          slot.close_token = token;
          slot.close_error = (res < 0) ? detail::ErrnoToUringError(-res) : UringErrorCode{};
          TryCompleteSocketClose(socket, emit);
        }
        return;
      }

      case detail::TrackedOpKind::kNone:
      default:
        return;
    }
  }

  template <class EmitFn>
    requires std::invocable<EmitFn&, Event>
  void TranslateBatch(std::span<const core::Delivery> batch, EmitFn&& emit) noexcept {
    for (const auto& d : batch) {
      TranslateDelivery(d, emit);
    }
  }
};

using CompletionSocketIo = BasicCompletionSocketIo<64, 64, 64>;

}  // namespace liburing::io

namespace liburing {
using io::BasicCompletionSocketIo;
using io::BasicSocketEvent;
using io::CompletionSocketIo;
using io::ReceiveMode;
using io::SocketEvent;
using io::SocketEventKind;
using io::SocketId;
using io::SocketIoConfig;
}  // namespace liburing
