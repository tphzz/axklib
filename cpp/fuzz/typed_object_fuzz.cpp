#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "axklib/object.hpp"

#ifndef AXK_FUZZ_OBJECT_TYPE
#error "AXK_FUZZ_OBJECT_TYPE must identify the focused object decoder"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  constexpr std::size_t maximum_input = 4U * 1024U * 1024U;
  constexpr std::string_view magic{"FSFSDEV3SPLX"};
  constexpr std::string_view object_type{AXK_FUZZ_OBJECT_TYPE};
  if (data == nullptr || size > maximum_input)
    return 0;

  std::vector<std::byte> payload(std::max<std::size_t>(0x400U, 0x40U + size));
  std::transform(data, data + size, payload.begin() + 0x40,
                 [](std::uint8_t value) { return static_cast<std::byte>(value); });
  std::transform(magic.begin(), magic.end(), payload.begin(),
                 [](char value) { return static_cast<std::byte>(value); });
  std::transform(object_type.begin(), object_type.end(), payload.begin() + 0x0c,
                 [](char value) { return static_cast<std::byte>(value); });
  static_cast<void>(axk::decode_object(payload));
  return 0;
}
