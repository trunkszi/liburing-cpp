/** @file @brief Registered aligned buffers retain DMA storage until original-operation completion. */
#pragma once

#include <fcntl.h>
#include <sys/uio.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <liburing/abi/uapi_io_uring.hpp>
#include <liburing/sys/mmap_region.hpp>
#include <liburing/sys/unique_fd.hpp>
#include <liburing/sys/uring_sys.hpp>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <uarch/uarch.hpp>
#include <utility>

namespace liburing::resources {

enum class BufferState : std::uint8_t { kFree = 0, kRetired = 1, kCheckedOut = 2, kInFlight = 4, kQuarantined = 5 };

// Non-owning completion identity. The pool must outlive all tokens;
// byte access is permitted only while this generation is checked out.
class BufferHandle final {
  friend class FixedBufferPool;
  std::uint8_t* ptr_{};
  std::uint32_t capacity_{};
  std::uint32_t generation_{};
  std::uint16_t index_{};
  [[maybe_unused]] std::array<std::uint8_t, 6> reserved_{};
  BufferHandle(std::uint8_t* ptr, std::uint32_t capacity, std::uint32_t generation, std::uint16_t index) noexcept
      : ptr_(ptr), capacity_(capacity), generation_(generation), index_(index) {}

 public:
  BufferHandle() noexcept = default;
  [[nodiscard]] std::uint16_t Index() const noexcept { return index_; }
  [[nodiscard]] std::uint32_t Generation() const noexcept { return generation_; }
  [[nodiscard]] std::uint8_t* Data() const noexcept { return ptr_; }
  [[nodiscard]] std::span<std::uint8_t> Bytes() const noexcept { return {ptr_, capacity_}; }
  [[nodiscard]] std::uint32_t Capacity() const noexcept { return capacity_; }
};

static_assert(sizeof(BufferHandle) == 24 && uarch::NoPadding<BufferHandle>);

struct BufferSlotSpec {
  using Word = std::uint64_t;
  BufferState state[[= uarch::bits::Field{0, 3}]];
  std::uint32_t generation[[= uarch::bits::Field{32, 32}]];
};
using BufferSlotWord = uarch::BitWord<BufferSlotSpec>;
static_assert(sizeof(BufferSlotWord) == 8 && uarch::NoPadding<BufferSlotWord> && uarch::RegisterPassable<BufferSlotWord>);

// Single issuer, exclusive fixed-buffer registration. An owned native fd
// duplicate pins the kernel ring identity across descriptor reuse.
class FixedBufferPool final {
  struct Slot {
    BufferSlotWord word{};
    [[nodiscard]] std::uint32_t Generation() const noexcept { return word.get<^^BufferSlotSpec::generation>(); }
    [[nodiscard]] BufferState State() const noexcept { return word.get<^^BufferSlotSpec::state>(); }
    void SetState(BufferState state) noexcept { word = word.with<^^BufferSlotSpec::state>(state); }
    void AdvanceGeneration() noexcept { word = word.wrapping_add_top_field<^^BufferSlotSpec::generation>(1); }
  };
  struct Storage {
    sys::MmapRegion mapping;
    std::unique_ptr<Slot[]> slots;
    std::unique_ptr<std::uint16_t[]> free;
    std::uint8_t* base{};
    std::size_t stride{};
    std::uint32_t capacity{};
    std::uint32_t count{};
    std::uint32_t available{};
    std::uint32_t pending{};
    sys::UniqueFd dup_ring_fd;
    bool disarmed{};
    ~Storage() noexcept {
      bool failed = false;
      if (!disarmed && dup_ring_fd.valid()) {
        failed = !ToResult(sys::SysIoUringRegister(dup_ring_fd.get(), abi::kUnregisterBuffers, nullptr, 0));
      }
      if (pending != 0 || failed || disarmed) {
        // Even successful unregister leaves references held by outstanding I/O.
        union RetainedMapping {
          sys::MmapRegion mapping;
          explicit RetainedMapping(sys::MmapRegion&& value) noexcept : mapping(std::move(value)) {}
          ~RetainedMapping() noexcept {}
        } retained{std::move(mapping)};
        // Preserve the ring reference together with the DMA allocation.
        [[maybe_unused]] const auto retained_fd = dup_ring_fd.release();
      }
    }
  };
  std::unique_ptr<Storage> storage_;

