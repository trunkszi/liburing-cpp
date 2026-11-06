/** @file @brief Compile-time and runtime ring geometry descriptors and single-issuer flag builders. */
#pragma once

#include <concepts>
#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <uarch/uarch.hpp>

namespace liburing::ring {

enum class RingMode : std::uint8_t {
  kStandard64_16 = 0,      // NOLINT(readability-identifier-naming)
  kFull128_32 = 1,         // NOLINT(readability-identifier-naming)
  kMixed64_128_16_32 = 2,  // NOLINT(readability-identifier-naming)
};

enum class SqRingMode : std::uint8_t {
  kSq64 = 0,
  kSq128 = 1,
  kSqMixed = 2,
};

enum class CqRingMode : std::uint8_t {
  kCq16 = 0,
  kCq32 = 1,
  kCqMixed = 2,
};

struct Standard64_16 {  // NOLINT(readability-identifier-naming)
  static constexpr RingMode kMode = RingMode::kStandard64_16;
  static constexpr SqRingMode kSqMode = SqRingMode::kSq64;
  static constexpr CqRingMode kCqMode = CqRingMode::kCq16;
  static constexpr std::uint32_t kSetupFlags = 0U;
  static constexpr std::uint32_t kSqeSlotBytes = 64U;
  static constexpr std::uint32_t kMaxSqeBytes = 64U;
  static constexpr std::uint32_t kCqeSlotBytes = 16U;
  static constexpr std::uint32_t kMaxCqeBytes = 16U;
  static constexpr bool kSupportsSqe128 = false;
  static constexpr bool kSupportsCqe32 = false;
  static constexpr bool kIsMixedSqe = false;
  static constexpr bool kIsMixedCqe = false;
};

struct Full128_32 {  // NOLINT(readability-identifier-naming)
  static constexpr RingMode kMode = RingMode::kFull128_32;
  static constexpr SqRingMode kSqMode = SqRingMode::kSq128;
  static constexpr CqRingMode kCqMode = CqRingMode::kCq32;
  static constexpr std::uint32_t kSetupFlags = abi::kSetupSqe128 | abi::kSetupCqe32;
  static constexpr std::uint32_t kSqeSlotBytes = 128U;
  static constexpr std::uint32_t kMaxSqeBytes = 128U;
  static constexpr std::uint32_t kCqeSlotBytes = 32U;
  static constexpr std::uint32_t kMaxCqeBytes = 32U;
  static constexpr bool kSupportsSqe128 = true;
  static constexpr bool kSupportsCqe32 = true;
  static constexpr bool kIsMixedSqe = false;
  static constexpr bool kIsMixedCqe = false;
};

struct Mixed64_128_16_32 {  // NOLINT(readability-identifier-naming)
  static constexpr RingMode kMode = RingMode::kMixed64_128_16_32;
  static constexpr SqRingMode kSqMode = SqRingMode::kSqMixed;
  static constexpr CqRingMode kCqMode = CqRingMode::kCqMixed;
  static constexpr std::uint32_t kSetupFlags = abi::kSetupSqeMixed | abi::kSetupCqeMixed;
  static constexpr std::uint32_t kSqeSlotBytes = 64U;
  static constexpr std::uint32_t kMaxSqeBytes = 128U;
  static constexpr std::uint32_t kCqeSlotBytes = 16U;
  static constexpr std::uint32_t kMaxCqeBytes = 32U;
  static constexpr bool kSupportsSqe128 = true;
  static constexpr bool kSupportsCqe32 = true;
  static constexpr bool kIsMixedSqe = true;
  static constexpr bool kIsMixedCqe = true;
};

template <class Geometry>
concept RingGeometryDescriptor = requires {
  { Geometry::kMode } -> std::convertible_to<RingMode>;
  { Geometry::kSetupFlags } -> std::convertible_to<std::uint32_t>;
  { Geometry::kSqeSlotBytes } -> std::convertible_to<std::uint32_t>;
  { Geometry::kCqeSlotBytes } -> std::convertible_to<std::uint32_t>;
};

inline constexpr std::uint32_t kSingleIssuerSetupFlags =
    abi::kSetupSingleIssuer | abi::kSetupCoopTaskrun | abi::kSetupTaskrunFlag | abi::kSetupDeferTaskrun | abi::kSetupNoSqArray;

[[nodiscard]] constexpr std::uint32_t ModeSetupFlags(RingMode mode) noexcept {
  switch (mode) {
    case RingMode::kStandard64_16:
      return Standard64_16::kSetupFlags;
    case RingMode::kFull128_32:
      return Full128_32::kSetupFlags;
    case RingMode::kMixed64_128_16_32:
      return Mixed64_128_16_32::kSetupFlags;
  }
  return 0U;
}

[[nodiscard]] constexpr SqRingMode ToSqRingMode(RingMode mode) noexcept {
  switch (mode) {
    case RingMode::kStandard64_16:
      return SqRingMode::kSq64;
    case RingMode::kFull128_32:
      return SqRingMode::kSq128;
    case RingMode::kMixed64_128_16_32:
      return SqRingMode::kSqMixed;
  }
  return SqRingMode::kSq64;
}

[[nodiscard]] constexpr CqRingMode ToCqRingMode(RingMode mode) noexcept {
  switch (mode) {
    case RingMode::kStandard64_16:
      return CqRingMode::kCq16;
    case RingMode::kFull128_32:
      return CqRingMode::kCq32;
    case RingMode::kMixed64_128_16_32:
      return CqRingMode::kCqMixed;
  }
  return CqRingMode::kCq16;
}

[[nodiscard]] constexpr RingMode ToRingMode(SqRingMode sq_mode) noexcept {
  switch (sq_mode) {
    case SqRingMode::kSq64:
      return RingMode::kStandard64_16;
    case SqRingMode::kSq128:
      return RingMode::kFull128_32;
    case SqRingMode::kSqMixed:
      return RingMode::kMixed64_128_16_32;
  }
  return RingMode::kStandard64_16;
}

[[nodiscard]] constexpr RingMode ToRingMode(CqRingMode cq_mode) noexcept {
  switch (cq_mode) {
    case CqRingMode::kCq16:
      return RingMode::kStandard64_16;
    case CqRingMode::kCq32:
      return RingMode::kFull128_32;
    case CqRingMode::kCqMixed:
      return RingMode::kMixed64_128_16_32;
  }
  return RingMode::kStandard64_16;
}

[[nodiscard]] constexpr SqRingMode DetectSqRingMode(std::uint32_t setup_flags) noexcept {
  if ((setup_flags & abi::kSetupSqeMixed) != 0U) {
    return SqRingMode::kSqMixed;
  }
  if ((setup_flags & abi::kSetupSqe128) != 0U) {
    return SqRingMode::kSq128;
  }
  return SqRingMode::kSq64;
}

[[nodiscard]] constexpr CqRingMode DetectCqRingMode(std::uint32_t setup_flags) noexcept {
  if ((setup_flags & abi::kSetupCqeMixed) != 0U) {
    return CqRingMode::kCqMixed;
  }
  if ((setup_flags & abi::kSetupCqe32) != 0U) {
    return CqRingMode::kCq32;
  }
  return CqRingMode::kCq16;
}

[[nodiscard]] constexpr RingMode DetectRingMode(std::uint32_t setup_flags) noexcept {
  if ((setup_flags & (abi::kSetupSqeMixed | abi::kSetupCqeMixed)) != 0U) {
    return RingMode::kMixed64_128_16_32;
  }
  if ((setup_flags & abi::kSetupSqe128) != 0U) {
    return RingMode::kFull128_32;
  }
  return RingMode::kStandard64_16;
}

[[nodiscard]] constexpr std::uint32_t BuildSingleIssuerFlags(RingMode mode = RingMode::kStandard64_16,
                                                             std::uint32_t extra_flags = 0U) noexcept {
  return kSingleIssuerSetupFlags | ModeSetupFlags(mode) | extra_flags;
}

template <RingGeometryDescriptor Geometry = Standard64_16>
[[nodiscard]] constexpr abi::IoUringParams MakeSingleIssuerParams(std::uint32_t cq_entries = 0U,
                                                                  std::uint32_t extra_flags = 0U) noexcept {
  abi::IoUringParams params{};
  params.flags = BuildSingleIssuerFlags(Geometry::kMode, extra_flags);
  if (cq_entries != 0U) {
    params.flags |= abi::kSetupCqSize;
    params.cq_entries = cq_entries;
  }
  return params;
}

// In SQE_MIXED mode, 128-byte SQEs are selected by 128-byte opcodes rather than an SQE flag bit.
[[nodiscard]] constexpr bool Is128ByteOpcode(std::uint8_t opcode) noexcept {
  return opcode == static_cast<std::uint8_t>(abi::kOpNop128) || opcode == static_cast<std::uint8_t>(abi::kOpUringCmd128);
}

static_assert(sizeof(abi::FlatSqe) == 64U && uarch::NoPadding<abi::FlatSqe>);
static_assert(sizeof(abi::FlatSqe128) == 128U && uarch::NoPadding<abi::FlatSqe128>);
static_assert(sizeof(abi::FlatCqe) == 16U && uarch::NoPadding<abi::FlatCqe> && uarch::GprPair<abi::FlatCqe>);
static_assert(sizeof(abi::FlatCqe32) == 32U && uarch::NoPadding<abi::FlatCqe32>);

}  // namespace liburing::ring

namespace liburing {
using ring::BuildSingleIssuerFlags;
using ring::CqRingMode;
using ring::DetectCqRingMode;
using ring::DetectRingMode;
using ring::DetectSqRingMode;
using ring::Full128_32;
using ring::Is128ByteOpcode;
using ring::kSingleIssuerSetupFlags;
using ring::MakeSingleIssuerParams;
using ring::Mixed64_128_16_32;
using ring::ModeSetupFlags;
using ring::RingGeometryDescriptor;
using ring::RingMode;
using ring::SqRingMode;
using ring::Standard64_16;
using ring::ToCqRingMode;
using ring::ToRingMode;
using ring::ToSqRingMode;
}  // namespace liburing
