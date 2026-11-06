#include <cstdio>
#include <liburing/caps/kernel_caps.hpp>

#include "../../support/host_kernel.hpp"
using namespace liburing;
using namespace liburing::caps;
static_assert(ParseKernelVersion("6.16.0-28-generic")->minor == 16);
static_assert(ParseKernelVersion("7.0.1")->major == 7);
static_assert(!ParseKernelVersion("not-a-kernel"));
static_assert(!ParseKernelVersion("6.4294967296.0"));
static_assert(!CheckKernelRelease("6.15.99", 6, 16));
static_assert(CheckKernelRelease("6.16.0", 6, 16).has_value());
int main() {
  if (const int gate = test_support::SkipBelowKernel(6, 16); gate >= 0) return gate;
  RequiredCaps req{};
  if (!req.RequireOpcode(abi::kOpNop)) return 2;
  req.required_futex = kFutexU32 | kFutexNuma | kFutexMpol;
  auto caps = KernelCaps::Probe(req);
  if (!caps) {
    std::printf("probe error %u\n", static_cast<unsigned>(caps.error()));
    return 3;
  }
  if (!caps->SupportsOpcode(abi::kOpNop) || !caps->SupportsFutex(kFutexNuma | kFutexMpol)) return 4;
  RequiredCaps mixed{};
  mixed.setup_flags = abi::kSetupSqeMixed | abi::kSetupCqeMixed;
  mixed.require_query = true;
  if (!mixed.RequireOpcode(abi::kOpUringCmd128)) return 5;
  auto extension = KernelCaps::Probe(mixed);
  if (!extension || extension->tier() != KernelTier::kExtendedMixed) return 6;
  if (!caps->query_available || caps->register_opcode_count < 38) return 7;
  return 0;
}
