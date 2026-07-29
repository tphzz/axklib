#include "audio_export_support.hpp"

#include <cctype>
#include <format>

namespace axk::audio_export_detail {

std::string safe_component(std::string value, std::string_view fallback) {
    const auto trim = [](std::string &text) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
            text.erase(text.begin());
        }
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
            text.pop_back();
        }
    };
    trim(value);
    std::size_t duplicate_count{};
    while (!value.empty() && value.back() == '*') {
        ++duplicate_count;
        value.pop_back();
    }
    if (duplicate_count != 0U)
        trim(value);
    std::string cleaned;
    bool previous_space{};
    bool previous_underscore{};
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '<' || character == '>') {
            cleaned += character == '<' ? "_lt_" : "_gt_";
            previous_space = false;
            previous_underscore = true;
            continue;
        }
        const bool invalid = byte < 0x20U || std::string_view{":\"/\\|?*"}.contains(character);
        if (invalid || character == '_') {
            if (!previous_underscore)
                cleaned += '_';
            previous_space = false;
            previous_underscore = true;
            continue;
        }
        if (std::isspace(byte) != 0) {
            if (!previous_space && !cleaned.empty())
                cleaned += ' ';
            previous_space = true;
            previous_underscore = false;
            continue;
        }
        cleaned += character;
        previous_space = false;
        previous_underscore = false;
    }
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.' || cleaned.back() == '_')) {
        cleaned.pop_back();
    }
    while (!cleaned.empty() && (cleaned.front() == ' ' || cleaned.front() == '.' || cleaned.front() == '_')) {
        cleaned.erase(cleaned.begin());
    }
    if (cleaned.empty())
        cleaned = fallback;
    if (duplicate_count != 0U)
        cleaned += std::format(" ({})", duplicate_count + 1U);
    return cleaned;
}

void append_publication_warnings(std::vector<std::string> &destination, const PublicationOutcome &publication) {
    for (const auto &warning : publication.warnings)
        destination.push_back(warning.code + ": " + warning.message);
}

} // namespace axk::audio_export_detail
