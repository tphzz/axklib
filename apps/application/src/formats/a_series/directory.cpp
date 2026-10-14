#include "formats/a_series/directory.hpp"
#include "formats/a_series/support.hpp"
#include "image_sessions_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace axk::app::formats::a_series {
namespace {
struct LoadedDirectory {
    AxkObjectDirectory directory;
    std::vector<AxkObjectDirectoryEntry> entries;
};

Result<LoadedDirectory> load_directory(const ImageSourceMember &member, const CancellationToken &cancellation) {
    const auto &source = member.reference;
    if (source.kind != ImageSourceKind::directory)
        return std::unexpected(session_error("invalid_companion_sources", "Select extracted companion disk folders"));
    LoadedDirectory result;
    for (const auto &root : member.roots)
        for (const auto &entry : root.entries) {
            if (auto checked = cancellation.check(); !checked)
                return std::unexpected(core_error(checked.error(), source));
            if (!entry.reader)
                continue;
            result.entries.push_back({entry.path, entry.reader, MediaFileLocation{member.id, entry.path}});
        }
    auto directory = AxkObjectDirectory::open(result.entries, source.relative_path, cancellation);
    if (!directory && directory.error().code == ErrorCode::container_unrecognized)
        return std::unexpected(session_error("native_format_unrecognized", directory.error().message));
    if (!directory)
        return std::unexpected(core_error(directory.error(), source));
    result.directory = std::move(*directory);
    return result;
}

std::string marker_name(FloppySetMarker marker) {
    switch (marker) {
    case FloppySetMarker::ordinary:
        return "ORDINARY";
    case FloppySetMarker::continuation:
        return "CONTINUATION";
    case FloppySetMarker::final:
        return "FINAL";
    case FloppySetMarker::invalid:
        return "INVALID";
    case FloppySetMarker::none:
        return "NONE";
    }
    return "NONE";
}

ImageFloppySetSummary directory_summary(const AxkObjectDirectory &directory, const ImageSourceRef &source,
                                        const std::vector<ImageSourceRef> &companions) {
    ImageFloppySetSummary result;
    const auto &identity = directory.disk_identity();
    if (identity.trusted_for_disk_set) {
        result.set_label = identity.set_name;
        for (const auto &member : directory.disk_members())
            result.members.push_back({member.index, member.label, marker_name(member.marker)});
        const auto &last = result.members.back();
        result.status = identity.index == 1U && last.marker == "FINAL" ? ImageFloppySetStatus::complete
                                                                       : ImageFloppySetStatus::incomplete;
        if (result.status == ImageFloppySetStatus::incomplete)
            result.next_required_index = identity.index != 1U ? 1U : static_cast<std::uint16_t>(last.index + 1U);
    } else if (!directory.disk_members().empty() && directory.validation_issues().empty() &&
               identity.marker == FloppySetMarker::ordinary) {
        result.status = ImageFloppySetStatus::single;
        result.set_label = identity.label;
        result.members.push_back({identity.index, identity.label, "ORDINARY"});
    } else {
        result.status = ImageFloppySetStatus::recovery;
        result.set_label = source.relative_path;
        result.members.push_back({1U, source.relative_path, "NONE"});
        for (std::size_t index = 0U; index < companions.size(); ++index)
            result.members.push_back({static_cast<std::uint16_t>(index + 2U), companions[index].relative_path, "NONE"});
    }
    return result;
}
} // namespace

