#include <liburing/core/deferred_queue.hpp>
#include <liburing/core/operation_slab.hpp>
using namespace liburing;
struct Payload {
  std::uint64_t value{};
};
constexpr bool stale_token_dropped(std::uint32_t seed = 0) {
  OperationSlab<Payload, 2> slab(7, seed);
  const auto token = slab.TryAllocate(OpKind::kOneShot);
  if (!token || slab.Get(kNullToken) || slab.Get(MakeToken(Slot(*token), 0, 7, Generation(*token) + 1))) return false;
  if (!slab.Arm(*token) || !slab.Complete(*token, CqeClass::kTerminal)) return false;
  if (!slab.Get(*token)) return false;
  if (!slab.FinishDispatch(*token) || slab.Get(*token)) return false;
  const auto reused = slab.TryAllocate(OpKind::kOneShot);
  return reused && Slot(*reused) == Slot(*token) && Generation(*reused) != Generation(*token) && !slab.Get(*token);
}
static_assert(stale_token_dropped());
constexpr bool generation_exhaustion_retires_slot() {
  OperationSlab<Payload, 1> slab(7, 0xfffffffeU);
  const auto token = slab.TryAllocate(OpKind::kOneShot);
  if (!token || Generation(*token) != 0xffffffffU || !slab.Arm(*token)) return false;
  if (!slab.Complete(*token, CqeClass::kTerminal)) return false;
  if (State(*slab.Header(*token)) != OpState::kRetiring) return false;
  if (!slab.FinishDispatch(*token)) return false;
  return !slab.Get(*token) && !slab.TryAllocate(OpKind::kOneShot);
}
constexpr bool multishot_more_retains_slot() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = slab.TryAllocate(OpKind::kMultishot);
  if (!token || !slab.Arm(*token) || !slab.Complete(*token, CqeClass::kMore)) return false;
  if (State(*slab.Header(*token)) != OpState::kArmed || !slab.FinishDispatch(*token)) return false;
  if (!slab.Complete(*token, CqeClass::kTerminal) || slab.TryAllocate(OpKind::kOneShot)) return false;
  return slab.FinishDispatch(*token) && !slab.Get(*token);
}
constexpr bool chain_member_countdown_retires() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = slab.TryAllocate(OpKind::kChain, 3);
  if (!token || !slab.Arm(*token)) return false;
  for (std::uint8_t member : std::array<std::uint8_t, 3>{2, 0, 1}) {
    const auto part = token->with<^^TokenSpec::member>(member);
    if (!slab.Complete(part, CqeClass::kTerminal)) return false;
    if (!slab.FinishDispatch(part)) return false;
    if (member != 1 && !slab.Get(*token)) return false;
  }
  return !slab.Get(*token);
}
constexpr bool deferred_queue_fifo_order() {
  DeferredQueue<4> queue;
  if (!queue.TryEnqueue(1) || !queue.TryEnqueue(2) || !queue.TryEnqueue(3) || queue.TryEnqueue(1)) return false;
  if (!queue.Remove(2) || queue.Remove(2)) return false;
  if (queue.PopFront() != 1 || queue.PopFront() != 3 || queue.PopFront() != 0) return false;
  return queue.TryEnqueue(2) && queue.PopFront() == 2 && queue.Empty();
}
static_assert(generation_exhaustion_retires_slot());
static_assert(multishot_more_retains_slot());
static_assert(chain_member_countdown_retires());
static_assert(deferred_queue_fifo_order());
constexpr bool deferred_queue_tail_reuse() {
  DeferredQueue<4> queue;
  for (std::uint32_t slot = 1; slot < 4; ++slot)
    if (queue.Contains(slot)) return false;
  if (!queue.Empty() || queue.Size() != 0 || queue.Front() != 0) return false;
  if (!queue.TryEnqueue(1) || !queue.TryEnqueue(2) || !queue.TryEnqueue(3)) return false;
  if (!queue.Remove(3) || queue.Contains(3) || !queue.Contains(2) || queue.Size() != 2) return false;
  if (!queue.TryEnqueue(3) || !queue.Remove(2) || !queue.Remove(3) || !queue.Contains(1)) return false;
  if (!queue.TryEnqueue(2) || queue.PopFront() != 1 || !queue.Contains(2) || queue.Front() != 2) return false;
  if (!queue.Remove(2) || !queue.Empty() || queue.Size() != 0 || queue.Remove(2)) return false;
  for (std::uint32_t slot = 1; slot < 4; ++slot)
    if (queue.Contains(slot)) return false;
  return queue.TryEnqueue(1) && queue.PopFront() == 1 && queue.PopFront() == 0 && queue.Empty();
}
static_assert(deferred_queue_tail_reuse());
constexpr bool cancel_preserves_armed_slot() {
  OperationSlab<Payload, 1> slab(7);
  auto token = slab.TryAllocate(OpKind::kOneShot);
  if (!token || !slab.Defer(*token) || !slab.CancelUnsubmitted(*token)) return false;
  if (slab.CancelUnsubmitted(*token)) return false;
  token = slab.TryAllocate(OpKind::kOneShot);
  if (!token || !slab.Arm(*token) || slab.CancelUnsubmitted(*token) || !slab.RequestCancel(*token)) return false;
  if (slab.TryAllocate(OpKind::kOneShot) || !slab.Get(*token)) return false;
  return slab.Complete(*token, CqeClass::kTerminal) && slab.FinishDispatch(*token) && !slab.FinishDispatch(*token);
}
constexpr bool invalid_tokens_are_rejected() {
  OperationSlab<Payload, 2> slab(7);
  const auto token = slab.TryAllocate(OpKind::kChain, 3);
  if (!token || !slab.Arm(*token)) return false;
  if (slab.Get(token->with<^^TokenSpec::subsystem>(8U)) || slab.Get(token->with<^^TokenSpec::slot>(4U)) ||
      slab.Get(token->with<^^TokenSpec::slot>(0U)) || slab.Get(token->with<^^TokenSpec::member>(3U)))
    return false;
  if (!slab.Complete(*token, CqeClass::kTerminal) || !slab.FinishDispatch(*token)) return false;
  return !slab.Complete(*token, CqeClass::kTerminal);
}
constexpr bool queue_slot_membership() {
  DeferredQueue<4> queue;
  if (queue.TryEnqueue(0) || queue.TryEnqueue(4) || queue.Remove(0) || queue.Contains(4)) return false;
  if (!queue.TryEnqueue(2) || !queue.Contains(2) || queue.Size() != 1 || queue.Front() != 2) return false;
  if (!queue.Remove(2) || !queue.Empty() || queue.Size() != 0) return false;
  if (!queue.TryEnqueue(3) || !queue.TryEnqueue(1) || !queue.Remove(3)) return false;
  return queue.Front() == 1 && queue.PopFront() == 1 && queue.Empty();
}
constexpr bool zc_slab_retains_payload() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = slab.TryAllocate(OpKind::kZeroCopy);
  if (!token || !slab.Arm(*token)) return false;
  slab.Get(*token)->value = 42;
  if (!slab.Complete(*token, CqeClass::kNotification) || !slab.FinishDispatch(*token)) return false;
  if (slab.TryAllocate(OpKind::kOneShot) || slab.Get(*token)->value != 42) return false;
  if (!slab.Complete(*token, CqeClass::kMore) || !slab.Get(*token)) return false;
  if (!slab.FinishDispatch(*token) || slab.Get(*token)) return false;
  return slab.TryAllocate(OpKind::kOneShot).has_value();
}
static_assert(zc_slab_retains_payload());
static_assert(cancel_preserves_armed_slot());
static_assert(invalid_tokens_are_rejected());
static_assert(queue_slot_membership());
constexpr bool same_batch_multishot_more_and_final() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = *slab.TryAllocate(OpKind::kMultishot);
  if (!slab.Arm(token) || !slab.Complete(token, CqeClass::kMore) || !slab.Complete(token, CqeClass::kTerminal)) return false;
  if (!slab.FinishDispatch(token) || !slab.Get(token)) return false;
  return slab.FinishDispatch(token) && !slab.Get(token);
}
static_assert(same_batch_multishot_more_and_final());
constexpr bool same_batch_zc_primary_and_notif() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = *slab.TryAllocate(OpKind::kZeroCopy);
  if (!slab.Arm(token) || !slab.Complete(token, CqeClass::kMore) || !slab.Complete(token, CqeClass::kNotification))
    return false;
  if (!slab.FinishDispatch(token) || !slab.Get(token)) return false;
  return slab.FinishDispatch(token) && !slab.Get(token);
}
constexpr bool zc_notif_first_then_no_more_primary() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = *slab.TryAllocate(OpKind::kZeroCopy);
  if (!slab.Arm(token) || !slab.Complete(token, CqeClass::kNotification) || !slab.Complete(token, CqeClass::kTerminal))
    return false;
  if (!slab.FinishDispatch(token) || !slab.Get(token)) return false;
  return slab.FinishDispatch(token) && !slab.Get(token);
}
static_assert(same_batch_zc_primary_and_notif());
static_assert(zc_notif_first_then_no_more_primary());
constexpr bool oneshot_with_more_stays_armed() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = *slab.TryAllocate(OpKind::kOneShot);
  if (!slab.Arm(token) || !slab.Complete(token, CqeClass::kMore) || !slab.FinishDispatch(token)) return false;
  if (!slab.Get(token) || State(*slab.Header(token)) != OpState::kArmed) return false;
  return slab.Complete(token, CqeClass::kTerminal) && slab.FinishDispatch(token) && !slab.Get(token);
}
static_assert(oneshot_with_more_stays_armed());
constexpr bool chain_with_zc_member_waits_notif() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = *slab.TryAllocate(OpKind::kChain, 2);
  const auto timeout = token.with<^^TokenSpec::member>(1U);
  if (!slab.Arm(token) || !slab.Complete(token, CqeClass::kMore) || !slab.Complete(timeout, CqeClass::kTerminal)) return false;
  if (!slab.FinishDispatch(token) || !slab.FinishDispatch(timeout) || !slab.Get(token)) return false;
  if (slab.TryAllocate(OpKind::kOneShot) || !slab.Complete(token, CqeClass::kNotification)) return false;
  return slab.FinishDispatch(token) && !slab.Get(token);
}
static_assert(chain_with_zc_member_waits_notif());
template <class Slab>
concept HasRearm = requires(Slab& slab, UserDataToken token) { slab.Rearm(token); };
static_assert(!HasRearm<OperationSlab<Payload, 1>>);
constexpr bool rearm_requires_submission() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = *slab.TryAllocate(OpKind::kMultishot);
  if (!slab.Arm(token) || !slab.Complete(token, CqeClass::kTerminal) || !slab.FinishDispatch(token)) return false;
  const auto next = slab.TryAllocate(OpKind::kMultishot);
  if (!next || *next == token || slab.Contains(token) || slab.Complete(*next, CqeClass::kTerminal)) return false;
  return slab.Arm(*next) && slab.Complete(*next, CqeClass::kTerminal) && slab.FinishDispatch(*next);
}
static_assert(rearm_requires_submission());
constexpr bool batch_pin_limit_preserves_cqe() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = *slab.TryAllocate(OpKind::kOneShot);
  if (!slab.Arm(token)) return false;
  for (unsigned i = 0; i < kMaxDispatchPins; ++i)
    if (!slab.Complete(token, CqeClass::kMore)) return false;
  if (slab.Complete(token, CqeClass::kTerminal) || DispatchPins(*slab.Header(token)) != kMaxDispatchPins) return false;
  if (!slab.FinishDispatch(token) || !slab.Complete(token, CqeClass::kTerminal)) return false;
  for (unsigned i = 1; i < kMaxDispatchPins; ++i) {
    if (!slab.FinishDispatch(token) || !slab.Get(token)) return false;
  }
  return slab.FinishDispatch(token) && !slab.Get(token);
}
constexpr bool chain_multiple_zc_members() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = *slab.TryAllocate(OpKind::kChain, 2);
  const auto second = token.with<^^TokenSpec::member>(1U);
  if (!slab.Arm(token) || !slab.Complete(token, CqeClass::kMore) || !slab.Complete(second, CqeClass::kMore)) return false;
  if (!slab.Complete(token, CqeClass::kNotification) || !slab.Complete(second, CqeClass::kNotification)) return false;
  for (unsigned i = 0; i < 3; ++i)
    if (!slab.FinishDispatch(token) || !slab.Get(token)) return false;
  return slab.FinishDispatch(token) && !slab.Get(token);
}
constexpr bool chain_notif_first_waits_primary() {
  OperationSlab<Payload, 1> slab(7);
  const auto token = *slab.TryAllocate(OpKind::kChain, 2);
  const auto second = token.with<^^TokenSpec::member>(1U);
  if (!slab.Arm(token) || !slab.Complete(token, CqeClass::kNotification) || !slab.Complete(second, CqeClass::kTerminal))
    return false;
  if (!slab.FinishDispatch(token) || !slab.FinishDispatch(second) || !slab.Get(token)) return false;
  if (!slab.Complete(token, CqeClass::kMore)) return false;
  return slab.FinishDispatch(token) && !slab.Get(token);
}
static_assert(chain_multiple_zc_members());
static_assert(chain_notif_first_waits_primary());
int main() {
  volatile std::uint32_t seed = 13;
  return batch_pin_limit_preserves_cqe() && chain_multiple_zc_members() && chain_notif_first_waits_primary() &&
                 rearm_requires_submission() && chain_with_zc_member_waits_notif() && oneshot_with_more_stays_armed() &&
                 same_batch_zc_primary_and_notif() && zc_notif_first_then_no_more_primary() &&
                 same_batch_multishot_more_and_final() && stale_token_dropped(seed) && generation_exhaustion_retires_slot() &&
                 multishot_more_retains_slot() && chain_member_countdown_retires() && deferred_queue_fifo_order() &&
                 deferred_queue_tail_reuse() && cancel_preserves_armed_slot() && invalid_tokens_are_rejected() &&
                 queue_slot_membership() && zc_slab_retains_payload()
             ? 0
             : 1;
}
