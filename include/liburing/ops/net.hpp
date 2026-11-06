/** @file @brief Network descriptors distinguish multishot and zero-copy kernel ownership. */
#pragma once
#include <liburing/ops/core.hpp>
namespace liburing::ops {
using PollAdd = detail::Operation<abi::kOpPollAdd>;
using PollMultishot = detail::Operation<abi::kOpPollAdd, OpKind::kMultishot>;
using PollRemove = detail::Operation<abi::kOpPollRemove>;
using Sendmsg = detail::Operation<abi::kOpSendmsg>;
using Recvmsg = detail::Operation<abi::kOpRecvmsg>;
using RecvmsgMultishot = detail::Operation<abi::kOpRecvmsg, OpKind::kMultishot>;
using Accept = detail::Operation<abi::kOpAccept>;
using AcceptMultishot = detail::Operation<abi::kOpAccept, OpKind::kMultishot>;
using Connect = detail::Operation<abi::kOpConnect>;
using Send = detail::Operation<abi::kOpSend>;
using Recv = detail::Operation<abi::kOpRecv>;
using RecvMultishot = detail::Operation<abi::kOpRecv, OpKind::kMultishot>;
using Shutdown = detail::Operation<abi::kOpShutdown>;
using Socket = detail::Operation<abi::kOpSocket>;
using SendZc = detail::Operation<abi::kOpSendZc, OpKind::kZeroCopy>;
using SendmsgZc = detail::Operation<abi::kOpSendmsgZc, OpKind::kZeroCopy>;
using Bind = detail::Operation<abi::kOpBind>;
using Listen = detail::Operation<abi::kOpListen>;
using RecvZc = detail::Operation<abi::kOpRecvZc, OpKind::kMultishot, 1, true>;
[[nodiscard]] constexpr PollAdd PreparePollAdd(std::int32_t fd, std::uint32_t events) noexcept {
  return PollAdd{{.fd = fd, .operation_flags = events}};
}
[[nodiscard]] constexpr PollMultishot PreparePollMultishot(std::int32_t fd, std::uint32_t events) noexcept {
  return PollMultishot{{.fd = fd, .len = abi::kPollAddMulti, .operation_flags = events}};
}
[[nodiscard]] constexpr PollRemove PreparePollRemove(UserDataToken target) noexcept {
  return PollRemove{{.fd = -1, .address = target.raw()}};
}
[[nodiscard]] constexpr PollRemove PreparePollUpdate(UserDataToken old_token, UserDataToken new_token, std::uint32_t events,
                                                     std::uint32_t flags) noexcept {
  return PollRemove{{.fd = -1, .offset = new_token.raw(), .address = old_token.raw(), .len = flags, .operation_flags = events}};
}
[[nodiscard]] constexpr Sendmsg PrepareSendmsg(std::int32_t fd, UserAddress message, std::uint32_t flags = 0) noexcept {
  return Sendmsg{{.fd = fd, .address = message.Raw(), .len = 1, .operation_flags = flags}};
}
[[nodiscard]] constexpr Recvmsg PrepareRecvmsg(std::int32_t fd, UserAddress message, std::uint32_t flags = 0) noexcept {
  return Recvmsg{{.fd = fd, .address = message.Raw(), .len = 1, .operation_flags = flags}};
}
[[nodiscard]] constexpr RecvmsgMultishot PrepareRecvmsgMultishot(std::int32_t fd, UserAddress message, std::uint32_t flags,
                                                                 std::uint16_t group) noexcept {
  return RecvmsgMultishot{{.ioprio = static_cast<std::uint16_t>(abi::kRecvMultishot),
                           .fd = fd,
                           .address = message.Raw(),
                           .len = 1,
                           .operation_flags = flags,
                           .buffer_index = group}}
      .WithSqeFlags<abi::kSqeBufferSelect>();
}
[[nodiscard]] constexpr Accept PrepareAccept(std::int32_t fd, UserAddress address, UserAddress length,
                                             std::uint32_t flags = 0) noexcept {
  return Accept{{.fd = fd, .offset = length.Raw(), .address = address.Raw(), .operation_flags = flags}};
}
[[nodiscard]] constexpr AcceptMultishot PrepareAcceptMultishot(std::int32_t fd, UserAddress address, UserAddress length,
                                                               std::uint32_t flags = 0) noexcept {
  return AcceptMultishot{{.ioprio = static_cast<std::uint16_t>(abi::kAcceptMultishot),
                          .fd = fd,
                          .offset = length.Raw(),
                          .address = address.Raw(),
                          .operation_flags = flags}};
}
[[nodiscard]] constexpr Connect PrepareConnect(std::int32_t fd, UserAddress address, std::uint32_t length) noexcept {
  return Connect{{.fd = fd, .offset = length, .address = address.Raw()}};
}
[[nodiscard]] constexpr Send PrepareSend(std::int32_t fd, UserAddress buffer, std::uint32_t length,
                                         std::uint32_t flags = 0) noexcept {
  return Send{{.fd = fd, .address = buffer.Raw(), .len = length, .operation_flags = flags}};
}
[[nodiscard]] constexpr Recv PrepareRecv(std::int32_t fd, UserAddress buffer, std::uint32_t length,
                                         std::uint32_t flags = 0) noexcept {
  return Recv{{.fd = fd, .address = buffer.Raw(), .len = length, .operation_flags = flags}};
}
[[nodiscard]] constexpr RecvMultishot PrepareRecvMultishot(std::int32_t fd, UserAddress buffer, std::uint32_t length,
                                                           std::uint32_t flags, std::uint16_t group) noexcept {
  return RecvMultishot{{.ioprio = static_cast<std::uint16_t>(abi::kRecvMultishot),
                        .fd = fd,
                        .address = buffer.Raw(),
                        .len = length,
                        .operation_flags = flags,
                        .buffer_index = group}}
      .WithSqeFlags<abi::kSqeBufferSelect>();
}
[[nodiscard]] constexpr Shutdown PrepareShutdown(std::int32_t fd, std::uint32_t how) noexcept {
  return Shutdown{{.fd = fd, .len = how}};
}
[[nodiscard]] constexpr Socket PrepareSocket(std::int32_t domain, std::uint32_t type, std::uint32_t protocol,
                                             std::uint32_t flags = 0) noexcept {
  return Socket{{.fd = domain, .offset = type, .len = protocol, .operation_flags = flags}};
}
[[nodiscard]] constexpr Socket PrepareSocketDirect(std::int32_t domain, std::uint32_t type, std::uint32_t protocol,
                                                   std::uint32_t file_index, std::uint32_t flags = 0) noexcept {
  return Socket{{.fd = domain,
                 .offset = type,
                 .len = protocol,
                 .operation_flags = flags,
                 .file_or_splice = detail::EncodeDirectFileIndex(file_index)}};
}
[[nodiscard]] constexpr Accept PrepareAcceptDirect(std::int32_t fd, UserAddress address, UserAddress length,
                                                   std::uint32_t file_index, std::uint32_t flags = 0) noexcept {
  return Accept{{.fd = fd,
                 .offset = length.Raw(),
                 .address = address.Raw(),
                 .operation_flags = flags,
                 .file_or_splice = detail::EncodeDirectFileIndex(file_index)}};
}
[[nodiscard]] constexpr SendZc PrepareSendZc(std::int32_t fd, UserAddress buffer, std::uint32_t length, std::uint32_t flags = 0,
                                             std::uint16_t zc_flags = 0) noexcept {
  return SendZc{{.ioprio = zc_flags, .fd = fd, .address = buffer.Raw(), .len = length, .operation_flags = flags}};
}
[[nodiscard]] constexpr SendmsgZc PrepareSendmsgZc(std::int32_t fd, UserAddress message, std::uint32_t flags = 0,
                                                   std::uint16_t zc_flags = 0) noexcept {
  return SendmsgZc{{.ioprio = zc_flags, .fd = fd, .address = message.Raw(), .len = 1, .operation_flags = flags}};
}
[[nodiscard]] constexpr Bind PrepareBind(std::int32_t fd, UserAddress address, std::uint32_t length) noexcept {
  return Bind{{.fd = fd, .offset = length, .address = address.Raw()}};
}
[[nodiscard]] constexpr Listen PrepareListen(std::int32_t fd, std::uint32_t backlog) noexcept {
  return Listen{{.fd = fd, .len = backlog}};
}
[[nodiscard]] constexpr RecvZc PrepareRecvZc(std::int32_t fd, std::uint32_t length, std::uint32_t ifq_index) noexcept {
  return RecvZc{
      {.ioprio = static_cast<std::uint16_t>(abi::kRecvMultishot), .fd = fd, .len = length, .file_or_splice = ifq_index}};
}
}  // namespace liburing::ops
