/** @file @brief Native C-linkage leaves expose dispatch and encoding for binary audits. */
#pragma once
#include <liburing/core/dispatcher.hpp>
#include <liburing/core/op_header.hpp>
#include <liburing/ring/completion_queue.hpp>
extern "C" {
std::uint64_t proactor_token_pack_probe(std::uint32_t slot, std::uint8_t subsystem, std::uint32_t generation) noexcept;
std::uint64_t proactor_fsm_step_probe(std::uint64_t header, std::uint32_t flags) noexcept;
void proactor_sqe_encode_probe(liburing::abi::FlatSqe* destination, std::int32_t fd, std::uint64_t address,
                               std::uint32_t length) noexcept;
std::size_t proactor_cq_reap_probe(liburing::ring::CqCursor* cursor, liburing::Completion* output, std::size_t size) noexcept;
std::uint64_t proactor_static_dispatch_probe(const liburing::Delivery* deliveries, std::size_t size) noexcept;
}
