#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "axklib/media.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    constexpr std::size_t maximum_input = 4U * 1024U * 1024U;
    if (data == nullptr || size > maximum_input)
        return 0;
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
        bytes[index] = static_cast<std::byte>(data[index]);
    auto reader = std::make_shared<axk::MemoryReader>(std::move(bytes));
    if (auto image = axk::FatImage::open(reader, "fat-image-fuzz.ima"); image)
        static_cast<void>(image->objects(maximum_input));
    return 0;
}
