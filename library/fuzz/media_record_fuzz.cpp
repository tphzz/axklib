#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "fuzz_envelopes.hpp"

#include "axklib/media.hpp"

#ifndef AXK_FUZZ_MEDIA_KIND
#error "AXK_FUZZ_MEDIA_KIND must identify FAT or ISO parsing"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
    constexpr std::size_t maximum_input = 4U * 1024U * 1024U;
    if (data == nullptr || size > maximum_input)
        return 0;
    const auto input =
        std::span{reinterpret_cast<const std::byte *>(data), size};
    auto bytes = AXK_FUZZ_MEDIA_KIND == 0
                     ? axk::fuzz::fat_record_envelope(input)
                     : axk::fuzz::iso_record_envelope(input);
    auto reader = std::make_shared<axk::MemoryReader>(std::move(bytes));
    if constexpr (AXK_FUZZ_MEDIA_KIND == 0) {
        if (auto image = axk::FatImage::open(reader, "record-fuzz.ima"); image)
            static_cast<void>(image->objects(maximum_input));
    } else {
        if (auto image = axk::IsoImage::open(reader, "record-fuzz.iso"); image)
            static_cast<void>(image->objects(maximum_input));
    }
    return 0;
}
