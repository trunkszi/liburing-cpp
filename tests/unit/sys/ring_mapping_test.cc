#include <cstdio>
#include <cstring>
#include <liburing/sys/mmap_region.hpp>
#include <utility>
using namespace liburing;
using namespace liburing::sys;
int RingMaps() {
  auto* file = std::fopen("/proc/self/maps", "r");
  if (!file) return -1;
  char line[1024];
  int count = 0;
  while (std::fgets(line, sizeof(line), file))
    if (std::strstr(line, "io_uring")) ++count;
  std::fclose(file);
  return count;
}
struct FailSecond {
  inline static int calls = 0;
  static Result<MmapRegion> Map(void* hint, std::size_t size, std::int32_t prot, std::int32_t flags, std::int32_t fd,
                                std::uint64_t offset) noexcept {
    if (++calls >= 2) return std::unexpected(UringErrorCode::kNoMemory);
    return MmapRegion::Map(hint, size, prot, flags, fd, offset);
  }
};
int main() {
  abi::IoUringParams params{};
  auto raw = ToResult(SysIoUringSetup(2, &params));
  if (!raw) return 1;
  auto owner = UniqueFd::Adopt(static_cast<std::int32_t>(*raw));
  if (!owner) return 2;
  {
    auto mappings = RingMappings::Map(owner->get(), params);
    if (!mappings || mappings->sq().empty() || mappings->sqes().size() != 128) return 3;
    if ((params.features & abi::kFeatSingleMmap) && mappings->sq().data() != mappings->cq().data()) return 4;
  }
  auto separate = params;
  separate.features &= ~abi::kFeatSingleMmap;
  {
    auto mappings = RingMappings::Map(owner->get(), separate);
    if (!mappings || mappings->sq().data() == mappings->cq().data()) return 5;
  }
  const auto before = RingMaps();
  auto rollback = RingMappings::Map<FailSecond>(owner->get(), separate);
  if (rollback || rollback.error() != UringErrorCode::kNoMemory || before < 0 || RingMaps() != before) return 6;
  for (int i = 0; i < 48; ++i) {
    auto registration = RegisteredRingFd::Register(owner->get());
    if (!registration) return 7;
    auto moved = std::move(*registration);
    if (registration->valid() || !moved.valid()) return 8;
    if (SysIoUringEnter(static_cast<std::int32_t>(moved.index()), 0, 0, abi::kEnterRegisteredRing) != 0) return 9;
    if (i % 2 == 0 && !moved.reset()) return 10;
  }
  auto rings = MmapRegion::Map(nullptr, 65536, kProtRead | kProtWrite, kMapPrivate | kMapAnonymous, -1, 0);
  auto sqes = MmapRegion::Map(nullptr, 65536, kProtRead | kProtWrite, kMapPrivate | kMapAnonymous, -1, 0);
  if (!rings || !sqes) return 11;
  abi::IoUringParams no_mmap{};
  no_mmap.flags = abi::kSetupNoMmap;
  no_mmap.sq_off.user_addr = Address(sqes->bytes().data());
  no_mmap.cq_off.user_addr = Address(rings->bytes().data());
  auto no_fd = ToResult(SysIoUringSetup(2, &no_mmap));
  if (!no_fd) return 12;
  auto no_owner = UniqueFd::Adopt(static_cast<std::int32_t>(*no_fd));
  if (!no_owner) return 13;
  const NoMmapMemory memory{rings->bytes(), sqes->bytes()};
  {
    auto mappings = RingMappings::Map(no_owner->get(), no_mmap, &memory);
    if (!mappings || mappings->sq().data() != memory.rings.data() || mappings->sqes().data() != memory.sqes.data()) return 14;
    const NoMmapMemory too_small{memory.rings.first(1), memory.sqes};
    if (RingMappings::Map(no_owner->get(), no_mmap, &too_small)) return 15;
  }
  if (!no_owner->reset()) return 16;
  rings->bytes()[65535] = std::byte{0x6b};
  if (rings->bytes()[65535] != std::byte{0x6b}) return 17;
  abi::IoUringParams only{};
  only.flags = abi::kSetupNoMmap | abi::kSetupRegisteredFdOnly;
  only.sq_off.user_addr = Address(sqes->bytes().data());
  only.cq_off.user_addr = Address(rings->bytes().data());
  auto registered_only = ToResult(SysIoUringSetup(2, &only));
  if (!registered_only) return 18;
  auto only_owner = RegisteredRingFd::Adopt(static_cast<std::uint32_t>(*registered_only));
  if (!only_owner || SysIoUringEnter(static_cast<std::int32_t>(only_owner->index()), 0, 0, abi::kEnterRegisteredRing) != 0)
    return 19;
  if (!only_owner->reset()) return 20;
  return 0;
}
