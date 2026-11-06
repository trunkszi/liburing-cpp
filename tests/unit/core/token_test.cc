#include <liburing/core/completion.hpp>
#include <liburing/core/control_command.hpp>
#include <liburing/core/user_data_token.hpp>
using namespace liburing;
constexpr bool TokenRoundTrip() {
  const auto token = MakeToken(0x3fffffU, 3, 255, 0xffffffffU);
  return token.raw() == 0xffffffffffffffffULL && Slot(token) == 0x3fffffU && Member(token) == 3 && Subsystem(token) == 255 &&
         Generation(token) == 0xffffffffU && kNullToken.raw() == 0;
}
static_assert(TokenRoundTrip());
constexpr bool ControlFifo() {
  ControlQueue<4> queue;
  const ControlCommand detach{{MakeToken(1, 0, 7, 1), 9}, 0, ControlKind::kDetach, 0};
  const ControlCommand resize{{kNullToken, 9}, 128, ControlKind::kResize, 0};
  if (!queue.TryEnqueue(1, detach) || !queue.TryEnqueue(2, resize) || queue.TryEnqueue(1, resize)) return false;
  const auto first = queue.PopFront();
  const auto second = queue.PopFront();
  return first && second && first->kind == ControlKind::kDetach && second->kind == ControlKind::kResize && !queue.PopFront();
}
static_assert(ControlFifo());
static_assert(sizeof(Completion) == 16 && sizeof(CqeExtended) == 32);
int main() { return TokenRoundTrip() && ControlFifo() ? 0 : 1; }
