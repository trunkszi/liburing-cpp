/** @file @brief Copied CQE values remain valid after the ring head advances. */
#pragma once
#include <cstdint>
#include <liburing/core/user_data_token.hpp>

namespace liburing {
inline constexpr std::uint32_t kCqeFlagMore = 1U << 1U;
inline constexpr std::uint32_t kCqeFlagNotif = 1U << 3U;
struct Completion {
  UserDataToken token;
  std::int32_t result;
  std::uint32_t flags;
};
struct CqeExtended {
  Completion completion;
  std::uint64_t extra[2];
};
struct Delivery {
  OperationKey key;
  Completion completion;
};
static_assert(sizeof(Completion) == 16 && uarch::NoPadding<Completion> && uarch::GprPair<Completion>);
static_assert(sizeof(CqeExtended) == 32 && uarch::NoPadding<CqeExtended>);
static_assert(sizeof(Delivery) == 32 && uarch::NoPadding<Delivery>);
static_assert(std::is_trivially_copyable_v<Delivery> && std::is_standard_layout_v<Delivery>);
}  // namespace liburing
