#include <liburing/core/completion.hpp>
#include <liburing/core/control_command.hpp>
#include <liburing/core/operation_slab.hpp>
using namespace liburing;
static_assert(MakeToken(1, 2, 7, 3).raw() == 0x0000000307800001ULL);
static_assert(uarch::reflect::byte_offset(^^OperationKey::ring_instance) == 8);
static_assert(uarch::reflect::byte_offset(^^Completion::result) == 8);
static_assert(uarch::reflect::byte_offset(^^Completion::flags) == 12);
static_assert(OpHeader{}.with<^^OpHeaderSpec::dispatch_pins>(1U).raw() == 0x1000U);
static_assert(MakeHeader(OpKind::kZeroCopy, 5).raw() == 0x0000000500020213ULL);
static_assert(ClassifyCqe(8) == CqeClass::kNotification && ClassifyCqe(2) == CqeClass::kMore);
static_assert(ClassifyCqe(0) == CqeClass::kTerminal && ClassifyCqe(10) == CqeClass::kNotification);
static_assert(alignof(uarch::CacheAligned<std::array<OpHeader, 4>>) == 64);
int main() {}
