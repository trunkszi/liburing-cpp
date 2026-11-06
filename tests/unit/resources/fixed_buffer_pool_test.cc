#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <liburing/resources/fixed_buffer_pool.hpp>
#include <liburing/ring/ring.hpp>
#include <optional>

using namespace liburing;
using resources::FixedBufferPool;
#define REQUIRE(condition)                                         \
  do {                                                             \
    if (!(condition)) {                                            \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
      return __LINE__;                                             \
    }                                                              \
  } while (false)

struct RecordingRing {
  ScriptedRing ring{8, 16};
  void* base{};
  std::uint32_t registrations{};
  std::uint32_t unregistrations{};
  bool reject_registration{};
  Result<std::int64_t> Register(std::uint32_t opcode, const void* arg, std::uint32_t count) noexcept {
    if (opcode == abi::kRegisterBuffers) {
      ++registrations;
      base = static_cast<const iovec*>(arg)[0].iov_base;
      if (reject_registration) return std::unexpected(UringErrorCode::kNoMemory);
    } else if (opcode == abi::kUnregisterBuffers) {
      ++unregistrations;
    }
    return ring.Register(opcode, arg, count);
  }
};

int Lifecycle() {
  ScriptedRing ring{8, 16};
  auto pool = FixedBufferPool::Create(ring, 1, 4096);
  REQUIRE(pool && pool->Capacity() == 1 && pool->Available() == 1);
  REQUIRE(ring.LastRegisterOpcode() == abi::kRegisterBuffers);
  auto buffer = pool->Checkout();
  REQUIRE(buffer && buffer->Capacity() == 4096 && buffer->Index() == 0 && buffer->Generation() == 1);
  REQUIRE(!pool->CompleteInFlight(*buffer) && !pool->Quarantine(*buffer));
  REQUIRE(pool->MarkInFlight(*buffer));
  REQUIRE(!pool->MarkInFlight(*buffer));
  REQUIRE(pool->Quarantine(*buffer) && pool->Quarantine(*buffer));
  REQUIRE(pool->HasPendingDma() && !pool->Checkout());
  REQUIRE(!pool->ReleaseCheckout(*buffer));
  REQUIRE(pool->CompleteInFlight(*buffer));
  REQUIRE(!pool->HasPendingDma() && !pool->CompleteInFlight(*buffer));
  auto next = pool->Checkout();
  REQUIRE(next && next->Generation() == 2);
  REQUIRE(!pool->ReleaseCheckout(*buffer) && !pool->MarkInFlight(*buffer));
  REQUIRE(pool->MarkInFlight(*next));
  REQUIRE(!pool->CompleteInFlight(*buffer) && !pool->Quarantine(*buffer));
  REQUIRE(pool->CompleteInFlight(*next));
  auto third = pool->Checkout();
  REQUIRE(third && pool->ReleaseCheckout(*third) && !pool->ReleaseCheckout(*third));
  auto other = FixedBufferPool::CreateOffline(1, 4096);
  REQUIRE(other && !other->ReleaseCheckout(*third));
  return 0;
}

int Allocation() {
  for (std::size_t alignment : {4096U, 8192U}) {
    auto pool = FixedBufferPool::CreateOffline(3, 4100, alignment);
    REQUIRE(pool);
    std::array<resources::BufferHandle, 3> buffers{};
    for (std::size_t i = 0; i < buffers.size(); ++i) {
      auto buffer = pool->Checkout();
      REQUIRE(buffer && buffer->Index() == i && buffer->Bytes().size() == 4100);
      REQUIRE(reinterpret_cast<std::uintptr_t>(buffer->Data()) % alignment == 0);
      REQUIRE(buffer->Bytes().front() == 0 && buffer->Bytes().back() == 0);
      buffers[i] = *buffer;
      buffer->Bytes().front() = static_cast<std::uint8_t>(i + 1);
    }
    auto empty = pool->Checkout();
    REQUIRE(!empty && empty.error() == UringErrorCode::kNoSpace);
    REQUIRE(buffers[1].Data() - buffers[0].Data() == 8192);
    REQUIRE(pool->ReleaseCheckout(buffers[1]));
    auto recycled = pool->Checkout();
    REQUIRE(recycled && recycled->Index() == 1 && recycled->Data()[0] == 2);
    REQUIRE(pool->ReleaseCheckout(*recycled));
    REQUIRE(pool->ReleaseCheckout(buffers[0]) && pool->ReleaseCheckout(buffers[2]));
  }
  for (std::size_t alignment : {0U, 32U, 63U, 65U, 4095U}) REQUIRE(!FixedBufferPool::CreateOffline(1, 4096, alignment));
  REQUIRE(!FixedBufferPool::CreateOffline(0, 4096));
  REQUIRE(!FixedBufferPool::CreateOffline(1, 0));
  REQUIRE(!FixedBufferPool::CreateOffline(1, static_cast<std::size_t>(UINT32_MAX) + 1));
  auto small = FixedBufferPool::CreateOffline(2, 65, 64);
  REQUIRE(small);
  auto first = small->Checkout();
  auto second = small->Checkout();
  REQUIRE(first && second && second->Data() - first->Data() == 128);
  return 0;
}

