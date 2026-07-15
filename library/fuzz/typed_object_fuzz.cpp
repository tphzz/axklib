#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "fuzz_envelopes.hpp"

#include "axklib/object.hpp"

#ifndef AXK_FUZZ_OBJECT_TYPE
#error "AXK_FUZZ_OBJECT_TYPE must identify the focused object decoder"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    constexpr std::size_t maximum_input = 4U * 1024U * 1024U;
    if (data == nullptr || size > maximum_input)
        return 0;
    const auto input = std::span{reinterpret_cast<const std::byte *>(data), size};
    const auto payload = axk::fuzz::typed_object_envelope(input, AXK_FUZZ_OBJECT_TYPE);
    static_cast<void>(axk::decode_object(payload));
    return 0;
}
