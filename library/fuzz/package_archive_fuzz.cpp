#include <cstddef>
#include <cstdint>
#include <span>

#include "axklib/io.hpp"
#include "axklib/package.hpp"
#include "axklib/package_archive.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (data == nullptr || size > 4U * 1024U * 1024U)
        return 0;
    const auto bytes = std::span{reinterpret_cast<const std::byte *>(data), size};
    const axk::MemoryReader reader{bytes};
    static_cast<void>(axk::package_internal::inspect_archive(reader));
    static_cast<void>(axk::package_internal::read_archive(bytes));
    static_cast<void>(axk::open_portable_package(bytes, "fuzz.axkpkg"));
    return 0;
}
