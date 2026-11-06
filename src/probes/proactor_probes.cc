#include <liburing/ops/fs.hpp>
#include <liburing/probes/probes.hpp>
namespace {
template <std::uint8_t Id>
struct ProbeSink {
  static constexpr std::uint8_t kSubsystemId = Id;
  std::uint64_t* sum;
  [[nodiscard]] static consteval liburing::caps::RequiredCaps RequiredCaps() noexcept { return {}; }
  void OnCompletions(std::span<const liburing::Delivery> deliveries) noexcept {
    for (const auto& delivery : deliveries) *sum += static_cast<std::uint32_t>(delivery.completion.result);
  }
};
}  // namespace
extern "C" std::uint64_t proactor_token_pack_probe(std::uint32_t slot, std::uint8_t subsystem,
                                                   std::uint32_t generation) noexcept {
  return liburing::MakeToken(slot, 0, subsystem, generation).raw();
}
extern "C" std::uint64_t proactor_fsm_step_probe(std::uint64_t header, std::uint32_t flags) noexcept {
  return liburing::Advance(liburing::OpHeader::from_raw(header), liburing::ClassifyCqe(flags)).raw();
}
extern "C" void proactor_sqe_encode_probe(liburing::abi::FlatSqe* destination, std::int32_t fd, std::uint64_t address,
                                          std::uint32_t length) noexcept {
  liburing::ops::PrepareRead(fd, address, length).Encode(*destination);
}
extern "C" std::size_t proactor_cq_reap_probe(liburing::ring::CqCursor* cursor, liburing::Completion* output,
                                              std::size_t size) noexcept {
  return cursor->ReapBatch(std::span<liburing::Completion>{output, size});
}
extern "C" std::uint64_t proactor_static_dispatch_probe(const liburing::Delivery* deliveries, std::size_t size) noexcept {
  std::uint64_t sum = 0;
  ProbeSink<1> a{&sum};
  ProbeSink<2> b{&sum};
  liburing::StaticDispatcher dispatcher{a, b};
  auto result = dispatcher.DispatchBatch(std::span<const liburing::Delivery>{deliveries, size});
  return result ? sum : 0;
}
