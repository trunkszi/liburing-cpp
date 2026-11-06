#pragma once
#include <cstddef>
#include <cstdint>
#include <liburing/error/uring_error.hpp>
#include <meta>
#include <type_traits>
namespace liburing {
class[[= uarch::layout::register_leaf]] OperationPayload {
  std::uint64_t raw_{};

 public:
  static constexpr std::uint64_t kMaxRaw = 0x00ffffffffffffffULL;
  [[nodiscard]] static constexpr Result<OperationPayload> New(std::uint64_t raw) noexcept {
    if (raw > kMaxRaw) return std::unexpected(UringErrorCode::kInvalid);
    return NewUnchecked(raw);
  }
  [[nodiscard]] static constexpr OperationPayload NewUnchecked(std::uint64_t raw) noexcept {
    OperationPayload result;
    result.raw_ = raw;
    return result;
  }
  [[nodiscard]] constexpr std::uint64_t Raw() const noexcept { return raw_; }
};
static_assert(uarch::NoPadding<OperationPayload> && uarch::RegisterLeaf<OperationPayload> &&
              uarch::RegisterPassable<OperationPayload>);
template <class T>
[[nodiscard]] consteval unsigned PayloadBits() {
  if constexpr (!std::is_class_v<T> || !std::is_standard_layout_v<T>)
    return 0;
  else {
    unsigned bits = 0;
    bool valid = true;
    static constexpr auto fields =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
    template for (constexpr auto field : fields) {
      using Field = typename[:std::meta::type_of(field):];
      if constexpr (std::is_same_v<Field, std::uint8_t> || std::is_same_v<Field, std::uint16_t> ||
                    std::is_same_v<Field, std::uint32_t> || std::is_same_v<Field, std::uint64_t>)
        bits += static_cast<unsigned>(sizeof(Field) * 8);
      else
        valid = false;
    }
    return valid && bits <= 56 ? bits : 0;
  }
}
template <class T>
concept PayloadEncodable = PayloadBits<T>() > 0;
template <PayloadEncodable T>
[[nodiscard]] constexpr std::uint64_t EncodePayload(const T& value) noexcept {
  std::uint64_t raw = 0;
  static constexpr auto fields =
      std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
  template for (constexpr auto field : fields) {
    using Field = typename[:std::meta::type_of(field):];
    raw = (raw << (sizeof(Field) * 8)) | value.[:field:];
  }
  return raw;
}
template <PayloadEncodable T>
[[nodiscard]] constexpr Result<T> DecodePayload(std::uint64_t raw) noexcept {
  constexpr std::size_t bits = PayloadBits<T>();
  if ((raw >> bits) != 0) return std::unexpected(UringErrorCode::kInvalid);
  T value{};
  unsigned remaining = PayloadBits<T>();
  static constexpr auto fields =
      std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
  template for (constexpr auto field : fields) {
    using Field = typename[:std::meta::type_of(field):];
    remaining -= static_cast<unsigned>(sizeof(Field) * 8);
    value.[:field:] = static_cast<Field>(raw >> remaining);
  }
  return value;
}
}  // namespace liburing