Result<OpenedDirectorySource> open_directory_source(const ImageSourceMember &primary_source,
                                                    const std::vector<std::shared_ptr<ImageSourceMember>> &companions,
                                                    const CancellationToken &cancellation) {
    const auto &source = primary_source.reference;
    auto primary = load_directory(primary_source, cancellation);
    if (!primary)
        return std::unexpected(primary.error());
    auto directory = std::move(primary->directory);
    std::vector<ImageSourceRef> matched;
    if (directory.disk_identity().trusted_for_disk_set) {
        const bool assemble = !companions.empty() || directory.disk_identity().index == 1U;
        if (assemble) {
            if (companions.size() >= FloppyDiskSet::maximum_members)
                return std::unexpected(
                    session_error("invalid_companion_sources", "Select at most 31 companion folders"));
            std::vector<AxkObjectDirectory> members;
            members.push_back(std::move(directory));
            auto entry_count = primary->entries.size();
            std::uint64_t payload_bytes{};
            for (const auto &entry : primary->entries)
                payload_bytes += entry.reader->size();
            for (const auto &source_member : companions) {
                auto member = load_directory(*source_member, cancellation);
                if (!member) {
                    if (member.error().code != "native_format_unrecognized")
                        return std::unexpected(member.error());
                    continue;
                }
                if (!member->directory.disk_identity().trusted_for_disk_set ||
                    member->directory.disk_identity().set_name != members.front().disk_identity().set_name)
                    continue;
                if (member->entries.size() > AxkObjectDirectory::maximum_entries - entry_count)
                    return std::unexpected(
                        session_error("invalid_companion_sources", "Companion set exceeds the directory entry limit"));
                entry_count += member->entries.size();
                for (const auto &entry : member->entries) {
                    if (entry.reader->size() > AxkObjectDirectory::maximum_payload_bytes - payload_bytes)
                        return std::unexpected(session_error("invalid_companion_sources",
                                                             "Companion set exceeds the directory payload limit"));
                    payload_bytes += entry.reader->size();
                }
                members.push_back(std::move(member->directory));
                matched.push_back(source_member->reference);
            }
            // A later disk remains independently browseable until disk one is admitted.
            if (members.size() == 1U && members.front().disk_identity().index != 1U) {
                directory = std::move(members.front());
            } else {
                auto assembled =
                    AxkObjectDirectory::open_members(std::move(members), source.relative_path, cancellation);
                if (!assembled)
                    return std::unexpected(core_error(assembled.error(), source));
                directory = std::move(*assembled);
            }
        }
    } else {
        auto recovered = append_required_companion_wave_data(directory, companions, primary->entries, cancellation);
        if (!recovered)
            return std::unexpected(recovered.error());
        matched = std::move(recovered->sources);
        if (!matched.empty()) {
            auto reopened = AxkObjectDirectory::open(std::move(primary->entries), source.relative_path, cancellation);
            if (!reopened)
                return std::unexpected(core_error(reopened.error(), source));
            directory = std::move(*reopened);
        }
    }
    std::vector<std::byte> snapshot;
    for (const auto &object : directory.stored_objects()) {
        for (const char ch : object.logical_path)
            snapshot.push_back(static_cast<std::byte>(ch));
        snapshot.push_back(std::byte{});
        const auto size = static_cast<std::uint64_t>(object.raw_payload.size());
        for (std::size_t byte = 0U; byte < sizeof(size); ++byte)
            snapshot.push_back(static_cast<std::byte>((size >> (byte * 8U)) & 0xffU));
        snapshot.insert(snapshot.end(), object.raw_payload.begin(), object.raw_payload.end());
    }
    auto summary = directory_summary(directory, source, matched);
    return OpenedDirectorySource{{MediaContainer{std::move(directory)}, std::move(matched), std::move(summary)},
                                 std::make_shared<MemoryReader>(std::move(snapshot))};
}

Result<std::vector<ImageSourceRef>> sibling_directory_sources(const Sandbox &sandbox, const ImageSourceRef &source,
                                                              std::string_view set_label, bool cataloged,
                                                              const CancellationToken &cancellation) {
    auto siblings = immediate_sibling_directories(sandbox, source);
    if (!siblings)
        return std::unexpected(siblings.error());
    std::vector<ImageSourceRef> result;
    std::set<std::uint16_t> indices;
    if (cataloged && siblings->size() >= FloppyDiskSet::maximum_members)
        return std::unexpected(
            session_error("companion_scan_limit", "Too many nearby folders; select companion folders explicitly"));
    for (const auto &sibling : *siblings) {
        ImageSourceRef candidate{sibling.root_id, sibling.relative_path, ImageSourceKind::directory};
        if (cataloged) {
            auto admitted =
                admit_image_sources(sandbox, nullptr, nullptr, std::vector{candidate}, "companion-scan", cancellation);
            if (!admitted)
                return std::unexpected(admitted.error());
            auto loaded = load_directory(*admitted->members.front(), cancellation);
            if (!loaded) {
                if (loaded.error().code != "native_format_unrecognized")
                    return std::unexpected(loaded.error());
                continue;
            }
            const auto &identity = loaded->directory.disk_identity();
            if (!identity.trusted_for_disk_set || identity.set_name != set_label)
                continue;
            if (!indices.insert(identity.index).second)
                return std::unexpected(
                    session_error("companion_ambiguous",
                                  "Multiple nearby folders have the same disk index; select companions explicitly"));
        }
        result.push_back(std::move(candidate));
    }
    return result;
}
} // namespace axk::app::formats::a_series
