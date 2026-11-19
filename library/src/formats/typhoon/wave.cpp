#include "axklib/formats/typhoon/wave.hpp"

#include <bit>
#include <cmath>
#include <map>
#include <string_view>

#include "iff.hpp"

namespace axk::typhoon {
namespace {
using namespace iff;
struct Sound {
    Wave wave;
    Bytes payload;
    bool compressed{};
};
Result<Sound> parse(Bytes bytes) {
    const auto type = form(bytes);
    if (!type || (*type != "AIFC" && *type != "AIFF"))
        return std::unexpected(invalid("Expected AIFF or AIFF-C Wave"));
    std::map<std::string_view, Bytes> fields;
    const auto ok = chunks(bytes.subspan(12), [&](std::string_view tag, Bytes b) -> Result<void> {
        if ((tag == "COMM" || tag == "SSND" || tag == "MARK" || tag == "INST") && !fields.emplace(tag, b).second)
            return std::unexpected(invalid("Duplicate AIFF Wave chunk"));
        return {};
    });
    if (!ok)
        return std::unexpected(ok.error());
    const auto comm = fields["COMM"], ssnd = fields["SSND"];
    if (comm.size() < (*type == "AIFC" ? 22U : 18U) || ssnd.size() < 8 || u32(ssnd, 0) > ssnd.size() - 8)
        return std::unexpected(invalid("Missing or truncated Wave data"));
    Sound result{};
    auto &w = result.wave;
    w.frames = u32(comm, 2);
    w.bits = u16(comm, 6);
    result.compressed = *type == "AIFC" && ascii(comm.subspan(18, 4)) == "DWVW";
    if (u16(comm, 0) != 1 || w.frames == 0 || w.frames > 262144 ||
        (result.compressed ? w.bits != 12
                           : ((w.bits != 12 && w.bits != 16 && w.bits != 8) ||
                              (*type == "AIFC" && ascii(comm.subspan(18, 4)) != "NONE"))))
        return std::unexpected(invalid("Unsupported Typhoon Wave encoding, channels or frame count"));
    const auto exponent = u16(comm, 8);
    if (exponent < 16383 || exponent > 16400)
        return std::unexpected(invalid("Invalid Wave sample rate"));
    const auto mantissa = (static_cast<std::uint64_t>(u32(comm, 10)) << 32U) | u32(comm, 14);
    w.sample_rate = static_cast<std::uint32_t>(
        std::lround(std::ldexp(static_cast<double>(mantissa), static_cast<int>(exponent) - 16383 - 63)));
    if (w.sample_rate == 0)
        return std::unexpected(invalid("Zero Wave sample rate"));
    result.payload = ssnd.subspan(8 + static_cast<std::size_t>(u32(ssnd, 0)));
    if (result.compressed ? result.payload.size() < (w.frames + 7U) / 8U
                          : result.payload.size() < w.frames * ((w.bits + 7U) / 8U))
        return std::unexpected(invalid("Truncated Wave frames"));
    std::map<std::uint16_t, std::uint32_t> markers;
    if (fields.contains("MARK")) {
        auto m = fields.at("MARK");
        if (m.size() < 2)
            return std::unexpected(invalid("Truncated Wave markers"));
        const auto count = u16(m, 0);
        m = m.subspan(2);
        for (unsigned i = 0; i < count; ++i) {
            if (m.size() < 7)
                return std::unexpected(invalid("Truncated Wave marker"));
            const auto length = 7U + std::to_integer<unsigned>(m[6]);
            const auto skip = length + (length & 1U);
            if (skip > m.size() || !markers.emplace(u16(m, 0), u32(m, 2)).second)
                return std::unexpected(invalid("Invalid Wave marker"));
            m = m.subspan(skip);
        }
    }
    if (fields.contains("INST")) {
        const auto inst = fields.at("INST");
        if (inst.size() != 20 || std::to_integer<unsigned>(inst[0]) > 127)
            return std::unexpected(invalid("Invalid Wave instrument chunk"));
        w.root_key = std::to_integer<std::uint8_t>(inst[0]);
        w.detune_cents = std::bit_cast<std::int8_t>(inst[1]);
        if (u16(inst, 14) != 0 &&
            (u16(inst, 14) != u16(inst, 8) || u16(inst, 16) != u16(inst, 10) || u16(inst, 18) != u16(inst, 12)))
            return std::unexpected(invalid("Separate Wave release loops are not supported"));
        if (u16(inst, 8) != 0) {
            const auto a = markers.find(u16(inst, 10)), z = markers.find(u16(inst, 12));
            if (u16(inst, 8) != 1 || a == markers.end() || z == markers.end() || a->second >= z->second ||
                z->second > w.frames)
                return std::unexpected(invalid("Invalid or unsupported Wave sustain loop"));
            w.loop = {{a->second, z->second - 1}};
        }
    }
    return result;
}
} // namespace

Result<Wave> inspect_wave(std::span<const std::byte> bytes) {
    auto s = parse(bytes);
    if (!s)
        return std::unexpected(s.error());
    return s->wave;
}
Result<std::vector<std::int16_t>> read_pcm16(std::span<const std::byte> bytes, std::uint32_t first_frame,
                                             std::uint32_t frame_count) {
    const auto s = parse(bytes);
    if (!s)
        return std::unexpected(s.error());
    if (first_frame > s->wave.frames || frame_count > s->wave.frames - first_frame)
        return std::unexpected(iff::invalid("Wave read exceeds its frame range"));
    std::vector<std::int16_t> samples;
    samples.reserve(frame_count);
    if (!s->compressed) {
        for (auto i = first_frame; i < first_frame + frame_count; ++i) {
            if (s->wave.bits == 8)
                samples.push_back(static_cast<std::int16_t>(std::bit_cast<std::int8_t>(s->payload[i]) * 256));
            else {
                const auto mask = s->wave.bits == 12 ? 0xfff0U : 0xffffU;
                samples.push_back(
                    std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(iff::u16(s->payload, 2U * i) & mask)));
            }
        }
        return samples;
    }
    // NuEdge's DWVW 1.2: width deltas and sample deltas both wrap. Never extend a short stream with zeroes.
    std::size_t bit{};
    bool short_stream = false;
    const auto next = [&]() -> int {
        if (bit / 8 >= s->payload.size()) {
            short_stream = true;
            return 0;
        }
        const int value = static_cast<int>((std::to_integer<unsigned>(s->payload[bit / 8]) >> (7U - bit % 8)) & 1U);
        ++bit;
        return value;
    };
    int width = 0, sample = 0;
    for (std::uint32_t i = 0; i < first_frame + frame_count; ++i) {
        int change = 0;
        while (change < 6 && next() == 0)
            ++change;
        if (change && next())
            change = -change;
        width = (width + change + 12) % 12;
        int delta = 0;
        if (width) {
            delta = 1;
            for (int j = 1; j < width; ++j)
                delta = delta * 2 + next();
            if (next())
                delta = -delta;
            if (delta == -2047)
                delta -= next();
        }
        if (short_stream)
            return std::unexpected(iff::invalid("Truncated DWVW sample"));
        sample = ((sample + delta + 4096) & 4095);
        if (i >= first_frame)
            samples.push_back(static_cast<std::int16_t>((sample >= 2048 ? sample - 4096 : sample) * 16));
    }
    return samples;
}
} // namespace axk::typhoon