  [[nodiscard]] bool Matches(const BufferHandle& buffer) const noexcept {
    return storage_ && buffer.index_ < storage_->count &&
           buffer.ptr_ == storage_->base + static_cast<std::size_t>(buffer.index_) * storage_->stride &&
           buffer.capacity_ == storage_->capacity && buffer.generation_ != 0 &&
           storage_->slots[buffer.index_].Generation() == buffer.generation_;
  }
  void Recycle(std::uint16_t index) noexcept {
    auto& slot = storage_->slots[index];
    if (slot.Generation() == std::numeric_limits<std::uint32_t>::max()) {
      slot.SetState(BufferState::kRetired);
    } else {
      slot.SetState(BufferState::kFree);
      storage_->free[storage_->available++] = index;
    }
  }

 public:
  FixedBufferPool() noexcept = default;
  FixedBufferPool(FixedBufferPool&&) noexcept = default;
  FixedBufferPool& operator=(FixedBufferPool&&) noexcept = default;
  FixedBufferPool(const FixedBufferPool&) = delete;
  FixedBufferPool& operator=(const FixedBufferPool&) = delete;

  [[nodiscard]] static Result<FixedBufferPool> CreateOffline(std::uint16_t count, std::size_t buffer_len,
                                                             std::size_t alignment = 4096) noexcept {
    if (count == 0 || buffer_len == 0 || buffer_len > UINT32_MAX || alignment < 64 || !std::has_single_bit(alignment))
      return std::unexpected(UringErrorCode::kInvalid);
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (buffer_len > limit - (alignment - 1)) return std::unexpected(UringErrorCode::kOverflow);
    const auto stride = (buffer_len + alignment - 1) & ~(alignment - 1);
    if (stride > (limit - (alignment - 1)) / count) return std::unexpected(UringErrorCode::kOverflow);
    const auto bytes = stride * count + alignment - 1;
    auto mapping = sys::MmapRegion::Map(nullptr, bytes, sys::kProtRead | sys::kProtWrite,
                                        sys::kMapPrivate | sys::kMapAnonymous | sys::kMapPopulate, -1, 0);
    if (!mapping) return std::unexpected(mapping.error());
    FixedBufferPool result;
    result.storage_.reset(new (std::nothrow) Storage{});
    if (!result.storage_) return std::unexpected(UringErrorCode::kNoMemory);
    auto& s = *result.storage_;
    s.slots.reset(new (std::nothrow) Slot[count]{});
    s.free.reset(new (std::nothrow) std::uint16_t[count]);
    if (!s.slots || !s.free) return std::unexpected(UringErrorCode::kNoMemory);
    const auto address = reinterpret_cast<std::uintptr_t>(mapping->bytes().data());
    s.base = reinterpret_cast<std::uint8_t*>((address + alignment - 1) & ~(alignment - 1));
    s.mapping = std::move(*mapping);
    s.stride = stride;
    s.capacity = static_cast<std::uint32_t>(buffer_len);
    s.count = count;
    s.available = count;
    for (std::uint32_t i = 0; i < count; ++i) s.free[i] = static_cast<std::uint16_t>(count - i - 1);
    return result;
  }

