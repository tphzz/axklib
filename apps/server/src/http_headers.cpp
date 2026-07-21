#include "http_headers.hpp"

#include <array>

namespace axk::server {
namespace {

bool rfc5987_attribute_character(unsigned char value) {
    const auto alphanumeric =
        (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
    return alphanumeric || value == '!' || value == '#' || value == '$' || value == '&' || value == '+' ||
           value == '-' || value == '.' || value == '^' || value == '_' || value == '`' || value == '|' || value == '~';
}

char hex_digit(unsigned int value) {
    constexpr std::array digits{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    return digits[value & 0x0fU];
}

} // namespace

std::string attachment_content_disposition(std::string_view filename) {
    std::string fallback;
    std::string encoded;
    fallback.reserve(filename.size());
    encoded.reserve(filename.size() * 3U);
    for (const auto character : filename) {
        const auto value = static_cast<unsigned char>(character);
        if (value >= 0x20U && value <= 0x7eU) {
            if (character == '"' || character == '\\')
                fallback.push_back('\\');
            fallback.push_back(character);
        } else {
            fallback.push_back('_');
        }
        if (rfc5987_attribute_character(value)) {
            encoded.push_back(character);
        } else {
            encoded.push_back('%');
            encoded.push_back(hex_digit(value >> 4U));
            encoded.push_back(hex_digit(value));
        }
    }
    if (fallback.empty()) {
        fallback = "download";
        encoded = fallback;
    }
    return "attachment; filename=\"" + fallback + "\"; filename*=UTF-8''" + encoded;
}

} // namespace axk::server
