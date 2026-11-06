#include <liburing/abi/proofs.hpp>

static_assert(liburing::abi::CheckLayouts());
static_assert(liburing::abi::kOpLast == 65);
static_assert(liburing::abi::kRegisterLast == 38);
int main() { return 0; }
