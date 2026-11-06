/** @file @brief Typed SQE images keep protocol metadata and engine-owned flags consistent. */
#pragma once
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/caps/required_caps.hpp>
#include <liburing/core/op_header.hpp>
#include <meta>
#include <type_traits>
namespace liburing::ops {
class[[= uarch::layout::register_leaf]] UserAddress {
  std::uint64_t value_{};

 public:
  constexpr UserAddress() noexcept = default;
  template <std::integral Integer>
  constexpr UserAddress(Integer value) noexcept : value_(static_cast<std::uint64_t>(value)) {}
  constexpr UserAddress(std::nullptr_t) noexcept {}
  template <class T>
  UserAddress(T* pointer) noexcept : value_(reinterpret_cast<std::uint64_t>(pointer)) {}
  [[nodiscard]] constexpr std::uint64_t Raw() const noexcept { return value_; }
};
static_assert(sizeof(UserAddress) == 8 && uarch::NoPadding<UserAddress> && uarch::RegisterLeaf<UserAddress>);
namespace detail {
[[nodiscard]] constexpr std::uint32_t EncodeDirectFileIndex(std::uint32_t file_index) noexcept {
  // Zero selects a process fd in the ABI; the allocation sentinel bypasses the slot bias.
  return file_index == abi::kFileIndexAlloc ? abi::kFileIndexAlloc : file_index + 1U;
}

[[nodiscard]] constexpr std::uint64_t MergeMemberToken(std::uint64_t supplied, std::uint64_t destination) noexcept {
  // Reservation owns member identity, including implicit tokens.
  const auto selected = supplied == 0 ? destination : supplied;
  const auto tagged =
      UserDataToken::from_raw(selected).with<^^TokenSpec::member>(Member(UserDataToken::from_raw(destination))).raw();
  return destination != 0 ? tagged : selected;
}

template <std::uint32_t kOpcodeValue, OpKind kKindValue = OpKind::kOneShot, std::size_t kSlotsValue = 1,
          bool kCqe32Value = false, std::uint32_t kFutexValue = 0, bool kUntrackedValue = false>
class Operation {
 public:
  static_assert(kOpcodeValue < abi::kOpLast && (kSlotsValue == 1 || kSlotsValue == 2));
  static constexpr std::uint32_t kOpcode = kOpcodeValue;
  static constexpr std::size_t kSqeSlots = kSlotsValue;
  static constexpr OpKind kKind = kKindValue;
  static constexpr bool kRequiresCqe32 = kCqe32Value;
  static constexpr bool kUntracked = kUntrackedValue;
  using Sqe = std::conditional_t<kSlotsValue == 1, abi::FlatSqe, abi::FlatSqe128>;

 private:
  Sqe fields_{};
  constexpr abi::FlatSqe& Base() noexcept {
    if constexpr (kSlotsValue == 1)
      return fields_;
    else
      return fields_.sqe;
  }

