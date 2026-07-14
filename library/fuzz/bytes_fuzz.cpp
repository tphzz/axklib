#include <cstddef>
#include <cstdint>
#include <span>

#include "axklib/bytes.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
    if (data == nullptr || size > 4U * 1024U * 1024U)
        return 0;
    const auto bytes =
        std::span{reinterpret_cast<const std::byte *>(data), size};
    const axk::ByteReader reader{bytes};
    const auto offset =
        size == 0U ? 0U : static_cast<std::size_t>(data[0]) % (size + 1U);
    const auto count = size < 2U ? 0U : static_cast<std::size_t>(data[1]);
    static_cast<void>(reader.slice(offset, count));
    static_cast<void>(reader.u8(offset));
    static_cast<void>(reader.be16(offset));
    static_cast<void>(reader.be32(offset));
    static_cast<void>(reader.le16(offset));
    static_cast<void>(reader.le32(offset));
    static_cast<void>(reader.ascii_field(offset, count));
    static_cast<void>(reader.printable_ascii_field(offset, count));
    static_cast<void>(reader.decoded_ascii_field(offset, count));
    return 0;
}