int Teardown() {
  RecordingRing ring;
  {
    auto pool = FixedBufferPool::Create(ring, 1, 4096);
    REQUIRE(pool);
    auto moved = std::move(*pool);
    REQUIRE(!pool->Checkout() && moved.Checkout());
  }
  REQUIRE(ring.registrations == 1 && ring.unregistrations == 0);
  ring.reject_registration = true;
  REQUIRE(!FixedBufferPool::Create(ring, 1, 4096));
  REQUIRE(ring.unregistrations == 0);
  ring.reject_registration = false;
  for (int scenario = 0; scenario < 3; ++scenario) {
    const auto before = ring.unregistrations;
    {
      auto pool = FixedBufferPool::Create(ring, 1, 4096);
      REQUIRE(pool);
      if (scenario < 3) {
        auto buffer = pool->Checkout();
        REQUIRE(buffer && pool->MarkInFlight(*buffer));
        if (scenario == 1) REQUIRE(pool->Quarantine(*buffer));
        if (scenario == 2) pool->ReleaseOwnershipOnStall();
      } else {
        pool->ReleaseOwnershipOnStall();
      }
    }
    REQUIRE(ring.unregistrations == before);
    unsigned char resident{};
    REQUIRE(::mincore(ring.base, 4096, &resident) == 0);
    static_cast<std::uint8_t*>(ring.base)[0] = 42;
    // No I/O was submitted; this test owns the intentionally retained mapping.
    REQUIRE(::munmap(ring.base, 8191) == 0);
  }
  {
    auto pool = FixedBufferPool::Create(ring, 1, 4096);
    REQUIRE(pool);
  }
  unsigned char resident{};
  REQUIRE(::mincore(ring.base, 4096, &resident) == -1 && errno == ENOMEM);
  return 0;
}

int KernelRegistration() {
  auto ring = KernelRing::Create(8);
  REQUIRE(ring);
  {
    auto pool = FixedBufferPool::Create(*ring, 2, 4096);
    REQUIRE(pool);
    auto buffer = pool->Checkout();
    REQUIRE(buffer && pool->ReleaseCheckout(*buffer));
    auto conflict = FixedBufferPool::Create(*ring, 1, 4096);
    REQUIRE(!conflict && conflict.error() == UringErrorCode::kBusy);
    const iovec vector{buffer->Data(), buffer->Capacity()};
    auto still_registered = ring->Register(abi::kRegisterBuffers, &vector, 1);
    REQUIRE(!still_registered && still_registered.error() == UringErrorCode::kBusy);
  }
  auto again = FixedBufferPool::Create(*ring, 1, 8192, 8192);
  REQUIRE(again);
  return 0;
}

int MovedRingLifetime() {
  std::optional<FixedBufferPool> pool;
  std::optional<KernelRing> owner;
  {
    auto local = KernelRing::Create(8);
    REQUIRE(local);
    auto created = FixedBufferPool::Create(*local, 1, 4096);
    REQUIRE(created);
    pool.emplace(std::move(*created));
    owner.emplace(std::move(*local));
  }
  pool.reset();
  auto replacement = FixedBufferPool::Create(*owner, 1, 4096);
  REQUIRE(replacement);
  return 0;
}

