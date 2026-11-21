#include "instrument.hpp"

#include <algorithm>
#include <bit>
#include <optional>

namespace axk::typhoon {
namespace {
using namespace iff;
struct Split {
    std::optional<std::uint8_t> first;
    std::optional<std::size_t> ref;
};
Result<void> group(Item &item, Bytes body, std::size_t &index) {
    std::vector<Split> splits;
    Bytes parm;
    auto ok = chunks(body, [&](std::string_view tag, Bytes field) -> Result<void> {
        if (tag == "Parm") {
            if (field.size() != 64 || !parm.empty())
                return std::unexpected(invalid("Unsupported group parameter layout"));
            parm = field;
        }
        if (tag != "Splt")
            return {};
        if (splits.size() >= 256)
            return std::unexpected(invalid("Too many splits in one group"));
        auto &split = splits.emplace_back();
        return chunks(field, [&](std::string_view key, Bytes data) -> Result<void> {
            if (key == "Parm") {
                if (splits.size() == 1)
                    return {}; // The first split always starts at native key zero.
                if (split.first || data.size() != 2 || std::to_integer<unsigned>(data[0]) > 127)
                    return std::unexpected(invalid("Unsupported split point"));
                split.first = std::to_integer<std::uint8_t>(data[0]);
            }
            if (key == "Wave") {
                if (split.ref || index >= item.references.size())
                    return std::unexpected(invalid("Invalid split Wave reference"));
                split.ref = index++;
            }
            return {};
        });
    });
    if (!ok)
        return ok;
    if (parm.empty())
        return std::unexpected(invalid("Group has no range"));
    auto byte = [&](unsigned n) { return std::to_integer<std::uint8_t>(parm[n]); };
    if (byte(0) > byte(1) || byte(1) > 127 || byte(2) > byte(3) || byte(3) > 127)
        return std::unexpected(invalid("Unsupported group range"));
    if (byte(7) > 127)
        return std::unexpected(invalid("Group Volume is outside the supported range 0..127"));
    const auto trigger = static_cast<TriggerMode>(std::clamp<int>(std::bit_cast<std::int8_t>(byte(14)), -2, 1));
    constexpr unsigned voice_keys = 109, midi_origin = 12;
    item.has_split_table |= !splits.empty();
    for (std::size_t i = 0; i < splits.size(); ++i) {
        const auto &s = splits[i];
        if (i && !s.first)
            return std::unexpected(invalid("Missing split point"));
        const unsigned first = s.first.value_or(0);
        const unsigned next = i + 1U < splits.size() ? splits[i + 1].first.value_or(0) : voice_keys;
        if (i + 1U < splits.size() && next <= first)
            return std::unexpected(invalid("Split points not ascending"));
        if (!s.ref)
            continue; // An empty split closes the preceding region; it is not a Wave.
        const auto low = std::max<unsigned>(first, byte(0));
        const auto high = std::min<unsigned>({next - 1U, byte(1), voice_keys - 1U});
        if (low <= high)
            item.regions.push_back({*s.ref, static_cast<std::uint8_t>(low + midi_origin),
                                    static_cast<std::uint8_t>(high + midi_origin), byte(2), byte(3), byte(6), trigger,
                                    std::bit_cast<std::int8_t>(byte(4)), std::bit_cast<std::int8_t>(byte(5)), byte(7)});
    }
    return {};
}
} // namespace
Result<void> instrument_layout(Item &item, iff::Bytes body) {
    if (item.type != "performance" && item.type != "voice")
        return {};
    std::size_t index = 0;
    auto ok = iff::chunks(body, [&](std::string_view tag, iff::Bytes field) -> Result<void> {
        if (item.type == "voice" && tag == "Grop")
            return group(item, field, index);
        if (item.type != "performance" || (tag != "Entr" && tag != "PChg"))
            return {};
        iff::Bytes parm;
        std::optional<std::size_t> target;
        auto result = iff::chunks(field, [&](std::string_view inner, iff::Bytes value) -> Result<void> {
            if (inner == "Parm")
                parm = value;
            if (inner == "Voic") {
                if (target || index >= item.references.size())
                    return std::unexpected(iff::invalid("Invalid Voice reference"));
                target = index++;
            }
            return {};
        });
        if (!result || tag == "PChg" || !target)
            return result;
        if (parm.size() != 12)
            return std::unexpected(iff::invalid("Unknown Performance entry layout"));
        item.parts.push_back(
            {*target, std::to_integer<std::uint8_t>(parm[0]), std::bit_cast<std::int8_t>(parm[1]), iff::u16(parm, 2)});
        return {};
    });
    if (!ok)
        return ok;
    if (index != item.references.size())
        return std::unexpected(iff::invalid("Unmapped native instrument references"));
    return {};
}
} // namespace axk::typhoon
