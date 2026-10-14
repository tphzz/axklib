#include "axklib/conversion/tx16w_to_a_series/source.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

#include "axklib/media.hpp"

namespace axk::tx16w {
namespace {

Error parse_error(ErrorCode code, std::string message, std::string_view source_name = {}) {
    ErrorContext context;
    if (!source_name.empty())
        context.source_path = std::string{source_name};
    return make_error(code, ErrorCategory::object, std::move(message), std::move(context));
}

std::string uppercase(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
}

bool same_file(const SourceFile &left, const SourceFile &right) { return left.bytes == right.bytes; }

Result<void> assign_member(const SourceFile *&destination, const SourceFile &source, std::string_view identity) {
    if (destination == nullptr) {
        destination = &source;
        return {};
    }
    if (same_file(*destination, source))
        return {};
    return std::unexpected{parse_error(ErrorCode::object_malformed,
                                       "TX16W disk set contains conflicting files for " + std::string{identity},
                                       source.name)};
}

struct NativeFileGroup {
    std::string name;
    const SourceFile *setup{};
    const SourceFile *voices{};
    const SourceFile *performances{};
};

} // namespace

Result<Inspection> inspect_files(std::span<const SourceFile> files, const CancellationToken &cancellation) {
    Inspection result;
    std::map<std::string, NativeFileGroup, std::less<>> groups;
    for (const auto &file : files) {
        if (const auto cancelled = cancellation.check(); !cancelled)
            return std::unexpected{cancelled.error()};
        const auto identity = identify_file(file.name);
        if (!identity)
            continue;
        if (identity->kind == 'W') {
            const auto duplicate = std::ranges::find_if(
                result.waves, [&](const Wave &wave) { return uppercase(wave.name) == uppercase(file.name); });
            if (duplicate != result.waves.end()) {
                const auto existing = std::ranges::find_if(
                    files, [&](const SourceFile &candidate) { return candidate.name == duplicate->name; });
                if (existing != files.end() && same_file(*existing, file))
                    continue;
                return std::unexpected{parse_error(ErrorCode::object_malformed,
                                                   "TX16W disk set contains conflicting Wave files", file.name)};
            }
            auto wave = decode_wave(file.bytes, file.name);
            if (!wave)
                return std::unexpected{wave.error()};
            if (wave->sample_rate_source == SampleRateSource::defaulted_33333) {
                result.notices.push_back(
                    {ParseNoticeDisposition::defaulted, wave->name, "sample_rate_encoding",
                     "Unknown TX16W sample-rate encoding defaults to 33,333 Hz; verify pitch after import"});
            }
            wave->source_member = file.source_member;
            result.waves.push_back(std::move(*wave));
            continue;
        }
        auto &group = groups[identity->set_key];
        group.name = identity->set_name;
        if (identity->kind == 'S') {
            if (auto assigned = assign_member(group.setup, file, identity->set_key + " Setup"); !assigned)
                return std::unexpected{assigned.error()};
        } else if (identity->kind == 'V') {
            if (auto assigned = assign_member(group.voices, file, identity->set_key + " Voice"); !assigned)
                return std::unexpected{assigned.error()};
        } else if (identity->kind == 'U') {
            if (auto assigned = assign_member(group.performances, file, identity->set_key + " Performance");
                !assigned) {
                return std::unexpected{assigned.error()};
            }
        } else if (identity->kind == 'F')
            result.unsupported_files.push_back(file.name);
        else if (identity->kind == 'P' || identity->kind == 'O' || identity->kind == 'C' || identity->kind == 'R' ||
                 identity->kind == 'X')
            result.unsupported_files.push_back(file.name);
    }
    if (!result.unsupported_files.empty())
        result.profile = Profile::yamaha_native_with_auxiliary_files;
    for (const auto &[key, group] : groups) {
        static_cast<void>(key);
        if (group.setup == nullptr && group.voices == nullptr)
            continue;
        if (group.setup == nullptr || group.voices == nullptr) {
            return std::unexpected{parse_error(ErrorCode::object_missing,
                                               "TX16W native set requires matching Setup (.Snn) and Voice (.Vnn) files",
                                               group.name)};
        }
        const std::span<const std::byte> performance_bytes =
            group.performances == nullptr ? std::span<const std::byte>{} : std::span{group.performances->bytes};
        auto setup = decode_native_setup(group.setup->bytes, group.voices->bytes, performance_bytes, group.name);
        if (!setup)
            return std::unexpected{setup.error()};
        result.setups.push_back(std::move(*setup));
    }
    if (result.setups.empty() && result.waves.empty()) {
        return std::unexpected{parse_error(ErrorCode::container_unrecognized,
                                           "FAT image contains no Yamaha TX16W native Setup or Wave files")};
    }
    return result;
}

Result<Inspection> inspect_disk(const FatImage &disk, const CancellationToken &cancellation) {
    std::vector<SourceFile> files;
    files.reserve(disk.files().size());
    for (const auto &file : disk.files()) {
        if (const auto cancelled = cancellation.check(); !cancelled)
            return std::unexpected{cancelled.error()};
        const auto identity = identify_file(file.path);
        if (!identity)
            continue;
        auto bytes = disk.read_file(file, cancellation);
        if (!bytes)
            return std::unexpected{bytes.error()};
        files.push_back({file.path, std::move(*bytes), disk.source_name()});
    }
    return inspect_files(files, cancellation);
}

} // namespace axk::tx16w
