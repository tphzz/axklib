#include "axklib/su700.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/bytes.hpp"

namespace axk {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported, std::move(message));
}
std::string trimmed(std::string value) {
    while (!value.empty() && value.back() == ' ')
        value.pop_back();
    return value;
}
std::string folded(std::string value) {
    for (auto &c : value)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return value;
}
Result<std::string> name_at(std::span<const std::byte> bytes, std::size_t offset) {
    if (bytes[offset + 8U] != std::byte{})
        return std::unexpected(invalid("Unterminated SU700 name"));
    std::string result;
    for (const auto byte : bytes.subspan(offset, 8U)) {
        const auto c = std::to_integer<unsigned char>(byte);
        if (c < 32U || c > 126U ||
            std::string_view{"/\\:*?\"<>|."}.find(static_cast<char>(c)) != std::string_view::npos)
            return std::unexpected(invalid("Unsupported SU700 name encoding"));
        result.push_back(static_cast<char>(c));
    }
    if (trimmed(result).empty())
        return std::unexpected(invalid("Empty active SU700 name"));
    return result;
}
Result<bool> complete_payload(const FatImage &image, const FatFile &file, bool song, const CancellationToken &cancel) {
    if (file.size < 14U)
        return false;
    auto head = image.read_file_prefix(file, 14U, cancel);
    if (!head)
        return std::unexpected(head.error());
    const ByteReader bytes{*head};
    const auto signature = bytes.ascii_field(0, 4);
    const auto length = bytes.be32(4);
    if (song) {
        if (*signature != "FLhd" || *length != file.size)
            return false;
        const std::uint32_t track_header = *bytes.ascii_field(8, 4) == "MThd" ? 8U : 14U;
        if (file.size < track_header + 8U)
            return false;
        auto track = image.read_file_range(file, track_header, 8U, cancel);
        if (!track)
            return std::unexpected(track.error());
        return *ByteReader{*track}.ascii_field(0, 4) == "MThd";
    }
    if (*signature != "FORM" || static_cast<std::uint64_t>(*length) + 8U != file.size ||
        *bytes.ascii_field(8, 4) != "AIFF")
        return false;
    std::uint64_t offset = 12;
    bool common = false;
    bool application = false;
    while (offset < file.size) {
        if (file.size - offset < 8U)
            return false;
        auto chunk = image.read_file_range(file, offset, 8U, cancel);
        if (!chunk)
            return std::unexpected(chunk.error());
        const ByteReader reader{*chunk};
        const auto size = *reader.be32(4);
        const auto kind = *reader.ascii_field(0, 4);
        if (kind == "COMM")
            common = size >= 18U;
        if (kind == "APPL" || kind == "SSND")
            application = true;
        offset += 8U + static_cast<std::uint64_t>(size) + (size & 1U);
        if (offset > file.size)
            return false;
    }
    return common && application;
}
} // namespace

Result<Su700Control> decode_su700_control(std::span<const std::byte> bytes) {
    if (bytes.size() != 7400U)
        return std::unexpected(invalid("SONGCONT.DAT must contain 7400 bytes"));
    Su700Control result;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 370U) {
        const auto active = bytes[offset + 369U];
        if (active == std::byte{})
            continue;
        if (active != std::byte{1})
            return std::unexpected(invalid("Unsupported SU700 song state"));
        auto name = name_at(bytes, offset);
        if (!name)
            return std::unexpected(name.error());
        Su700Song song{std::move(*name), {}};
        for (std::size_t slot = 1; slot <= 40U; ++slot) {
            const auto position = offset + slot * 9U;
            if (bytes[position] == std::byte{})
                continue;
            auto sample = name_at(bytes, position);
            if (!sample)
                return std::unexpected(sample.error());
            song.samples.push_back(std::move(*sample));
        }
        result.songs.push_back(std::move(song));
    }
    return result;
}

Result<Su700FloppyInspection> inspect_su700_floppy(const FatImage &image, const CancellationToken &cancel) {
    Su700FloppyInspection result;
    const auto control_file =
        std::ranges::find_if(image.files(), [](const auto &file) { return folded(file.path) == "SONGCONT.DAT"; });
    if (control_file == image.files().end())
        return result;
    const auto blocked = [&](std::string issue) -> Result<Su700FloppyInspection> {
        result.status = Su700ImportStatus::unsupported;
        result.issue = std::move(issue);
        result.files.clear();
        return result;
    };
    if (image.geometry().profile != FatProfile::a_series_floppy || image.geometry().boot_offset != 0 ||
        !image.directories().empty() || image.yamaha_catalog())
        return blocked("Choose a flat SU700 FAT12 floppy, not a disk set or another device's image.");
    if (control_file->size != 7400U)
        return blocked("SONGCONT.DAT is incomplete or unsupported.");
    auto control_bytes = image.read_file(*control_file, cancel);
    if (!control_bytes)
        return std::unexpected(control_bytes.error());
    auto control = decode_su700_control(*control_bytes);
    if (!control)
        return blocked(control.error().message);
    if (control->songs.empty())
        return blocked("The floppy contains no active SU700 songs.");
    std::map<std::string, const FatFile *> sources;
    for (const auto &file : image.files()) {
        if (!sources.emplace(folded(file.path), &file).second)
            return blocked("Duplicate floppy filenames.");
    }
    std::map<std::string, std::vector<std::string>> used;
    result.files.push_back({control_file->path, {"SONGCONT.DAT"}, control_file->size, Su700ImportRole::control});
    used.emplace(folded(control_file->path), std::vector<std::string>{"SONGCONT.DAT"});
    const auto add = [&](const std::string &name, bool song) -> Result<void> {
        const std::string extension = song ? ".SSQ" : ".SSP";
        const auto source = folded(trimmed(name) + extension);
        const std::vector<std::string> destination{song ? "SUSQ" : "SUSP", name + extension};
        const auto found = sources.find(source);
        if (found == sources.end())
            return std::unexpected(invalid("Missing " + source + "; incomplete disk sets cannot be imported."));
        if (const auto existing = used.find(source); existing != used.end()) {
            if (existing->second != destination)
                return std::unexpected(invalid("Ambiguous SU700 reference: " + source));
            return {};
        }
        auto valid = complete_payload(image, *found->second, song, cancel);
        if (!valid)
            return std::unexpected(valid.error());
        if (!*valid)
            return std::unexpected(invalid("Incomplete or unsupported payload: " + source));
        used.emplace(source, destination);
        result.files.push_back({found->second->path, destination, found->second->size,
                                song ? Su700ImportRole::song : Su700ImportRole::sample});
        if (song)
            ++result.song_count;
        else
            ++result.sample_count;
        return {};
    };
    for (const auto &song : control->songs) {
        if (auto checked = cancel.check(); !checked)
            return std::unexpected(checked.error());
        if (auto added = add(song.name, true); !added) {
            if (added.error().code == ErrorCode::operation_cancelled)
                return std::unexpected(added.error());
            return blocked(added.error().message);
        }
        for (const auto &sample : song.samples) {
            if (auto added = add(sample, false); !added) {
                if (added.error().code == ErrorCode::operation_cancelled)
                    return std::unexpected(added.error());
                return blocked(added.error().message);
            }
        }
    }
    for (const auto &file : image.files())
        if (!used.contains(folded(file.path)))
            result.files.push_back({file.path, {file.name}, file.size, Su700ImportRole::extra});
    result.suggested_volume_name = trimmed(control->songs.front().name);
    result.status = Su700ImportStatus::complete;
    return result;
}
} // namespace axk
