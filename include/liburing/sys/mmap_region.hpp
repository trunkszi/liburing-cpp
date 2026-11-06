#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <liburing/sys/unique_fd.hpp>
#include <span>
#include <utility>
namespace liburing::sys {
inline constexpr std::int32_t kProtRead = 0x1;
inline constexpr std::int32_t kProtWrite = 0x2;
inline constexpr std::int32_t kMapShared = 0x1;
inline constexpr std::int32_t kMapPrivate = 0x2;
inline constexpr std::int32_t kMapAnonymous = 0x20;
inline constexpr std::int32_t kMapPopulate = 0x8000;

class MmapRegion final {
  void* address_{};
  std::size_t size_{};
  bool owned_{};
  MmapRegion(void* address, std::size_t size, bool owned) noexcept : address_(address), size_(size), owned_(owned) {}

 public:
  MmapRegion() noexcept = default;
  MmapRegion(const MmapRegion&) = delete ("mapping ownership is unique");
  MmapRegion& operator=(const MmapRegion&) = delete ("mapping ownership is unique");
  MmapRegion(MmapRegion&& other) noexcept
      : address_(std::exchange(other.address_, nullptr)),
        size_(std::exchange(other.size_, 0)),
        owned_(std::exchange(other.owned_, false)) {}
  MmapRegion& operator=(MmapRegion&& other) noexcept {
    if (this != &other) {
      if (!reset()) Panic("owned mapping cleanup failed");
      address_ = std::exchange(other.address_, nullptr);
      size_ = std::exchange(other.size_, 0);
      owned_ = std::exchange(other.owned_, false);
    }
    return *this;
  }
  ~MmapRegion() noexcept {
    if (!reset()) Panic("owned mapping cleanup failed");
  }
  [[nodiscard]] static Result<MmapRegion> Map(void* hint, std::size_t length, std::int32_t protection, std::int32_t flags,
                                              std::int32_t fd, std::uint64_t offset) noexcept {
    if (length == 0) return std::unexpected(UringErrorCode::kInvalid);
    const auto result = FromRawSyscall(Syscall6(kSysMmap, Address(hint), length, static_cast<std::uint64_t>(protection),
                                                static_cast<std::uint64_t>(flags),
                                                static_cast<std::uint64_t>(static_cast<std::int64_t>(fd)), offset));
    if (!result) return std::unexpected(result.error().code());
    return MmapRegion{reinterpret_cast<void*>(static_cast<std::uintptr_t>(*result)), length, true};
  }
  [[nodiscard]] static Result<MmapRegion> Borrow(std::span<std::byte> memory) noexcept {
    if (memory.empty()) return std::unexpected(UringErrorCode::kInvalid);
    return MmapRegion{memory.data(), memory.size(), false};
  }
  [[nodiscard]] bool owns_memory() const noexcept { return owned_; }
  [[nodiscard]] std::span<std::byte> bytes() const noexcept { return {static_cast<std::byte*>(address_), size_}; }
  [[nodiscard]] Result<void> reset() noexcept {
    if (owned_ && size_ != 0) {
      const auto result = ToResult(Syscall6(kSysMunmap, Address(address_), size_));
      if (!result) return std::unexpected(result.error());
    }
    address_ = nullptr;
    size_ = 0;
    owned_ = false;
    return {};
  }
};

struct NoMmapMemory {
  std::span<std::byte> rings;
  std::span<std::byte> sqes;
};
struct RingMappingSizes {
  std::size_t sq;
  std::size_t cq;
  std::size_t sqes;
};
class RingMappings final {
  MmapRegion sq_;
  MmapRegion cq_;
  MmapRegion sqes_;

  template <class Mapper>
  [[nodiscard]] static Result<MmapRegion> MapPages(std::size_t size, std::int32_t fd, std::uint64_t offset) noexcept {
    auto mapping = Mapper::Map(nullptr, size, kProtRead | kProtWrite, kMapShared | kMapPopulate, fd, offset);
    if (!mapping) {
      const auto error = mapping.error();
      // Prefault restrictions must not make otherwise valid ring mappings unavailable.
      if (error == UringErrorCode::kNoMemory || error == UringErrorCode::kAgain || error == UringErrorCode::kPermissionDenied ||
          error == UringErrorCode::kAccessDenied)
        return Mapper::Map(nullptr, size, kProtRead | kProtWrite, kMapShared, fd, offset);
    }
    return mapping;
  }

