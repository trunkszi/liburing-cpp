/** @file @brief NAPI busy-poll registration and static NAPI ID management wrappers. */
#pragma once

#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/error/uring_error.hpp>

namespace liburing::resources {

class NapiController final {
 public:
  template <class Ring>
  [[nodiscard]] static Result<abi::IoUringNapi> RegisterDynamicNapi(Ring& ring, std::uint32_t busy_poll_to_usec,
                                                                    bool prefer_busy_poll) noexcept {
    abi::IoUringNapi cfg{
        .busy_poll_to = busy_poll_to_usec,
        .prefer_busy_poll = static_cast<std::uint8_t>(prefer_busy_poll ? 1U : 0U),
        .opcode = static_cast<std::uint8_t>(abi::kIoUringNapiRegisterOp),
        .op_param = abi::kIoUringNapiTrackingDynamic,
    };
    auto res = ring.Register(abi::kRegisterNapi, &cfg, 1U);
    if (!res) return std::unexpected(res.error());
    return cfg;
  }

  template <class Ring>
  [[nodiscard]] static Result<abi::IoUringNapi> RegisterStaticNapi(Ring& ring, std::uint32_t busy_poll_to_usec,
                                                                   bool prefer_busy_poll) noexcept {
    abi::IoUringNapi cfg{
        .busy_poll_to = busy_poll_to_usec,
        .prefer_busy_poll = static_cast<std::uint8_t>(prefer_busy_poll ? 1U : 0U),
        .opcode = static_cast<std::uint8_t>(abi::kIoUringNapiRegisterOp),
        .op_param = abi::kIoUringNapiTrackingStatic,
    };
    auto res = ring.Register(abi::kRegisterNapi, &cfg, 1U);
    if (!res) return std::unexpected(res.error());
    return cfg;
  }

  template <class Ring>
  [[nodiscard]] static Result<void> AddStaticNapiId(Ring& ring, std::uint32_t napi_id) noexcept {
    abi::IoUringNapi cfg{
        .opcode = static_cast<std::uint8_t>(abi::kIoUringNapiStaticAddId),
        .op_param = napi_id,
    };
    auto res = ring.Register(abi::kRegisterNapi, &cfg, 1U);
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] static Result<void> DeleteStaticNapiId(Ring& ring, std::uint32_t napi_id) noexcept {
    abi::IoUringNapi cfg{
        .opcode = static_cast<std::uint8_t>(abi::kIoUringNapiStaticDelId),
        .op_param = napi_id,
    };
    auto res = ring.Register(abi::kRegisterNapi, &cfg, 1U);
    if (!res) return std::unexpected(res.error());
    return {};
  }

  template <class Ring>
  [[nodiscard]] static Result<abi::IoUringNapi> UnregisterNapi(Ring& ring) noexcept {
    abi::IoUringNapi previous{};
    auto res = ring.Register(abi::kUnregisterNapi, &previous, 1U);
    if (!res) return std::unexpected(res.error());
    return previous;
  }
};

}  // namespace liburing::resources

namespace liburing {
using resources::NapiController;
}  // namespace liburing