  // Registers the whole ring's buffer table; ResourceRegistry buffer subranges
  // and other fixed-buffer table owners cannot coexist on this ring.
  template <class Ring>
  [[nodiscard]] static Result<FixedBufferPool> Create(Ring& ring, std::uint16_t count, std::size_t buffer_len,
                                                      std::size_t alignment = 4096) noexcept {
    auto result = CreateOffline(count, buffer_len, alignment);
    if (!result) return std::unexpected(result.error());
    std::unique_ptr<iovec[]> vectors{new (std::nothrow) iovec[count]};
    if (!vectors) return std::unexpected(UringErrorCode::kNoMemory);
    auto& s = *result->storage_;
    for (std::uint32_t i = 0; i < count; ++i) vectors[i] = {s.base + i * s.stride, buffer_len};
    sys::UniqueFd dup_ring_fd;
    if constexpr (requires { ring.Fd(); }) {
      const auto ring_fd = ring.Fd();
      if (ring_fd < 0) return std::unexpected(UringErrorCode::kNotSupported);
      auto duplicated = ToResult(sys::Syscall6(sys::kSysFcntl, static_cast<std::uint64_t>(ring_fd), F_DUPFD_CLOEXEC, 0));
      if (!duplicated) return std::unexpected(duplicated.error());
      auto owned = sys::UniqueFd::Adopt(static_cast<std::int32_t>(*duplicated));
      if (!owned) return std::unexpected(owned.error());
      dup_ring_fd = std::move(*owned);
    }
    auto registered = ring.Register(abi::kRegisterBuffers, vectors.get(), count);
    if (!registered) return std::unexpected(registered.error());
    s.dup_ring_fd = std::move(dup_ring_fd);
    return result;
  }

  [[nodiscard]] Result<BufferHandle> Checkout() noexcept {
    if (storage_ && storage_->disarmed) return std::unexpected(UringErrorCode::kBadRing);
    if (!storage_ || storage_->available == 0) return std::unexpected(UringErrorCode::kNoSpace);
    const auto index = storage_->free[--storage_->available];
    auto& slot = storage_->slots[index];
    slot.AdvanceGeneration();
    slot.SetState(BufferState::kCheckedOut);
    return BufferHandle{storage_->base + index * storage_->stride, storage_->capacity, slot.Generation(), index};
  }
  [[nodiscard]] Result<void> ReleaseCheckout(const BufferHandle& buffer) noexcept {
    if (!Matches(buffer) || storage_->slots[buffer.index_].State() != BufferState::kCheckedOut)
      return std::unexpected(UringErrorCode::kInvalid);
    Recycle(buffer.index_);
    return {};
  }
  [[nodiscard]] Result<void> MarkInFlight(const BufferHandle& buffer) noexcept {
    if (!Matches(buffer) || storage_->slots[buffer.index_].State() != BufferState::kCheckedOut)
      return std::unexpected(UringErrorCode::kInvalid);
    storage_->slots[buffer.index_].SetState(BufferState::kInFlight);
    ++storage_->pending;
    return {};
  }
  [[nodiscard]] Result<void> Quarantine(const BufferHandle& buffer) noexcept {
    if (!Matches(buffer)) return std::unexpected(UringErrorCode::kInvalid);
    const auto state = storage_->slots[buffer.index_].State();
    if (state != BufferState::kInFlight && state != BufferState::kQuarantined) return std::unexpected(UringErrorCode::kInvalid);
    storage_->slots[buffer.index_].SetState(BufferState::kQuarantined);
    return {};
  }
  // Caller must observe the original I/O's terminal CQE (including required
  // zero-copy notifications); a cancellation control CQE is insufficient.
  [[nodiscard]] Result<void> CompleteInFlight(const BufferHandle& buffer) noexcept {
    if (!Matches(buffer)) return std::unexpected(UringErrorCode::kInvalid);
    const auto state = storage_->slots[buffer.index_].State();
    if (state != BufferState::kInFlight && state != BufferState::kQuarantined) return std::unexpected(UringErrorCode::kInvalid);
    --storage_->pending;
    Recycle(buffer.index_);
    return {};
  }
  [[nodiscard]] bool HasPendingDma() const noexcept { return storage_ && storage_->pending != 0; }
  void DisarmUnregisterOnStall() noexcept {
    if (HasPendingDma()) storage_->disarmed = true;
  }
  void ReleaseOwnershipOnStall() noexcept { DisarmUnregisterOnStall(); }
  [[nodiscard]] std::uint32_t Capacity() const noexcept { return storage_ ? storage_->count : 0; }
  [[nodiscard]] std::uint32_t Available() const noexcept { return storage_ ? storage_->available : 0; }
};
}  // namespace liburing::resources
