#pragma once
#include <array>
#include <cstdint>
#include <liburing/abi/proofs.hpp>
#include <liburing/caps/required_caps.hpp>
#include <liburing/sys/futex_sys.hpp>
#include <liburing/sys/unique_fd.hpp>
#include <limits>
#include <string_view>
namespace liburing::caps {
struct KernelVersion {
  std::uint32_t major;
  std::uint32_t minor;
  std::uint32_t patch;
};
[[nodiscard]] constexpr Result<KernelVersion> ParseKernelVersion(std::string_view release) noexcept {
  std::uint32_t parts[3]{};
  std::size_t position = 0;
  for (std::size_t field = 0; field < 3; ++field) {
    const auto start = position;
    while (position < release.size() && release[position] >= '0' && release[position] <= '9') {
      const auto digit = static_cast<std::uint32_t>(release[position] - '0');
      if (parts[field] > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U)
        return std::unexpected(UringErrorCode::kInvalid);
      parts[field] = parts[field] * 10U + digit;
      ++position;
    }
    if (position == start) return std::unexpected(UringErrorCode::kInvalid);
    if (field == 0) {
      if (position == release.size() || release[position] != '.') return std::unexpected(UringErrorCode::kInvalid);
      ++position;
    } else if (field == 1) {
      if (position == release.size() || release[position] == '-' || release[position] == '+') break;
      if (release[position] != '.') return std::unexpected(UringErrorCode::kInvalid);
      ++position;
    } else if (position != release.size() && release[position] != '-' && release[position] != '+') {
      return std::unexpected(UringErrorCode::kInvalid);
    }
  }
  return KernelVersion{parts[0], parts[1], parts[2]};
}
[[nodiscard]] constexpr Result<void> CheckKernelRelease(std::string_view release, std::uint32_t major = 6,
                                                        std::uint32_t minor = 16) noexcept {
  auto version = ParseKernelVersion(release);
  if (!version) return std::unexpected(version.error());
  if (version->major < major || (version->major == major && version->minor < minor))
    return std::unexpected(UringErrorCode::kUnsupportedKernelVersion);
  return {};
}
[[nodiscard]] inline Result<void> VerifyKernelVersion(std::uint32_t major = 6, std::uint32_t minor = 16) noexcept {
  struct Utsname {
    char name[65];
    char node[65];
    char release[65];
    char version[65];
    char machine[65];
    char domain[65];
  };
  Utsname name{};
  auto result = ToResult(sys::Syscall6(sys::kSysUname, sys::Address(&name)));
  if (!result) return std::unexpected(result.error());
  std::size_t length = 0;
  while (length < sizeof(name.release) && name.release[length] != '\0') ++length;
  if (length == sizeof(name.release)) return std::unexpected(UringErrorCode::kProtocolViolation);
  return CheckKernelRelease({name.release, length}, major, minor);
}
struct NativeProbeBackend {
  static Result<void> VerifyVersion() noexcept { return VerifyKernelVersion(); }
  static std::int64_t Setup(std::uint32_t entries, abi::IoUringParams* params) noexcept {
    return sys::SysIoUringSetup(entries, params);
  }
  static std::int64_t Register(std::int32_t fd, std::uint32_t opcode, void* data, std::uint32_t count) noexcept {
    return sys::SysIoUringRegister(fd, opcode, data, count);
  }
  static std::int64_t Wake(void* word, std::uint32_t flags) noexcept { return sys::SysFutexWake(word, 0xffffffffU, 0, flags); }
};
class KernelCaps final {
  std::array<uarch::Bitmap64, 2> opcodes_{};
  std::uint32_t futex_{};

