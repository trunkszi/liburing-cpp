#include <cstddef>
#include <cstdint>
#include <liburing/sys/mmap_region.hpp>
#include <liburing/sys/unique_fd.hpp>
#include <span>
#include <utility>
using namespace liburing;
int main() {
  const char name[] = "liburing-raii";
  const auto raw = sys::Syscall6(liburing::sys::kSysMemfdCreate, sys::Address(name), 1);
  if (raw < 0) return 1;
  const auto fd = static_cast<std::int32_t>(raw);
  {
    auto owner = sys::UniqueFd::Adopt(fd);
    if (!owner) return 2;
    auto moved = std::move(*owner);
    if (owner->valid() || moved.get() != fd) return 3;
  }
  if (sys::Syscall6(liburing::sys::kSysFcntl, static_cast<std::uint64_t>(fd), 1) != -9) return 4;
  std::byte* address = nullptr;
  {
    auto region =
        sys::MmapRegion::Map(nullptr, 4096, sys::kProtRead | sys::kProtWrite, sys::kMapPrivate | sys::kMapAnonymous, -1, 0);
    if (!region) return 5;
    address = region->bytes().data();
    region->bytes()[4095] = std::byte{0x7f};
    auto moved = std::move(*region);
    if (!region->bytes().empty() || moved.bytes()[4095] != std::byte{0x7f}) return 6;
    auto borrow = sys::MmapRegion::Borrow(moved.bytes());
    if (!borrow || borrow->owns_memory()) return 7;
  }
  unsigned char resident{};
  if (sys::Syscall6(liburing::sys::kSysMincore, sys::Address(address), 4096, sys::Address(&resident)) != -12) return 8;
  return 0;
}
