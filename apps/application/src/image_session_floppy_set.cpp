#include "image_session_floppy_set.hpp"
#include "image_sessions_internal.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <iterator>
#include <ranges>
#include <utility>

#include "axklib/utf8.hpp"

namespace axk::app::image_sessions_internal {
namespace {

std::string marker_name(axk::FloppySetMarker marker) {
    switch (marker) {
    case axk::FloppySetMarker::continuation:
        return "CONTINUATION";
    case axk::FloppySetMarker::final:
        return "FINAL";
    case axk::FloppySetMarker::invalid:
        return "INVALID";
    case axk::FloppySetMarker::none:
        return "NONE";
    }
    return "NONE";
}

ImageFloppySetMember member_summary(const axk::FatImage &member) {
    const auto &identity = member.disk_identity();
    return {.index = identity.index, .label = identity.label, .marker = marker_name(identity.marker)};
}

ImageFloppySetSummary single_summary(const axk::FatImage &image) {
    const auto &identity = image.disk_identity();
    ImageFloppySetSummary result;
    result.set_label = identity.trusted_for_disk_set ? identity.set_name : identity.label;
    result.members.push_back(member_summary(image));
    if (!identity.trusted_for_disk_set || identity.marker == axk::FloppySetMarker::none) {
        result.status = ImageFloppySetStatus::single;
    } else if (identity.marker == axk::FloppySetMarker::continuation || identity.index != 1U) {
        result.status = ImageFloppySetStatus::incomplete;
        if (identity.marker == axk::FloppySetMarker::continuation)
            result.next_required_index = static_cast<std::uint16_t>(identity.index + 1U);
    } else {
        result.status = ImageFloppySetStatus::complete;
    }
    return result;
}

ImageFloppySetSummary set_summary(const axk::FloppyDiskSet &set) {
    ImageFloppySetSummary result;
    result.status = set.status() == axk::FloppySetStatus::complete ? ImageFloppySetStatus::complete
                                                                   : ImageFloppySetStatus::incomplete;
    result.next_required_index = set.next_required_index();
    if (!set.members().empty())
        result.set_label = set.members().front().disk_identity().set_name;
    result.members.reserve(set.members().size());
    std::ranges::transform(set.members(), std::back_inserter(result.members), member_summary);
    return result;
}

bool has_floppy_extension(std::string_view name) {
    const auto path = axk::text::path_from_utf8(name);
    if (!path)
        return false;
    auto extension = axk::text::path_to_utf8(path->extension());
    std::ranges::transform(extension, extension.begin(), [](char value) {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    });
    return extension == ".ima" || extension == ".img";
}

} // namespace

Result<OpenedFloppySource> open_floppy_source(const Sandbox &sandbox, const ImageSourceRef &source,
                                              const SandboxFile &primary, MediaContainer media,
                                              const std::vector<ImageSourceRef> &companion_sources,
                                              PathReservationCoordinator *path_reservations,
                                              const CancellationToken &cancellation) {
    if (const auto *archive_set = std::get_if<axk::FloppyDiskSet>(&media.storage())) {
        if (!companion_sources.empty()) {
            return std::unexpected(
                session_error("companion_sources_unsupported", "a packaged floppy disk set is already self-contained"));
        }
        auto summary = set_summary(*archive_set);
        return OpenedFloppySource{.media = std::move(media),
                                  .companion_sources = {},
                                  .summary = std::move(summary),
                                  .verify_unchanged = primary.verify_unchanged,
                                  .companion_path_lease = {}};
    }

    const auto *primary_fat = std::get_if<axk::FatImage>(&media.storage());
    if (primary_fat == nullptr) {
        if (!companion_sources.empty()) {
            return std::unexpected(session_error("companion_sources_unsupported",
                                                 "companion image files can only be attached to a FAT12 floppy"));
        }
        return std::unexpected(session_error("companion_sources_unsupported", "the image is not a FAT12 floppy"));
    }
    if (companion_sources.empty()) {
        auto summary = single_summary(*primary_fat);
        return OpenedFloppySource{.media = std::move(media),
                                  .companion_sources = {},
                                  .summary = std::move(summary),
                                  .verify_unchanged = primary.verify_unchanged,
                                  .companion_path_lease = {}};
    }

    std::vector<PathAccess> accesses;
    accesses.reserve(companion_sources.size());
    for (const auto &candidate : companion_sources) {
        if (candidate.kind != ImageSourceKind::file) {
            return std::unexpected(
                session_error("invalid_companion_sources", "a raw floppy set requires companion image files"));
        }
        accesses.push_back({{candidate.root_id, candidate.relative_path}, PathAccessMode::shared});
    }
    PathReservationCoordinator::Lease companion_path_lease;
    if (path_reservations != nullptr) {
        auto acquired = path_reservations->try_acquire(accesses);
        if (!acquired)
            return std::unexpected(acquired.error());
        companion_path_lease = std::move(*acquired);
    }

    std::vector<axk::FatImage> members{*primary_fat};
    std::vector<std::function<Result<void>()>> verifiers{primary.verify_unchanged};
    members.reserve(companion_sources.size() + 1U);
    verifiers.reserve(companion_sources.size() + 1U);
    for (const auto &candidate : companion_sources) {
        auto opened = sandbox.open_file({candidate.root_id, candidate.relative_path});
        if (!opened)
            return std::unexpected(opened.error());
        auto member = axk::FatImage::open(opened->reader, opened->filename, cancellation);
        if (!member)
            return std::unexpected(core_error(member.error(), candidate));
        members.push_back(std::move(*member));
        verifiers.push_back(std::move(opened->verify_unchanged));
    }
    auto set = axk::FloppyDiskSet::open(std::move(members), source.relative_path, cancellation);
    if (!set)
        return std::unexpected(core_error(set.error(), source));
    auto summary = set_summary(*set);
    auto verify = [verifiers = std::move(verifiers)]() -> Result<void> {
        for (const auto &verifier : verifiers) {
            if (auto unchanged = verifier(); !unchanged)
                return std::unexpected(unchanged.error());
        }
        return {};
    };
    return OpenedFloppySource{.media = MediaContainer{std::move(*set)},
                              .companion_sources = companion_sources,
                              .summary = std::move(summary),
                              .verify_unchanged = std::move(verify),
                              .companion_path_lease = std::move(companion_path_lease)};
}

Result<std::vector<ImageSourceRef>> immediate_sibling_floppy_sources(const Sandbox &sandbox,
                                                                     const ImageSourceRef &source,
                                                                     std::string_view set_label,
                                                                     const CancellationToken &cancellation) {
    const auto source_path = axk::text::path_from_utf8(source.relative_path);
    if (!source_path || source_path->filename().empty())
        return std::vector<ImageSourceRef>{};
    const auto parent = axk::text::path_to_utf8(source_path->parent_path());
    auto listing = list_bounded_directory(sandbox, {source.root_id, parent}, 1024U);
    if (!listing)
        return std::unexpected(listing.error());

    std::vector<ImageSourceRef> result;
    for (const auto &entry : *listing) {
        if (entry.kind != DirectoryEntryKind::file || entry.relative_path == source.relative_path ||
            !has_floppy_extension(entry.name)) {
            continue;
        }
        ImageSourceRef candidate{source.root_id, entry.relative_path, ImageSourceKind::file};
        auto opened = sandbox.open_file({candidate.root_id, candidate.relative_path});
        if (!opened)
            return std::unexpected(opened.error());
        auto image = axk::FatImage::open(opened->reader, opened->filename, cancellation);
        if (!image)
            continue;
        const auto &identity = image->disk_identity();
        if (identity.trusted_for_disk_set && identity.set_name == set_label)
            result.push_back(std::move(candidate));
    }
    return result;
}

} // namespace axk::app::image_sessions_internal
