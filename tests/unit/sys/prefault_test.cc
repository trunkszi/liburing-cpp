#include <liburing/sys/mmap_region.hpp>
using namespace liburing;
using namespace liburing::sys;
struct PrefaultFallback {
  inline static unsigned prefault = 0;
  inline static unsigned fallback = 0;
  static Result<MmapRegion> Map(void* hint, std::size_t length, std::int32_t prot, std::int32_t flags, std::int32_t fd,
                                std::uint64_t offset) noexcept {
    if ((flags & kMapPopulate) != 0) {
      ++prefault;
      return std::unexpected(UringErrorCode::kNoMemory);
    }
    ++fallback;
    return MmapRegion::Map(hint, length, prot, flags, fd, offset);
  }
};
int main() {
  abi::IoUringParams params{};
  const auto raw = ToResult(SysIoUringSetup(2, &params));
  if (!raw) return 1;
  auto owner = UniqueFd::Adopt(static_cast<std::int32_t>(*raw));
  if (!owner) return 2;
  auto mappings = RingMappings::Map<PrefaultFallback>(owner->get(), params);
  if (!mappings) return 3;
  const unsigned expected = (params.features & abi::kFeatSingleMmap) != 0 ? 2U : 3U;
  return PrefaultFallback::prefault == expected && PrefaultFallback::fallback == expected ? 0 : 4;
}