 public:
  RingMappings() noexcept = default;
  RingMappings(RingMappings&&) noexcept = default;
  RingMappings& operator=(RingMappings&&) noexcept = default;
  [[nodiscard]] static Result<RingMappingSizes> Sizes(const abi::IoUringParams& params) noexcept {
    const auto n = params.sq_entries;
    const auto m = params.cq_entries;
    if (!n || !m || (n & (n - 1U)) || (m & (m - 1U))) return std::unexpected(UringErrorCode::kInvalid);
    if ((params.flags & (abi::kSetupSqe128 | abi::kSetupSqeMixed)) == (abi::kSetupSqe128 | abi::kSetupSqeMixed))
      return std::unexpected(UringErrorCode::kInvalid);
    if ((params.flags & (abi::kSetupCqe32 | abi::kSetupCqeMixed)) == (abi::kSetupCqe32 | abi::kSetupCqeMixed))
      return std::unexpected(UringErrorCode::kInvalid);
    const auto sqe_stride = (params.flags & abi::kSetupSqe128) ? 128U : 64U;
    const auto cqe_stride = (params.flags & abi::kSetupCqe32) ? 32U : 16U;
    std::size_t sq = 0;
    std::size_t cq = 0;
    for (auto offset : {params.sq_off.head, params.sq_off.tail, params.sq_off.ring_mask, params.sq_off.ring_entries,
                        params.sq_off.flags, params.sq_off.dropped})
      sq = std::max(sq, static_cast<std::size_t>(offset) + 4);
    if (!(params.flags & abi::kSetupNoSqArray))
      sq = std::max(sq, static_cast<std::size_t>(params.sq_off.array) + static_cast<std::size_t>(n) * 4);
    for (auto offset : {params.cq_off.head, params.cq_off.tail, params.cq_off.ring_mask, params.cq_off.ring_entries,
                        params.cq_off.flags, params.cq_off.overflow})
      cq = std::max(cq, static_cast<std::size_t>(offset) + 4);
    cq = std::max(cq, static_cast<std::size_t>(params.cq_off.cqes) + static_cast<std::size_t>(m) * cqe_stride);
    return RingMappingSizes{sq, cq, static_cast<std::size_t>(n) * sqe_stride};
  }
  template <class Mapper = MmapRegion>
  [[nodiscard]] static Result<RingMappings> Map(std::int32_t fd, const abi::IoUringParams& params,
                                                const NoMmapMemory* memory = nullptr) noexcept {
    auto sizes = Sizes(params);
    if (!sizes) return std::unexpected(sizes.error());
    RingMappings result;
    if (params.flags & abi::kSetupNoMmap) {
      const auto ring_size = std::max(sizes->sq, sizes->cq);
      if (!memory || memory->rings.size() < ring_size || memory->sqes.size() < sizes->sqes ||
          Address(memory->rings.data()) != params.cq_off.user_addr || Address(memory->sqes.data()) != params.sq_off.user_addr ||
          (Address(memory->rings.data()) & 4095U) || (Address(memory->sqes.data()) & 4095U))
        return std::unexpected(UringErrorCode::kInvalid);
      auto rings = MmapRegion::Borrow(memory->rings);
      auto sqes = MmapRegion::Borrow(memory->sqes);
      if (!rings) return std::unexpected(rings.error());
      if (!sqes) return std::unexpected(sqes.error());
      result.sq_ = std::move(*rings);
      result.sqes_ = std::move(*sqes);
      return result;
    }
    const bool shared = (params.features & abi::kFeatSingleMmap) != 0;
    auto sq = MapPages<Mapper>(shared ? std::max(sizes->sq, sizes->cq) : sizes->sq, fd, abi::kOffSqRing);
    if (!sq) return std::unexpected(sq.error());
    result.sq_ = std::move(*sq);
    if (!shared) {
      auto cq = MapPages<Mapper>(sizes->cq, fd, abi::kOffCqRing);
      if (!cq) return std::unexpected(cq.error());
      result.cq_ = std::move(*cq);
    }
    auto sqes = MapPages<Mapper>(sizes->sqes, fd, abi::kOffSqes);
    if (!sqes) return std::unexpected(sqes.error());
    result.sqes_ = std::move(*sqes);
    return result;
  }
  [[nodiscard]] std::span<std::byte> sq() const noexcept { return sq_.bytes(); }
  [[nodiscard]] std::span<std::byte> cq() const noexcept { return cq_.bytes().empty() ? sq_.bytes() : cq_.bytes(); }
  [[nodiscard]] std::span<std::byte> sqes() const noexcept { return sqes_.bytes(); }
};
}  // namespace liburing::sys
