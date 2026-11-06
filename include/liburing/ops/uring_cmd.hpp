/** @file @brief Inline command bytes remain part of the SQE until kernel consumption. */
#pragma once
#include <array>
#include <bit>
#include <cstring>
#include <liburing/ops/core.hpp>
namespace liburing::ops {
template <bool kCqe32 = false>
using UringCmdOp = detail::Operation<abi::kOpUringCmd, OpKind::kOneShot, 1, kCqe32>;
template <bool kCqe32 = false>
// Keep only the owned command and its parameters so preparation avoids a full SQE temporary.
class UringCmd128Op {
  std::int32_t fd_{-1};
  std::uint32_t command_{};
  std::uint32_t uring_flags_{};
  std::uint32_t sqe_flags_{};
  UserDataToken token_{};
  std::array<std::byte, 80> payload_{};
  using Metadata = detail::Operation<abi::kOpUringCmd128, OpKind::kOneShot, 2, kCqe32>;
  [[gnu::always_inline]] constexpr void CopyPayloadWords(abi::FlatSqe128& sqe) const noexcept {
    const auto words = std::bit_cast<std::array<std::uint64_t, 10>>(payload_);
    sqe.sqe.address3 = words[0];
    sqe.sqe.extra = words[1];
    template for (constexpr auto kIndex : std::define_static_array(std::array<std::size_t, 8>{0, 1, 2, 3, 4, 5, 6, 7})) {
      sqe.extension[kIndex] = words[kIndex + 2];
    }
  }

 public:
  using Sqe = abi::FlatSqe128;
  static constexpr std::uint32_t kOpcode = abi::kOpUringCmd128;
  static constexpr std::size_t kSqeSlots = 2;
  static constexpr OpKind kKind = OpKind::kOneShot;
  static constexpr bool kRequiresCqe32 = kCqe32;
  static constexpr bool kUntracked = false;
  constexpr UringCmd128Op() noexcept = default;
  constexpr UringCmd128Op(std::int32_t fd, std::uint32_t command, const std::array<std::byte, 80>& payload,
                          std::uint32_t flags = 0) noexcept
      : fd_(fd), command_(command), uring_flags_(flags), payload_(payload) {}
  [[nodiscard]] static consteval caps::RequiredCaps RequiredCaps() noexcept { return Metadata::RequiredCaps(); }
  [[nodiscard]] constexpr UringCmd128Op WithToken(UserDataToken token) const noexcept {
    auto result = *this;
    result.token_ = token;
    return result;
  }
  template <std::uint32_t kFlags>
    requires((kFlags & (abi::kSqeIoLink | abi::kSqeIoHardlink)) != 0)
  [[nodiscard]] constexpr UringCmd128Op WithSqeFlags() const noexcept = delete ("linked chains must use ChainShape");
  template <std::uint32_t kFlags>
    requires((kFlags & (abi::kSqeIoLink | abi::kSqeIoHardlink)) == 0)
  [[nodiscard]] constexpr UringCmd128Op WithSqeFlags() const noexcept {
    static_assert((kFlags & abi::kSqeIoDrain) == 0, "subsystem operations cannot use IOSQE_IO_DRAIN");
    static_assert((kFlags & abi::kSqeCqeSkipSuccess) == 0, "tracked operations require CQEs");
    constexpr auto allowed = abi::kSqeFixedFile | abi::kSqeAsync | abi::kSqeBufferSelect;
    static_assert((kFlags & ~allowed) == 0, "unsupported SQE flags");
    auto result = *this;
    result.sqe_flags_ |= kFlags;
    return result;
  }
  constexpr void Encode(Sqe& sqe) const noexcept {
    const auto destination_token = sqe.sqe.user_data;
    const auto preserved_link = static_cast<std::uint8_t>(sqe.sqe.flags & (abi::kSqeIoLink | abi::kSqeIoHardlink));
    sqe = {};
    sqe.sqe.opcode = static_cast<std::uint8_t>(kOpcode);
    sqe.sqe.flags = static_cast<std::uint8_t>(sqe_flags_ | preserved_link);
    sqe.sqe.fd = fd_;
    sqe.sqe.cmd_op(command_);
    sqe.sqe.uring_cmd_flags() = uring_flags_;
    sqe.sqe.user_data = detail::MergeMemberToken(token_.raw(), destination_token);
#if defined(__GNUC__) && !defined(__clang__)
    if consteval {
      CopyPayloadWords(sqe);
    } else {
      std::memcpy(sqe.command().data(), payload_.data(), payload_.size());
    }
#else
    CopyPayloadWords(sqe);
#endif
  }
};
static_assert(uarch::NoPadding<UringCmd128Op<>> && sizeof(UringCmd128Op<>) == 104);
using UringCmd = UringCmdOp<>;
using UringCmd128 = UringCmd128Op<>;
template <bool kCqe32 = false>
[[nodiscard]] constexpr UringCmdOp<kCqe32> PrepareUringCmd(std::int32_t fd, std::uint32_t command,
                                                           const std::array<std::byte, 16>& payload,
                                                           std::uint32_t flags = 0) noexcept {
  const auto words = std::bit_cast<std::array<std::uint64_t, 2>>(payload);
  return UringCmdOp<kCqe32>{{.fd = fd, .offset = command, .operation_flags = flags, .address3 = words[0], .extra = words[1]}};
}
template <bool kCqe32 = false>
[[nodiscard]] constexpr UringCmd128Op<kCqe32> PrepareUringCmd128(std::int32_t fd, std::uint32_t command,
                                                                 const std::array<std::byte, 80>& payload,
                                                                 std::uint32_t flags = 0) noexcept {
  return UringCmd128Op<kCqe32>{fd, command, payload, flags};
}
}  // namespace liburing::ops