int ClosedRingLifetime() {
  std::optional<FixedBufferPool> pool;
  void* base = nullptr;
  {
    auto ring = KernelRing::Create(8);
    REQUIRE(ring);
    auto created = FixedBufferPool::Create(*ring, 1, 4096);
    REQUIRE(created);
    pool.emplace(std::move(*created));
    auto buffer = pool->Checkout();
    REQUIRE(buffer);
    base = buffer->Data();
    REQUIRE(pool->ReleaseCheckout(*buffer));
  }
  unsigned char resident{};
  REQUIRE(::mincore(base, 4096, &resident) == 0);
  pool.reset();
  REQUIRE(::mincore(base, 4096, &resident) == -1 && errno == ENOMEM);
  return 0;
}

struct RegisteredOnlyTarget {
  int native_fd = -1;
  unsigned registrations{};
  [[nodiscard]] int Fd() const noexcept { return native_fd; }
  Result<std::int64_t> Register(std::uint32_t, const void*, std::uint32_t) noexcept {
    ++registrations;
    return 0;
  }
};
int RejectRegisteredOnly() {
  RegisteredOnlyTarget target;
  auto pool = FixedBufferPool::Create(target, 1, 4096);
  REQUIRE(!pool && pool.error() == UringErrorCode::kNotSupported && target.registrations == 0);
  return 0;
}

int RingFdReuse() {
  std::optional<FixedBufferPool> pool;
  int original_fd = -1;
  {
    auto ring = KernelRing::Create(8);
    REQUIRE(ring);
    original_fd = ring->Fd();
    auto created = FixedBufferPool::Create(*ring, 1, 4096);
    REQUIRE(created);
    pool.emplace(std::move(*created));
  }
  auto replacement = KernelRing::Create(8);
  REQUIRE(replacement && replacement->Fd() == original_fd);
  alignas(4096) static std::array<std::byte, 4096> page{};
  const iovec vector{page.data(), page.size()};
  REQUIRE(replacement->Register(abi::kRegisterBuffers, &vector, 1));
  pool.reset();
  auto duplicate_registration = replacement->Register(abi::kRegisterBuffers, &vector, 1);
  REQUIRE(!duplicate_registration && duplicate_registration.error() == UringErrorCode::kBusy);
  REQUIRE(replacement->Register(abi::kUnregisterBuffers, nullptr, 0));
  return 0;
}

std::array<bool, 256> OpenDescriptors() {
  std::array<bool, 256> descriptors{};
  for (std::size_t i = 0; i < descriptors.size(); ++i) descriptors[i] = ::fcntl(static_cast<int>(i), F_GETFD) >= 0;
  return descriptors;
}
int RetainedRingReference() {
  for (bool disarm : {false, true}) {
    auto ring = KernelRing::Create(8);
    REQUIRE(ring);
    const auto before = OpenDescriptors();
    int retained_fd = -1;
    void* base = nullptr;
    {
      auto pool = FixedBufferPool::Create(*ring, 1, 4096);
      REQUIRE(pool);
      const auto after = OpenDescriptors();
      for (std::size_t i = 0; i < before.size(); ++i) {
        if (!before[i] && after[i]) {
          REQUIRE(retained_fd == -1);
          retained_fd = static_cast<int>(i);
        }
      }
      REQUIRE(retained_fd >= 0 && (::fcntl(retained_fd, F_GETFD) & FD_CLOEXEC) != 0);
      auto buffer = pool->Checkout();
      REQUIRE(buffer && pool->MarkInFlight(*buffer));
      base = buffer->Data();
      if (disarm) pool->DisarmUnregisterOnStall();
    }
    REQUIRE(::fcntl(retained_fd, F_GETFD) >= 0);
    unsigned char resident{};
    REQUIRE(::mincore(base, 4096, &resident) == 0);
    // No request was submitted; this test can release both retained resources.
    if (disarm) REQUIRE(ToResult(sys::SysIoUringRegister(retained_fd, abi::kUnregisterBuffers, nullptr, 0)));
    REQUIRE(::close(retained_fd) == 0);
    REQUIRE(::munmap(base, 8191) == 0);
  }
  return 0;
}

int main() {
  if (auto result = Lifecycle()) return result;
  if (auto result = Allocation()) return result;
  if (auto result = Teardown()) return result;
  if (auto result = KernelRegistration()) return result;
  if (auto result = MovedRingLifetime()) return result;
  if (auto result = RingFdReuse()) return result;
  if (auto result = ClosedRingLifetime()) return result;
  if (auto result = RejectRegisteredOnly()) return result;
  return RetainedRingReference();
}
