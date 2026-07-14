#include <cstddef>
#include <cstdint>
#include <span>

#include "axklib/object.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (data == nullptr || size > 4U * 1024U * 1024U)
        return 0;
    const auto bytes = std::span{reinterpret_cast<const std::byte *>(data), size};
    static_cast<void>(axk::decode_object_header(bytes));
    return 0;
}
