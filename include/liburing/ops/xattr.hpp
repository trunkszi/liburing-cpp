/** @file @brief Xattr names, values, and paths occupy distinct kernel address fields. */
#pragma once
#include <liburing/ops/core.hpp>
namespace liburing::ops {
using Fsetxattr = detail::Operation<abi::kOpFsetxattr>;
using Setxattr = detail::Operation<abi::kOpSetxattr>;
using Fgetxattr = detail::Operation<abi::kOpFgetxattr>;
using Getxattr = detail::Operation<abi::kOpGetxattr>;
[[nodiscard]] constexpr Fsetxattr PrepareFsetxattr(std::int32_t fd, UserAddress name, UserAddress value, std::uint32_t length,
                                                   std::uint32_t flags = 0) noexcept {
  return Fsetxattr{{.fd = fd, .offset = value.Raw(), .address = name.Raw(), .len = length, .operation_flags = flags}};
}
[[nodiscard]] constexpr Setxattr PrepareSetxattr(UserAddress name, UserAddress value, UserAddress path, std::uint32_t length,
                                                 std::uint32_t flags = 0) noexcept {
  return Setxattr{
      {.fd = 0, .offset = value.Raw(), .address = name.Raw(), .len = length, .operation_flags = flags, .address3 = path.Raw()}};
}
[[nodiscard]] constexpr Fgetxattr PrepareFgetxattr(std::int32_t fd, UserAddress name, UserAddress value,
                                                   std::uint32_t length) noexcept {
  return Fgetxattr{{.fd = fd, .offset = value.Raw(), .address = name.Raw(), .len = length}};
}
[[nodiscard]] constexpr Getxattr PrepareGetxattr(UserAddress name, UserAddress value, UserAddress path,
                                                 std::uint32_t length) noexcept {
  return Getxattr{{.fd = 0, .offset = value.Raw(), .address = name.Raw(), .len = length, .address3 = path.Raw()}};
}
}  // namespace liburing::ops
