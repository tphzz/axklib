#include "detail.hpp"
#include "rawdisk/open/locator.hpp"

#include <algorithm>
#include <format>

namespace rawdisk::open {
namespace {

constexpr std::size_t kMaxLocator = 8192;

bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}
// Characters allowed unescaped in a value besides unreserved ones (RFC 3986 query, minus & = +).
bool value_extra(unsigned char c) { return c == ':' || c == '/' || c == '@'; }
bool mode_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }
bool key_char(char c) { return mode_char(c) || c == '_'; }

bool valid_key(std::string_view k) {
    const auto dot = k.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == k.size() ||
        k.find('.', dot + 1) != std::string_view::npos)
        return false;
    for (char c : k)
        if (c != '.' && !key_char(c))
            return false;
    return true;
}

std::string encode(std::string_view s) {
    std::string out;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (unreserved(c) || value_extra(c))
            out += static_cast<char>(c);
        else
            out += std::format("%{:02X}", static_cast<unsigned>(c));
    }
    return out;
}

int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

std::optional<std::string> decode(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == '%') {
            if (i + 2 >= s.size())
                return std::nullopt; // needs two hex digits
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi < 0 || lo < 0)
                return std::nullopt;
            const char v = static_cast<char>(hi * 16 + lo);
            if (v == '\0')
                return std::nullopt;
            out += v;
            i += 2;
        } else if (unreserved(c) || value_extra(c)) {
            out += static_cast<char>(c);
        } else {
            return std::nullopt;
        }
    }
    return out;
}

} // namespace

std::string format_locator(const Locator &l) {
    std::string out = l.mode;
    char sep = '?';
    for (const auto &[k, v] : l.answers) { // std::map: sorted, canonical
        out += sep;
        out += k;
        out += '=';
        out += encode(v);
        sep = '&';
    }
    return out;
}

Result<Locator> parse_locator(std::string_view text) noexcept {
    return detail::guard([&]() -> Result<Locator> {
        auto bad = [] { return detail::fail(Errc::locator_invalid); };
        if (text.empty() || text.size() > kMaxLocator)
            return bad();
        Locator l;
        const auto q = text.find('?');
        l.mode = std::string(text.substr(0, q));
        if (l.mode.empty() || !std::ranges::all_of(l.mode, mode_char))
            return bad();
        if (q == std::string_view::npos)
            return l;
        std::string_view rest = text.substr(q + 1);
        if (rest.empty())
            return bad();
        for (;;) {
            const auto amp = rest.find('&');
            const std::string_view pair = rest.substr(0, amp);
            const auto eq = pair.find('=');
            if (eq == std::string_view::npos)
                return bad();
            const std::string_view key = pair.substr(0, eq);
            if (!valid_key(key))
                return bad();
            auto value = decode(pair.substr(eq + 1));
            if (!value)
                return bad();
            if (!l.answers.emplace(std::string(key), std::move(*value)).second)
                return bad(); // duplicate
            if (amp == std::string_view::npos)
                break;
            rest.remove_prefix(amp + 1);
            if (rest.empty())
                return bad(); // trailing '&'
        }
        return l;
    });
}

Outcome<Opened> open(std::string_view text, Access access, const Registry &registry, Context context,
                     CancellationToken stop) {
    auto l = parse_locator(text);
    if (!l)
        return std::unexpected(SubmitError{l.error(), "", "", "malformed locator"});
    auto mode = registry.find(l->mode);
    if (!mode)
        return std::unexpected(
            SubmitError{make_error_code(Errc::locator_invalid), "", "", std::format("unknown mode '{}'", l->mode)});
    return run(std::move(mode), l->answers, access, std::move(context), std::move(stop));
}

} // namespace rawdisk::open
