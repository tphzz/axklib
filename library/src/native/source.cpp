#include "axklib/native/source.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace axk::native {
namespace {

Error source_error(std::string message, ErrorCode code = ErrorCode::invalid_argument) {
    return make_error(code, ErrorCategory::io, std::move(message));
}

} // namespace

SourceSet::SourceSet(std::vector<SourceMember> members) : members_(std::move(members)) {}

Result<std::shared_ptr<const SourceSet>> SourceSet::create(std::vector<SourceMember> members) {
    if (members.empty() || members.size() > maximum_members)
        return std::unexpected(source_error("A source set requires between one and 32 explicit members"));
    std::set<std::string> member_ids;
    std::size_t entry_count{};
    for (const auto &member : members) {
        if (member.id.empty() || (!member.reader && member.roots.empty()) || !member_ids.insert(member.id).second)
            return std::unexpected(source_error("Source member identity or reader is missing or duplicated"));
        std::set<std::string> root_ids;
        for (const auto &root : member.roots) {
            if (root.id.empty() || root.kind.empty() || !root_ids.insert(root.id).second)
                return std::unexpected(source_error("Filesystem root identity is missing or duplicated"));
            if (root.entries.size() > maximum_entries - entry_count)
                return std::unexpected(source_error("Source set exceeds its bounded filesystem entry count"));
            entry_count += root.entries.size();
            std::set<std::string> entry_ids;
            for (const auto &entry : root.entries) {
                if (entry.id.empty() || !entry_ids.insert(entry.id).second || (!entry.directory && !entry.reader))
                    return std::unexpected(
                        source_error("Filesystem entry identity or reader is missing or duplicated"));
            }
        }
    }
    return std::shared_ptr<const SourceSet>{new SourceSet{std::move(members)}};
}

std::span<const SourceMember> SourceSet::members() const noexcept { return members_; }

Result<std::shared_ptr<const RandomAccessReader>> SourceSet::reader(const SourceLocation &location) const {
    const auto member = std::ranges::find(members_, location.member_id, &SourceMember::id);
    if (member == members_.end())
        return std::unexpected(source_error("Source member does not exist"));
    if (location.root_id.empty() && location.entry_id.empty() && member->reader)
        return member->reader;
    const auto root = std::ranges::find(member->roots, location.root_id, &filesystems::Root::id);
    if (root == member->roots.end())
        return std::unexpected(source_error("Filesystem root does not exist"));
    const auto entry = std::ranges::find(root->entries, location.entry_id, &filesystems::Entry::id);
    if (entry == root->entries.end() || entry->directory || !entry->reader)
        return std::unexpected(source_error("Source file does not exist"));
    return entry->reader;
}

Result<SourceLocation> SourceSet::locate(std::string_view member_id, std::string_view root_id,
                                         std::string_view entry_id) const {
    SourceLocation location{std::string{member_id}, std::string{root_id}, std::string{entry_id}, 0U, 0U};
    auto source = reader(location);
    if (!source)
        return std::unexpected(source.error());
    location.length = (*source)->size();
    return location;
}

Result<std::vector<std::byte>> SourceSet::read(const SourceLocation &location,
                                               const CancellationToken &cancellation) const {
    if (const auto active = cancellation.check(); !active)
        return std::unexpected(active.error());
    const auto member = std::ranges::find(members_, location.member_id, &SourceMember::id);
    if (member == members_.end())
        return std::unexpected(source_error("Source member does not exist"));
    if (member->verify_unchanged) {
        if (const auto verified = member->verify_unchanged(); !verified)
            return std::unexpected(verified.error());
    }
    auto source = reader(location);
    if (!source)
        return std::unexpected(source.error());
    if (location.offset > (*source)->size() || location.length > (*source)->size() - location.offset)
        return std::unexpected(source_error("Read extends beyond its source file", ErrorCode::out_of_bounds));
    if (location.length > maximum_read_bytes)
        return std::unexpected(
            source_error("Read exceeds the source materialization limit", ErrorCode::io_unsupported_size));
    std::vector<std::byte> bytes(static_cast<std::size_t>(location.length));
    for (std::size_t offset = 0U; offset < bytes.size();) {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        const auto count = std::min<std::size_t>(64U * 1024U, bytes.size() - offset);
        auto read = (*source)->read_exact_at(location.offset + offset, std::span{bytes}.subspan(offset, count));
        if (!read)
            return std::unexpected(read.error());
        offset += count;
    }
    if (const auto active = cancellation.check(); !active)
        return std::unexpected(active.error());
    if (member->verify_unchanged) {
        if (const auto verified = member->verify_unchanged(); !verified)
            return std::unexpected(verified.error());
    }
    return bytes;
}

Result<void> SourceSet::verify(const CancellationToken &cancellation) const {
    for (const auto &member : members_) {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        if (member.verify_unchanged) {
            if (const auto verified = member.verify_unchanged(); !verified)
                return verified;
        }
    }
    return {};
}

} // namespace axk::native
