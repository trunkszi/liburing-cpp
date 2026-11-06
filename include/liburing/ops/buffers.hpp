/** @file @brief Legacy provided-buffer commands carry group and buffer IDs without owning storage. */
#pragma once
#include <liburing/ops/core.hpp>
namespace liburing::ops {
using ProvideBuffers = detail::Operation<abi::kOpProvideBuffers>;
using RemoveBuffers = detail::Operation<abi::kOpRemoveBuffers>;
[[nodiscard]] constexpr ProvideBuffers PrepareProvideBuffers(UserAddress address, std::uint32_t length, std::int32_t count,
                                                             std::uint16_t group, std::uint16_t first_id) noexcept {
  return ProvideBuffers{{.fd = count, .offset = first_id, .address = address.Raw(), .len = length, .buffer_index = group}};
}
[[nodiscard]] constexpr RemoveBuffers PrepareRemoveBuffers(std::int32_t count, std::uint16_t group) noexcept {
  return RemoveBuffers{{.fd = count, .buffer_index = group}};
}
}  // namespace liburing::ops
