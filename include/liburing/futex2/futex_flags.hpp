#pragma once
#include <cstdint>
#include <liburing/error/uring_error.hpp>
#include <liburing/futex2/futex_word.hpp>
namespace liburing::futex2 {
enum class FutexScope : std::uint32_t { kShared = 0, kProcessPrivate = abi::kFutex2Private };
enum class FutexLocality : std::uint32_t {
  kDefault = 0,
  kNuma = abi::kFutex2Numa,
  kNumaMpol = abi::kFutex2Numa | abi::kFutex2Mpol
};
[[nodiscard]] constexpr Result<void> ValidateFutex2Flags(std::uint32_t flags) noexcept {
  constexpr auto allowed = abi::kFutex2SizeMask | abi::kFutex2Numa | abi::kFutex2Mpol | abi::kFutex2Private;
  if (flags & ~allowed) return std::unexpected(UringErrorCode::kInvalid);
  if ((flags & abi::kFutex2SizeMask) != abi::kFutex2SizeU32) return std::unexpected(UringErrorCode::kNotSupported);
  if ((flags & abi::kFutex2Mpol) && !(flags & abi::kFutex2Numa)) return std::unexpected(UringErrorCode::kInvalid);
  return {};
}
template <FutexScope Scope = FutexScope::kShared, FutexLocality Locality = FutexLocality::kDefault>
[[nodiscard]] consteval std::uint32_t Futex2Flags() noexcept {
  constexpr auto scope = static_cast<std::uint32_t>(Scope);
  constexpr auto locality = static_cast<std::uint32_t>(Locality);
  static_assert(Scope == FutexScope::kShared || Scope == FutexScope::kProcessPrivate, "invalid futex scope");
  static_assert(!(locality & abi::kFutex2Mpol) || (locality & abi::kFutex2Numa), "MPOL requires NUMA");
  static_assert(Locality == FutexLocality::kDefault || Locality == FutexLocality::kNuma || Locality == FutexLocality::kNumaMpol,
                "invalid futex locality");
  return abi::kFutex2SizeU32 | scope | locality;
}
struct FutexRef {
  std::uint64_t uaddr;
  std::uint32_t flags;
  std::uint32_t reserved;
};
static_assert(sizeof(FutexRef) == 16 && uarch::NoPadding<FutexRef> && uarch::RegisterPassable<FutexRef> &&
              uarch::GprPair<FutexRef>);
static_assert(uarch::reflect::byte_offset(^^FutexRef::uaddr) == 0 && uarch::reflect::byte_offset(^^FutexRef::flags) == 8 &&
              uarch::reflect::byte_offset(^^FutexRef::reserved) == 12);
[[nodiscard]] constexpr Result<void> ValidateRef(FutexRef ref) noexcept {
  auto flags = ValidateFutex2Flags(ref.flags);
  if (!flags) return flags;
  if (ref.uaddr == 0 || ref.reserved != 0) return std::unexpected(UringErrorCode::kInvalid);
  const auto aligned = (ref.flags & abi::kFutex2Numa) ? uarch::PowerOfTwo<3>::is_multiple_of(ref.uaddr)
                                                      : uarch::PowerOfTwo<2>::is_multiple_of(ref.uaddr);
  if (!aligned) return std::unexpected(UringErrorCode::kInvalid);
  return {};
}
[[nodiscard, gnu::always_inline]] inline Result<FutexRef> MakeRef(FutexWord32& word,
                                                                  FutexScope scope = FutexScope::kShared) noexcept {
  if (scope != FutexScope::kShared && scope != FutexScope::kProcessPrivate) return std::unexpected(UringErrorCode::kInvalid);
  FutexRef ref{reinterpret_cast<std::uint64_t>(&word.value), abi::kFutex2SizeU32 | static_cast<std::uint32_t>(scope), 0};
  auto valid = ValidateRef(ref);
  if (!valid) return std::unexpected(valid.error());
  return ref;
}
[[nodiscard, gnu::always_inline]] inline Result<FutexRef> MakeNumaRef(FutexNumaWord32& word,
                                                                      FutexScope scope = FutexScope::kShared,
                                                                      FutexLocality locality = FutexLocality::kNuma) noexcept {
  if (scope != FutexScope::kShared && scope != FutexScope::kProcessPrivate) return std::unexpected(UringErrorCode::kInvalid);
  if (locality != FutexLocality::kNuma && locality != FutexLocality::kNumaMpol)
    return std::unexpected(UringErrorCode::kInvalid);
  const auto flags = abi::kFutex2SizeU32 | static_cast<std::uint32_t>(scope) | static_cast<std::uint32_t>(locality);
  FutexRef ref{reinterpret_cast<std::uint64_t>(&word.value), flags, 0};
  auto valid = ValidateRef(ref);
  if (!valid) return std::unexpected(valid.error());
  return ref;
}
[[nodiscard, gnu::always_inline]] inline Result<FutexRef> MakeNumaRef(FutexNumaWord32& word, FutexScope scope,
                                                                      bool use_mpol) noexcept {
  return MakeNumaRef(word, scope, use_mpol ? FutexLocality::kNumaMpol : FutexLocality::kNuma);
}
[[nodiscard, gnu::always_inline]] inline Result<abi::FutexWaitv> MakeWaitv(FutexRef ref, std::uint32_t expected) noexcept {
  auto valid = ValidateRef(ref);
  if (!valid) return std::unexpected(valid.error());
  return abi::FutexWaitv{expected, ref.uaddr, ref.flags, 0};
}
Result<FutexRef> MakeRef(FutexWord8&, FutexScope = FutexScope::kShared) =
    delete ("Linux kernel futex_flags_valid() currently only implements FUTEX2_SIZE_U32");
Result<FutexRef> MakeRef(FutexWord16&, FutexScope = FutexScope::kShared) =
    delete ("Linux kernel futex_flags_valid() currently only implements FUTEX2_SIZE_U32");
Result<FutexRef> MakeRef(FutexWord64&, FutexScope = FutexScope::kShared) =
    delete ("Linux kernel futex_flags_valid() currently only implements FUTEX2_SIZE_U32");
Result<FutexRef> MakeNumaRef(FutexNumaWord64&, FutexScope = FutexScope::kShared, FutexLocality = FutexLocality::kNuma) =
    delete ("Linux kernel futex_flags_valid() currently only implements FUTEX2_SIZE_U32");
}  // namespace liburing::futex2
