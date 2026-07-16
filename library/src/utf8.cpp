#include "axklib/utf8.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <utility>

#include <utf8cpp/utf8.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace axk::text {
namespace {

Error text_error(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::io, std::move(message));
}

std::string publication_suffix() {
    static std::atomic<std::uint64_t> sequence{1U};
#if defined(_WIN32)
    const auto process = static_cast<std::uint64_t>(_getpid());
#else
    const auto process = static_cast<std::uint64_t>(getpid());
#endif
    return std::format(".axklib-publication.p{}.{}.tmp", process, sequence.fetch_add(1U, std::memory_order_relaxed));
}

} // namespace

bool is_valid_utf8(std::string_view value) { return utf8::is_valid(value); }

Result<std::string> utf16_to_utf8(std::u16string_view value) {
    try {
        return utf8::utf16to8(value);
    } catch (const utf8::invalid_utf16 &) {
        return std::unexpected{text_error("text contains an invalid UTF-16 surrogate sequence")};
    }
}

Result<std::filesystem::path> path_from_utf8(std::string_view value) {
    if (!is_valid_utf8(value))
        return std::unexpected{text_error("path is not valid UTF-8")};
    if (std::ranges::contains(value, '\0'))
        return std::unexpected{text_error("path contains an embedded NUL character")};
    const std::u8string utf8_path{reinterpret_cast<const char8_t *>(value.data()),
                                  reinterpret_cast<const char8_t *>(value.data() + value.size())};
    return std::filesystem::path{utf8_path};
}

std::string path_to_utf8(const std::filesystem::path &value) {
    const auto text = value.generic_u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

Result<std::filesystem::path> temporary_sibling(const std::filesystem::path &target, std::string_view suffix) {
    const auto generated = suffix.empty() ? publication_suffix() : std::string{suffix};
    auto prefix_path = path_from_utf8(".");
    auto suffix_path = path_from_utf8(generated);
    if (!prefix_path)
        return std::unexpected{prefix_path.error()};
    if (!suffix_path)
        return std::unexpected{suffix_path.error()};
    if (suffix_path->has_parent_path())
        return std::unexpected{text_error("temporary suffix must not contain a path separator")};
    auto filename = std::move(*prefix_path);
    filename += target.filename().native();
    filename += suffix_path->native();
    return target.parent_path() / filename;
}

} // namespace axk::text
