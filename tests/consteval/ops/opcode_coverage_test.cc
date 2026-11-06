#include "encoding_cases.hpp"
consteval bool OpcodeCoverageProof() { return ops_test::AllEncodings(); }
static_assert(OpcodeCoverageProof());
static_assert(ops_test::Variants());
constexpr bool CapabilityMetadata() {
  using namespace liburing;
  using namespace liburing::ops;
  constexpr auto wide_caps = Nop128::RequiredCaps();
  constexpr auto recv_caps = RecvZc::RequiredCaps();
  constexpr auto numa_caps = FutexWait<abi::kFutex2SizeU32 | abi::kFutex2Numa | abi::kFutex2Mpol>::RequiredCaps();
  constexpr auto command_caps = UringCmd128Op<true>::RequiredCaps();
  return Nop::RequiredCaps().required_futex == 0 && wide_caps.setup_flags == (1U << 19) && wide_caps.require_query &&
         recv_caps.setup_flags == (1U << 11) && numa_caps.required_futex == 7 &&
         command_caps.setup_flags == ((1U << 19) | (1U << 11)) && command_caps.opcodes[1].raw() == 1;
}
constexpr bool ConstructorCannotBypassFlags() {
  using namespace liburing;
  using namespace liburing::ops;
  abi::FlatSqe fields{.opcode = 99, .flags = 66, .fd = 7, .user_data = 0x1234};
  return ops_test::Check(Nop{fields}, {.opcode = 0, .fd = 7}) && ops_test::Check(UntrackedNop{fields}, {.opcode = 0, .fd = 7});
}
static_assert(CapabilityMetadata());
static_assert(ConstructorCannotBypassFlags());
static_assert(uarch::NoPadding<liburing::ops::Nop> && sizeof(liburing::ops::Nop) == 64);
static_assert(uarch::NoPadding<liburing::ops::Nop128> && sizeof(liburing::ops::Nop128) == 128);
constexpr bool ChainStampPreserved() {
  using namespace liburing;
  using namespace liburing::ops;
  const auto token = MakeToken(5, 0, 7, 1);
  const auto member = MakeToken(5, 1, 7, 1);
  abi::FlatSqe sqes[2]{};
  sqes[0].flags = abi::kSqeIoLink;
  sqes[0].user_data = token.raw();
  sqes[1].user_data = member.raw();
  PrepareSend(3, 0x1000, 16).WithToken(token).Encode(sqes[0]);
  PrepareLinkTimeout(nullptr).WithToken(token).Encode(sqes[1]);
  if ((sqes[0].flags & abi::kSqeIoLink) == 0 || sqes[1].user_data != member.raw()) return false;
  PrepareLinkTimeout(nullptr).Encode(sqes[1]);
  if (sqes[1].user_data != member.raw()) return false;
  sqes[0].flags = abi::kSqeIoHardlink;
  const auto own = MakeToken(9, 0, 8, 2);
  PrepareNop().WithToken(own).Encode(sqes[0]);
  if (sqes[0].flags != abi::kSqeIoHardlink || sqes[0].user_data != own.raw()) return false;
  abi::FlatSqe128 wide{};
  wide.sqe.flags = abi::kSqeIoLink;
  wide.sqe.user_data = member.raw();
  PrepareNop128().WithToken(token).Encode(wide);
  if (wide.sqe.flags != abi::kSqeIoLink || wide.sqe.user_data != member.raw()) return false;
  std::array<std::byte, 80> payload{};
  payload[79] = std::byte{0x5a};
  PrepareUringCmd128(7, 9, payload).WithToken(token).Encode(wide);
  if (wide.sqe.flags != abi::kSqeIoLink || wide.sqe.user_data != member.raw() || wide.extension[7] != 0x5a00000000000000ULL)
    return false;
  PrepareUringCmd128(7, 9, payload).Encode(wide);
  return wide.sqe.flags == abi::kSqeIoLink && wide.sqe.user_data == member.raw();
}
constexpr bool DirectVariants() {
  using namespace liburing;
  using namespace liburing::ops;
  abi::FlatSqe actual{};
  PrepareMsgRingSendFd(3, 7, 11, 0x1234, 5).Encode(actual);
  if (actual.opcode != abi::kOpMsgRing || actual.fd != 3 || actual.address != abi::kMsgSendFd || actual.offset != 0x1234 ||
      actual.address3 != 7 || actual.file_or_splice != 12 || actual.operation_flags != 5)
    return false;
  actual = {};
  PrepareOpenatDirect(-100, 0x1000, 2, 0644, 9).Encode(actual);
  if (actual.opcode != abi::kOpOpenat || actual.fd != -100 || actual.address != 0x1000 || actual.len != 0644 ||
      actual.operation_flags != 2 || actual.file_or_splice != 10)
    return false;
  actual = {};
  PrepareSocketDirect(2, 1, 6, 8, 4).Encode(actual);
  if (actual.opcode != abi::kOpSocket || actual.fd != 2 || actual.offset != 1 || actual.len != 6 ||
      actual.file_or_splice != 9 || actual.operation_flags != 4)
    return false;
  actual = {};
  PrepareAcceptDirect(12, 0x2000, 0x3000, 10, 16).Encode(actual);
  return actual.opcode == abi::kOpAccept && actual.fd == 12 && actual.address == 0x2000 && actual.offset == 0x3000 &&
         actual.file_or_splice == 11 && actual.operation_flags == 16;
}
static_assert(ChainStampPreserved());
constexpr bool DirectFileSlots() {
  using namespace liburing;
  using namespace liburing::ops;
  for (const auto slot : std::array<std::uint32_t, 4>{0, 3, 0xfffffffeU, abi::kFileIndexAlloc}) {
    const auto expected = slot == abi::kFileIndexAlloc ? abi::kFileIndexAlloc : slot + 1U;
    abi::FlatSqe actual{};
    PrepareOpenatDirect(-100, 0x1000, 0, 0, slot).Encode(actual);
    if (actual.file_index() != expected) return false;
    PrepareSocketDirect(2, 1, 0, slot).Encode(actual);
    if (actual.file_index() != expected) return false;
    PrepareAcceptDirect(3, nullptr, nullptr, slot).Encode(actual);
    if (actual.file_index() != expected) return false;
    PrepareMsgRingSendFd(4, 5, slot, 6).Encode(actual);
    if (actual.file_index() != expected) return false;
  }
  return true;
}
static_assert(DirectVariants());
static_assert(DirectFileSlots());
int main() {
  return DirectFileSlots() && DirectVariants() && ChainStampPreserved() && CapabilityMetadata() &&
                 ConstructorCannotBypassFlags() && ops_test::AllEncodings() && ops_test::Variants()
             ? 0
             : 1;
}
