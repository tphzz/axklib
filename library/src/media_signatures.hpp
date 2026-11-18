#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

namespace axk::detail {

inline bool ex5_descriptor_signature(std::span<const std::byte> descriptor) {
    constexpr std::string_view signature{"SY1200 V0.0.0   "};
    constexpr std::size_t offset = 0x10U;
    if (descriptor.size() < offset + signature.size())
        return false;
    for (std::size_t index = 0; index < signature.size(); ++index) {
        if (descriptor[offset + index] != static_cast<std::byte>(signature[index]))
            return false;
    }
    return true;
}

inline bool ex5_disk_signature(std::span<const std::byte> prefix) {
    return prefix.size() >= 0x220U && ex5_descriptor_signature(prefix.subspan(0x200U, 0x20U));
}

inline bool sfs_disk_signature(std::span<const std::byte> prefix) {
    constexpr std::string_view magic{"YAMAHA_dev3"};
    return prefix.size() >= magic.size() && !ex5_disk_signature(prefix) &&
           std::ranges::equal(prefix.first(magic.size()), magic,
                              [](std::byte left, char right) { return left == static_cast<std::byte>(right); });
}

} // namespace axk::detail
