#include <liburing/ops/fs.hpp>
#include <liburing/ops/futex.hpp>
#include <liburing/ops/net.hpp>
#include <liburing/ops/uring_cmd.hpp>
extern "C" void OpsEncodeNop(liburing::abi::FlatSqe* destination, liburing::UserDataToken token) noexcept {
  liburing::ops::PrepareNop().WithToken(token).Encode(*destination);
}
extern "C" void OpsEncodeRead(liburing::abi::FlatSqe* destination, std::int32_t fd, liburing::ops::UserAddress buffer,
                              std::uint32_t length, std::uint64_t offset, liburing::UserDataToken token) noexcept {
  liburing::ops::PrepareRead(fd, buffer, length, offset).WithToken(token).Encode(*destination);
}
extern "C" void OpsEncodeFutex(liburing::abi::FlatSqe* destination, std::uint32_t* word, std::uint32_t value) noexcept {
  liburing::ops::PrepareFutexWait(word, value).Encode(*destination);
}
extern "C" void OpsEncodeMultishot(liburing::abi::FlatSqe* destination, std::int32_t fd, std::uint16_t group) noexcept {
  liburing::ops::PrepareRecvMultishot(fd, liburing::ops::UserAddress{}, 0, 0, group).Encode(*destination);
}
extern "C" void OpsEncodeCommand128(liburing::abi::FlatSqe128* destination, std::int32_t fd, std::uint32_t command,
                                    const std::array<std::byte, 80>* payload) noexcept {
  liburing::ops::PrepareUringCmd128(fd, command, *payload).Encode(*destination);
}
extern "C" std::uint64_t OpsAddressValue(liburing::ops::UserAddress address) noexcept { return address.Raw(); }
