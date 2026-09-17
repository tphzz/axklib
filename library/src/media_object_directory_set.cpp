#include "axklib/media.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "axklib/floppy_catalog_internal.hpp"
#include "media_floppy_assembly.hpp"
#include "media_internal.hpp"

namespace axk {
const FloppyDiskIdentity &AxkObjectDirectory::disk_identity() const noexcept { return disk_identity_; }
std::span<const FloppyDiskIdentity> AxkObjectDirectory::disk_members() const noexcept { return disk_members_; }
std::span<const MediaValidationIssue> AxkObjectDirectory::validation_issues() const noexcept {
    return validation_issues_;
}

Result<AxkObjectDirectory> AxkObjectDirectory::open_members(std::vector<AxkObjectDirectory> members,
                                                            std::string source_name,
                                                            const CancellationToken &cancellation) {
    if (members.empty() || members.size() > FloppyDiskSet::maximum_members)
        return std::unexpected{detail::media_error(
            ErrorCode::invalid_argument, "directory disk set requires between one and 32 members", source_name)};
    std::ranges::sort(members, {}, [](const auto &member) { return member.disk_identity_.index; });
    AxkObjectDirectory result;
    result.source_name_ = source_name.empty() ? members.front().source_name_ : std::move(source_name);
    result.disk_identity_ = members.front().disk_identity_;
    std::vector<detail::FloppyPendingObject> pending;
    for (std::size_t index = 0U; index < members.size(); ++index) {
        if (auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        auto &member = members[index];
        const auto &identity = member.disk_identity_;
        if (!identity.trusted_for_disk_set || !member.catalog_ || member.disk_members_.size() != 1U ||
            identity.set_name != result.disk_identity_.set_name || identity.index != index + 1U ||
            (index + 1U < members.size() && identity.marker != FloppySetMarker::continuation))
            return std::unexpected{detail::media_error(
                ErrorCode::container_invalid_geometry,
                std::format("folder '{}' is not valid disk {} of this set", member.source_name_, index + 1U),
                result.source_name_)};
        if (member.source_bytes_ > maximum_payload_bytes - result.source_bytes_ ||
            member.source_entry_count_ > maximum_entries - result.source_entry_count_)
            return std::unexpected{detail::media_error(ErrorCode::io_unsupported_size,
                                                       "directory disk set exceeds its entry or payload limit",
                                                       result.source_name_)};
        result.source_bytes_ += member.source_bytes_;
        result.source_entry_count_ += member.source_entry_count_;
        result.disk_members_.push_back(identity);
        for (auto &object : member.objects_) {
            const auto slot = detail::yamaha_floppy_filename_slot(object.logical_path);
            if (!slot)
                return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                           "directory object has no catalog slot",
                                                           object.logical_path)};
            const auto entry = std::ranges::find(member.catalog_->files, *slot, &YamahaFloppyCatalogEntry::slot);
            if (entry == member.catalog_->files.end())
                return std::unexpected{detail::media_error(ErrorCode::container_invalid_geometry,
                                                           "directory object is not cataloged", object.logical_path)};
            object.logical_path = entry->logical_path;
            object.scope_key = std::format("axk-directory-set:{}", identity.set_name);
            object.raw_volume = identity.set_name;
            auto label = identity.set_name;
            while (!label.empty() && label.back() == ' ')
                label.pop_back();
            object.volume_label = {label.empty() ? "Floppy disk set" : label, LabelStatus::confirmed,
                                   "Yamaha floppy disk-set catalog label"};
            pending.push_back({std::move(object), *slot, entry->logical_path});
        }
    }
    const auto status = result.disk_members_.back().marker == FloppySetMarker::final ? FloppySetStatus::complete
                                                                                     : FloppySetStatus::incomplete;
    auto objects =
        detail::assemble_floppy_objects(std::move(pending), status, static_cast<std::size_t>(maximum_payload_bytes),
                                        result.source_name_, MediaObjectReadMode::complete);
    if (!objects)
        return std::unexpected{objects.error()};
    result.objects_ = std::move(*objects);
    return result;
}
} // namespace axk
