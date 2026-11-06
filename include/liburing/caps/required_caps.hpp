#pragma once
#include <array>
#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/error/uring_error.hpp>
#include <uarch/uarch.hpp>
namespace liburing::caps {
inline constexpr std::uint32_t kFutexU32 = 1;
inline constexpr std::uint32_t kFutexNuma = 2;
inline constexpr std::uint32_t kFutexMpol = 4;
enum class KernelTier : std::uint32_t { kBaseline616, kExtendedMixed };
struct RequiredCaps {
  std::uint32_t setup_flags{};
  std::uint32_t feature_flags{};
  std::array<uarch::Bitmap64, 2> opcodes{};
  std::uint32_t required_futex{kFutexU32};
  bool require_query{};
  [[nodiscard]] constexpr Result<void> RequireOpcode(std::uint32_t opcode) noexcept {
    if (opcode >= abi::kOpLast) return std::unexpected(UringErrorCode::kInvalid);
    const auto word = uarch::PowerOfTwo<6>::div(opcode);
    const auto bit = static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(opcode));
    opcodes[word] = opcodes[word].set(bit);
    return {};
  }
  template <std::uint32_t... Opcodes>
  [[nodiscard]] static consteval RequiredCaps For() noexcept {
    static_assert(((Opcodes < abi::kOpLast) && ...), "required opcode outside UAPI");
    RequiredCaps result;
    (static_cast<void>(result.RequireOpcode(Opcodes)), ...);
    return result;
  }
};
}  // namespace liburing::caps
