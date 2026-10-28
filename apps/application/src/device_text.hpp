#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <unicode/uchar.h>
#include <unicode/unistr.h>

namespace axk::app::device_detail {
inline std::string device_text(std::string_view value) {
    const auto text = icu::UnicodeString::fromUTF8(value);
    icu::UnicodeString clean;
    std::size_t count{};
    for (std::int32_t at = 0; at < text.length() && count < 256U;) {
        const auto character = text.char32At(at);
        at += U16_LENGTH(character);
        const auto category = u_charType(character);
        if (category == U_CONTROL_CHAR || category == U_FORMAT_CHAR || category == U_LINE_SEPARATOR ||
            category == U_PARAGRAPH_SEPARATOR)
            continue;
        clean.append(character);
        ++count;
    }
    std::string result;
    clean.toUTF8String(result);
    return result;
}
} // namespace axk::app::device_detail