 public:
  constexpr Operation() noexcept {
    Base().opcode = static_cast<std::uint8_t>(kOpcode);
    Base().fd = -1;
  }
  explicit constexpr Operation(const Sqe& fields) noexcept : fields_(fields) {
    Base().opcode = static_cast<std::uint8_t>(kOpcode);
    Base().flags = 0;
    Base().user_data = 0;
  }
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept {
    auto required = caps::RequiredCaps::For<kOpcode>();
    required.required_futex = kFutexValue;
    if constexpr (kOpcode >= abi::kOpNop128) {
      required.setup_flags |= abi::kSetupSqeMixed;
      required.require_query = true;
    }
    if constexpr (kRequiresCqe32) required.setup_flags |= abi::kSetupCqe32;
    return required;
  }
  constexpr void Encode(Sqe& sqe) const noexcept {
    static_assert(sizeof(abi::FlatSqe) == 64 && uarch::NoPadding<abi::FlatSqe>);
    auto merged = fields_;
    std::uint64_t destination_token{};
    if constexpr (!kUntracked) {
      auto& base = [&]() -> abi::FlatSqe& {
        if constexpr (kSlotsValue == 1)
          return sqe;
        else
          return sqe.sqe;
      }();
      auto& image = [&]() -> abi::FlatSqe& {
        if constexpr (kSlotsValue == 1)
          return merged;
        else
          return merged.sqe;
      }();
      destination_token = base.user_data;
      image.flags |= static_cast<std::uint8_t>(base.flags & (abi::kSqeIoLink | abi::kSqeIoHardlink));
    }
    if constexpr (kSlotsValue == 1) {
#if defined(__GNUC__) && !defined(__clang__)
      // Aggregate assignment lets GCC scalar-replace its temporary representation arrays.
      sqe = merged;
#else
      if consteval {
        sqe = merged;
      } else {
        // Whole-word writes coalesce the kernel header and its neighboring scalar fields.
        const auto words = std::bit_cast<std::array<std::uint64_t, 8>>(merged);
        template for (constexpr auto kIndex : std::define_static_array(std::array<std::size_t, 8>{0, 1, 2, 3, 4, 5, 6, 7})) {
          std::memcpy(reinterpret_cast<std::byte*>(&sqe) + kIndex * sizeof(std::uint64_t), &words[kIndex],
                      sizeof(std::uint64_t));
        }
      }
#endif
    } else {
      sqe.sqe = merged.sqe;
      // Expose each fixed extension word so the compiler keeps the inline payload in registers.
      template for (constexpr auto kIndex : std::define_static_array(std::array<std::size_t, 8>{0, 1, 2, 3, 4, 5, 6, 7})) {
        sqe.extension[kIndex] = merged.extension[kIndex];
      }
    }
    if constexpr (!kUntracked) {
      if constexpr (kSlotsValue == 1)
        sqe.user_data = MergeMemberToken(fields_.user_data, destination_token);
      else
        sqe.sqe.user_data = MergeMemberToken(fields_.sqe.user_data, destination_token);
    }
  }
  [[nodiscard]] constexpr Operation WithToken(UserDataToken token) const noexcept
    requires(!kUntracked)
  {
    auto result = *this;
    result.Base().user_data = token.raw();
    return result;
  }
  [[nodiscard]] constexpr Operation WithToken(UserDataToken) const noexcept
    requires(kUntracked)
  = delete ("untracked operations require kNullToken");
  template <std::uint32_t kFlags>
    requires((kFlags & (abi::kSqeIoLink | abi::kSqeIoHardlink)) != 0)
  [[nodiscard]] constexpr Operation WithSqeFlags() const noexcept = delete ("linked chains must use ChainShape");
  template <std::uint32_t kFlags>
    requires((kFlags & (abi::kSqeIoLink | abi::kSqeIoHardlink)) == 0 && (kFlags & abi::kSqeIoDrain) != 0)
  [[nodiscard]] constexpr Operation WithSqeFlags() const noexcept = delete ("subsystem operations cannot use IOSQE_IO_DRAIN");
  template <std::uint32_t kFlags>
    requires((kFlags & (abi::kSqeIoLink | abi::kSqeIoHardlink | abi::kSqeIoDrain)) == 0 && !kUntracked &&
             (kFlags & abi::kSqeCqeSkipSuccess) != 0)
  [[nodiscard]] constexpr Operation WithSqeFlags() const noexcept = delete ("tracked operations require CQEs");
  template <std::uint32_t kFlags>
    requires((kFlags & (abi::kSqeIoLink | abi::kSqeIoHardlink | abi::kSqeIoDrain)) == 0 &&
             (kUntracked || (kFlags & abi::kSqeCqeSkipSuccess) == 0))
  [[nodiscard]] constexpr Operation WithSqeFlags() const noexcept {
    constexpr auto allowed = abi::kSqeFixedFile | abi::kSqeAsync | abi::kSqeBufferSelect | abi::kSqeCqeSkipSuccess;
    static_assert((kFlags & ~allowed) == 0, "unsupported SQE flags");
    auto result = *this;
    result.Base().flags |= static_cast<std::uint8_t>(kFlags);
    return result;
  }
};
template <class Op>
[[nodiscard]] constexpr Op Make(abi::FlatSqe fields) noexcept {
  if constexpr (Op::kSqeSlots == 1)
    return Op{fields};
  else {
    abi::FlatSqe128 wide{};
    wide.sqe = fields;
    return Op{wide};
  }
}
}  // namespace detail

template <class Op>
concept OpDescriptor =
    requires(const Op& op, typename Op::Sqe& sqe) {
      { Op::kOpcode } -> std::convertible_to<std::uint32_t>;
      { Op::kSqeSlots } -> std::convertible_to<std::size_t>;
      { Op::kKind } -> std::convertible_to<OpKind>;
      { Op::kRequiresCqe32 } -> std::convertible_to<bool>;
      { Op::RequiredCaps() } -> std::same_as<caps::RequiredCaps>;
      { op.Encode(sqe) } noexcept -> std::same_as<void>;
    } && Op::kOpcode < abi::kOpLast && (Op::kSqeSlots == 1 || Op::kSqeSlots == 2) &&
    std::same_as<typename Op::Sqe, std::conditional_t<Op::kSqeSlots == 1, abi::FlatSqe, abi::FlatSqe128>>;
using Nop = detail::Operation<abi::kOpNop>;
using Nop128 = detail::Operation<abi::kOpNop128, OpKind::kOneShot, 2>;
using Timeout = detail::Operation<abi::kOpTimeout>;
using TimeoutRemove = detail::Operation<abi::kOpTimeoutRemove>;
using AsyncCancel = detail::Operation<abi::kOpAsyncCancel>;
using LinkTimeout = detail::Operation<abi::kOpLinkTimeout>;
using EpollCtl = detail::Operation<abi::kOpEpollCtl>;
using MsgRing = detail::Operation<abi::kOpMsgRing>;
using Waitid = detail::Operation<abi::kOpWaitid>;
using FixedFdInstall = detail::Operation<abi::kOpFixedFdInstall>;
using EpollWait = detail::Operation<abi::kOpEpollWait>;
using Pipe = detail::Operation<abi::kOpPipe>;
using UntrackedNop = detail::Operation<abi::kOpNop, OpKind::kOneShot, 1, false, 0, true>;
[[nodiscard]] constexpr Nop PrepareNop() noexcept { return {}; }
[[nodiscard]] constexpr Nop128 PrepareNop128() noexcept { return {}; }
[[nodiscard]] constexpr UntrackedNop PreparePadNop() noexcept { return UntrackedNop{}.WithSqeFlags<abi::kSqeCqeSkipSuccess>(); }
[[nodiscard]] constexpr UntrackedNop PrepareSkipSuccessNop() noexcept { return PreparePadNop(); }
[[nodiscard]] constexpr Timeout PrepareTimeout(UserAddress time, std::uint32_t count, std::uint32_t flags = 0) noexcept {
  return Timeout{{.fd = -1, .offset = count, .address = time.Raw(), .len = 1, .operation_flags = flags}};
}
[[nodiscard]] constexpr TimeoutRemove PrepareTimeoutRemove(UserDataToken target, std::uint32_t flags = 0) noexcept {
  return TimeoutRemove{{.fd = -1, .address = target.raw(), .operation_flags = flags}};
}
[[nodiscard]] constexpr TimeoutRemove PrepareTimeoutUpdate(UserDataToken target, UserAddress time,
                                                           std::uint32_t flags = 0) noexcept {
  return TimeoutRemove{
      {.fd = -1, .offset = time.Raw(), .address = target.raw(), .operation_flags = flags | abi::kTimeoutUpdate}};
}
[[nodiscard]] constexpr AsyncCancel PrepareAsyncCancel(UserDataToken target, std::uint32_t flags = 0) noexcept {
  return AsyncCancel{{.fd = -1, .address = target.raw(), .operation_flags = flags}};
}
[[nodiscard]] constexpr AsyncCancel PrepareAsyncCancelFd(std::int32_t fd, std::uint32_t flags = 0) noexcept {
  return AsyncCancel{{.fd = fd, .operation_flags = flags | abi::kAsyncCancelFd}};
}
[[nodiscard]] constexpr LinkTimeout PrepareLinkTimeout(UserAddress time, std::uint32_t flags = 0) noexcept {
  return LinkTimeout{{.fd = -1, .address = time.Raw(), .len = 1, .operation_flags = flags}};
}
[[nodiscard]] constexpr EpollCtl PrepareEpollCtl(std::int32_t epfd, std::int32_t fd, std::uint32_t operation,
                                                 UserAddress event) noexcept {
  return EpollCtl{{.fd = epfd, .offset = static_cast<std::uint32_t>(fd), .address = event.Raw(), .len = operation}};
}
[[nodiscard]] constexpr MsgRing PrepareMsgRing(std::int32_t fd, std::uint32_t result, std::uint64_t data,
                                               std::uint32_t flags = 0) noexcept {
  return MsgRing{{.fd = fd, .offset = data, .len = result, .operation_flags = flags}};
}
[[nodiscard]] constexpr MsgRing PrepareMsgRingSendFd(std::int32_t fd, std::int32_t source_fd, std::uint32_t target_file_index,
                                                     std::uint64_t data, std::uint32_t flags = 0) noexcept {
  return MsgRing{{.fd = fd,
                  .offset = data,
                  .address = abi::kMsgSendFd,
                  .operation_flags = flags,
                  .file_or_splice = detail::EncodeDirectFileIndex(target_file_index),
                  .address3 = static_cast<std::uint32_t>(source_fd)}};
}
[[nodiscard]] constexpr Waitid PrepareWaitid(std::uint32_t type, std::int32_t id, UserAddress info, std::uint32_t options,
                                             std::uint32_t flags = 0) noexcept {
  return Waitid{{.fd = id, .offset = info.Raw(), .len = type, .operation_flags = flags, .file_or_splice = options}};
}
[[nodiscard]] constexpr FixedFdInstall PrepareFixedFdInstall(std::int32_t fd, std::uint32_t flags = 0) noexcept {
  return FixedFdInstall{{.fd = fd, .operation_flags = flags}}.WithSqeFlags<abi::kSqeFixedFile>();
}
[[nodiscard]] constexpr EpollWait PrepareEpollWait(std::int32_t fd, UserAddress events, std::uint32_t count,
                                                   std::uint32_t flags = 0) noexcept {
  return EpollWait{{.fd = fd, .address = events.Raw(), .len = count, .operation_flags = flags}};
}
[[nodiscard]] constexpr Pipe PreparePipe(UserAddress fds, std::uint32_t flags = 0) noexcept {
  return Pipe{{.fd = 0, .address = fds.Raw(), .operation_flags = flags}};
}
}  // namespace liburing::ops
