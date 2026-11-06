/** @file @brief Kernel identities carry generations independently of ring lifetimes. */
#pragma once
#include <cstdint>
#include <uarch/uarch.hpp>

namespace liburing {
struct TokenSpec {
  using Word = std::uint64_t;
  std::uint32_t slot[[= uarch::bits::Field{0, 22}]];
  std::uint8_t member[[= uarch::bits::Field{22, 2}]];
  std::uint8_t subsystem[[= uarch::bits::Field{24, 8}]];
  std::uint32_t generation[[= uarch::bits::Field{32, 32}]];
};
using UserDataToken = uarch::BitWord<TokenSpec>;
inline constexpr UserDataToken kNullToken{};
inline constexpr std::uint8_t kInternalSubsystemId = 0;
inline constexpr std::uint8_t kMailboxSubsystemId = 255;

[[nodiscard]] constexpr UserDataToken MakeToken(std::uint32_t slot, std::uint8_t member, std::uint8_t subsystem,
                                                std::uint32_t generation) noexcept {
  return UserDataToken{}
      .with<^^TokenSpec::slot>(slot)
      .with<^^TokenSpec::member>(member)
      .with<^^TokenSpec::subsystem>(subsystem)
      .with<^^TokenSpec::generation>(generation);
}
[[nodiscard]] constexpr auto Slot(UserDataToken token) noexcept { return token.get<^^TokenSpec::slot>(); }
[[nodiscard]] constexpr auto Member(UserDataToken token) noexcept { return token.get<^^TokenSpec::member>(); }
[[nodiscard]] constexpr auto Subsystem(UserDataToken token) noexcept { return token.get<^^TokenSpec::subsystem>(); }
[[nodiscard]] constexpr auto Generation(UserDataToken token) noexcept { return token.get<^^TokenSpec::generation>(); }

struct OperationKey {
  UserDataToken token;
  std::uint64_t ring_instance;
  constexpr bool operator==(const OperationKey&) const noexcept = default;
};
static_assert(sizeof(UserDataToken) == 8 && uarch::NoPadding<UserDataToken> && uarch::RegisterPassable<UserDataToken>);
static_assert(sizeof(OperationKey) == 16 && uarch::NoPadding<OperationKey> && uarch::GprPair<OperationKey>);
static_assert(uarch::RegisterLeaf<uarch::PoolIndex<TokenSpec>>);
}  // namespace liburing
