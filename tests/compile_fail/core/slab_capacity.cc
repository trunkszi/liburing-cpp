// expected-error: slab Log must fit the token slot field
#include <liburing/core/operation_slab.hpp>
static_assert(sizeof(liburing::OperationSlab<std::uint64_t, 23>) > 0);