 public:
  abi::IoUringParams params{};
  std::uint64_t setup_support{};
  std::uint32_t register_opcode_count{};
  bool query_available{};
  [[nodiscard]] constexpr bool SupportsOpcode(std::uint32_t opcode) const noexcept {
    return opcode < abi::kOpLast &&
           opcodes_[uarch::PowerOfTwo<6>::div(opcode)].test(static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(opcode)));
  }
  [[nodiscard]] constexpr bool SupportsFutex(std::uint32_t required) const noexcept { return (futex_ & required) == required; }
  [[nodiscard]] constexpr KernelTier tier() const noexcept {
    const auto mixed = abi::kSetupSqeMixed | abi::kSetupCqeMixed;
    return query_available && (setup_support & mixed) == mixed && SupportsOpcode(abi::kOpUringCmd128)
               ? KernelTier::kExtendedMixed
               : KernelTier::kBaseline616;
  }
  template <class Backend = NativeProbeBackend>
  [[nodiscard]] static Result<KernelCaps> Probe(const RequiredCaps& required) noexcept {
    auto version = Backend::VerifyVersion();
    if (!version) return std::unexpected(version.error());
    if ((required.required_futex & ~(kFutexU32 | kFutexNuma | kFutexMpol)) ||
        ((required.required_futex & kFutexMpol) && !(required.required_futex & kFutexNuma)))
      return std::unexpected(UringErrorCode::kInvalid);
    if ((required.setup_flags & abi::kSetupRegisteredFdOnly) && !(required.setup_flags & abi::kSetupNoMmap))
      return std::unexpected(UringErrorCode::kInvalid);
    // NO_MMAP and registered-only setup need caller backing; query proves these flags without changing ownership.
    constexpr auto query_only = abi::kSetupNoMmap | abi::kSetupRegisteredFdOnly;
    KernelCaps result;
    result.params.flags = required.setup_flags & ~query_only;
    auto setup = ToResult(Backend::Setup(2, &result.params));
    if (!setup) return std::unexpected(setup.error());
    if (*setup > std::numeric_limits<std::int32_t>::max()) return std::unexpected(UringErrorCode::kProtocolViolation);
    auto fd = sys::UniqueFd::Adopt(static_cast<std::int32_t>(*setup));
    if (!fd) return std::unexpected(fd.error());
    if ((result.params.features & required.feature_flags) != required.feature_flags)
      return std::unexpected(UringErrorCode::kNotSupported);
    struct ProbeStorage {
      abi::IoUringProbe header;
      abi::IoUringProbeOp ops[abi::kOpLast];
    } probe{};
    static_assert(uarch::NoPadding<ProbeStorage>);
    auto registered = ToResult(Backend::Register(fd->get(), abi::kRegisterProbe, &probe, abi::kOpLast));
    if (!registered) return std::unexpected(registered.error());
    if (*registered != 0 || probe.header.ops_len > abi::kOpLast) return std::unexpected(UringErrorCode::kProtocolViolation);
    for (std::uint32_t i = 0; i < probe.header.ops_len; ++i) {
      const auto opcode = probe.ops[i].op;
      if (opcode >= abi::kOpLast) return std::unexpected(UringErrorCode::kProtocolViolation);
      if (probe.ops[i].flags & abi::kIoUringOpSupported) {
        const auto word = uarch::PowerOfTwo<6>::div(opcode);
        result.opcodes_[word] = result.opcodes_[word].set(static_cast<std::uint32_t>(uarch::PowerOfTwo<6>::rem(opcode)));
      }
    }
    for (std::size_t i = 0; i < required.opcodes.size(); ++i)
      if (!(required.opcodes[i] - result.opcodes_[i]).is_empty()) return std::unexpected(UringErrorCode::kNotSupported);
    abi::IoUringQueryOpcode query{};
    abi::IoUringQueryHdr header{.query_data = sys::Address(&query), .size = sizeof(query)};
    auto queried = ToResult(Backend::Register(fd->get(), abi::kRegisterQuery, &header, 0));
    if (queried && header.result > 0) return std::unexpected(UringErrorCode::kProtocolViolation);
    if (queried && header.result == 0) {
      if (*queried != 0 || header.size < sizeof(query)) return std::unexpected(UringErrorCode::kProtocolViolation);
      result.query_available = true;
      result.setup_support = query.ring_setup_flags;
      result.register_opcode_count = query.nr_register_opcodes;
      if ((query.ring_setup_flags & required.setup_flags) != required.setup_flags)
        return std::unexpected(UringErrorCode::kNotSupported);
    } else {
      const auto error = queried ? ToResult(header.result).error() : queried.error();
      if (error != UringErrorCode::kInvalid && error != UringErrorCode::kNotSupported &&
          error != UringErrorCode::kNotImplemented)
        return std::unexpected(error);
      if (required.require_query || (required.setup_flags & query_only)) return std::unexpected(UringErrorCode::kNotSupported);
      result.setup_support = result.params.flags;
    }
    uarch::CacheAligned<std::uint32_t> word{};
    auto normal = ToResult(Backend::Wake(word.data(), abi::kFutex2SizeU32 | abi::kFutex2Private));
    if (normal)
      result.futex_ |= kFutexU32;
    else if (required.required_futex & kFutexU32)
      return std::unexpected(normal.error());
    struct NumaWord {
      std::uint32_t value;
      std::uint32_t node;
    };
    static_assert(uarch::NoPadding<NumaWord> && uarch::RegisterPassable<NumaWord>);
    uarch::CacheAligned<NumaWord> numa{{0, abi::kFutexNoNode}};
    const auto flags = abi::kFutex2SizeU32 | abi::kFutex2Private | abi::kFutex2Numa;
    auto on_node = ToResult(Backend::Wake(numa.data(), flags));
    if (on_node)
      result.futex_ |= kFutexNuma;
    else if (required.required_futex & kFutexNuma)
      return std::unexpected(on_node.error());
    numa->node = abi::kFutexNoNode;
    auto mpol = ToResult(Backend::Wake(numa.data(), flags | abi::kFutex2Mpol));
    if (mpol)
      result.futex_ |= kFutexMpol;
    else if (required.required_futex & kFutexMpol)
      return std::unexpected(mpol.error());
    return result;
  }
};
}  // namespace liburing::caps
