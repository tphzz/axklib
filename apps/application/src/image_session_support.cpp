#include "image_sessions_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "axklib/application/secure_random.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/lookups.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"
#include "axklib/utf8.hpp"

#include "content_digest.hpp"

namespace axk::app::image_sessions_internal {

axk::app::Error session_error(std::string code, std::string message, bool retryable) {
    return {std::move(code), std::move(message), {}, retryable};
}

axk::app::Error core_error(const axk::Error &error, const axk::app::ImageSourceRef &source) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    context.relative_path = source.relative_path;
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "image_open_failed",
            error.message, std::move(context), false};
}

axk::app::Result<std::string> random_identifier(std::string_view prefix) {
    auto suffix = axk::app::secure_random_hex(16U);
    if (!suffix)
        return std::unexpected(suffix.error());
    return std::string{prefix} + std::move(*suffix);
}

std::string media_kind_name(axk::MediaKind kind) {
    switch (kind) {
    case axk::MediaKind::sfs:
        return "sfs";
    case axk::MediaKind::fat12_floppy:
        return "fat12";
    case axk::MediaKind::fat12_floppy_set:
        return "fat12-set";
    case axk::MediaKind::iso9660:
        return "iso9660";
    case axk::MediaKind::standalone_object:
        return "standalone-object";
    case axk::MediaKind::axk_object_directory:
        return "axk-object-directory";
    }
    return "unknown";
}

std::string fold_ascii(std::string_view value) {
    std::string folded;
    folded.reserve(value.size());
    std::ranges::transform(value, std::back_inserter(folded), [](char character) {
        return character >= 'a' && character <= 'z' ? static_cast<char>(character - ('a' - 'A')) : character;
    });
    return folded;
}

std::optional<std::string> normalized_segment_header_identity(std::span<const std::byte> header) {
    if (header.size() < 0x28U)
        return std::nullopt;
    std::string identity(header.size(), '\0');
    std::ranges::transform(header, identity.begin(),
                           [](std::byte value) { return static_cast<char>(std::to_integer<unsigned char>(value)); });
    std::fill(identity.begin() + 0x20, identity.begin() + 0x28, '\0');
    return identity;
}

std::optional<std::string> incomplete_segment_identity(const axk::MediaObject &object) {
    const auto &header = object.decoded.header;
    if (header.type != axk::ObjectType::smpl ||
        (header.payload_offset_0x24 == 0U && header.payload_bytes_0x20 == header.payload_bytes_0x1c) ||
        header.header_size < 0x28U || header.header_size > object.raw_payload.size()) {
        return std::nullopt;
    }
    return normalized_segment_header_identity(std::span{object.raw_payload}.first(header.header_size));
}

axk::app::Result<std::vector<axk::app::DirectoryEntry>> list_bounded_directory(const axk::app::Sandbox &sandbox,
                                                                               const axk::app::DirectoryRef &reference,
                                                                               std::size_t maximum_entries) {
    std::vector<axk::app::DirectoryEntry> entries;
    std::optional<std::string> cursor;
    do {
        const auto remaining = maximum_entries + 1U - entries.size();
        const auto page_size = std::min<std::size_t>(remaining, 1000U);
        auto page = sandbox.list_directory(reference, page_size, cursor);
        if (!page)
            return std::unexpected(page.error());
        entries.insert(entries.end(), std::make_move_iterator(page->entries.begin()),
                       std::make_move_iterator(page->entries.end()));
        if (entries.size() > maximum_entries) {
            return std::unexpected(
                session_error("image_open_failed", "AXK object directory companion scan exceeds its entry limit"));
        }
        cursor = std::move(page->next_cursor);
    } while (cursor);
    return entries;
}

std::unordered_set<std::string> missing_required_wave_data_names(const axk::AxkObjectDirectory &primary) {
    std::unordered_set<std::string> available_names;
    for (const auto &object : primary.stored_objects()) {
        if (object.decoded.header.type == axk::ObjectType::smpl)
            available_names.insert(object.decoded.header.name);
    }

    std::unordered_set<std::string> missing_names;
    const auto add_missing = [&](std::string_view name) {
        if (!name.empty() && !available_names.contains(std::string{name}))
            missing_names.emplace(name);
    };
    for (const auto &object : primary.stored_objects()) {
        const auto *sample = std::get_if<axk::CurrentSbnk>(&object.decoded.payload);
        if (sample == nullptr)
            continue;
        add_missing(sample->left.wave_data_name);
        if (sample->right)
            add_missing(sample->right->wave_data_name);
    }
    return missing_names;
}

axk::app::Result<CompanionSegments> append_required_companion_wave_data(
    const axk::app::Sandbox &sandbox, const axk::app::ImageSourceRef &source, const axk::AxkObjectDirectory &primary,
    const std::vector<axk::app::ImageSourceRef> &companion_sources, std::vector<axk::AxkObjectDirectoryEntry> &entries,
    std::vector<std::function<axk::app::Result<void>()>> &verifiers) {
    std::unordered_set<std::string> incomplete_identities;
    std::unordered_set<std::size_t> incomplete_identity_sizes;
    for (const auto &object : primary.stored_objects()) {
        auto identity = incomplete_segment_identity(object);
        if (identity) {
            incomplete_identity_sizes.insert(identity->size());
            incomplete_identities.insert(std::move(*identity));
        }
    }
    const auto missing_names = missing_required_wave_data_names(primary);
    if (incomplete_identities.empty() && missing_names.empty())
        return CompanionSegments{};

    CompanionSegments result;
    for (std::size_t directory_index = 0U; directory_index < companion_sources.size(); ++directory_index) {
        const auto &companion = companion_sources[directory_index];
        if (companion.kind != axk::app::ImageSourceKind::axk_object_directory) {
            return std::unexpected(session_error("invalid_companion_sources",
                                                 "an extracted-object recovery requires companion directories"));
        }
        const axk::app::DirectoryRef directory{companion.root_id, companion.relative_path};
        if (directory.root_id == source.root_id && directory.relative_path == source.relative_path)
            continue;
        auto listing = list_bounded_directory(sandbox, directory, axk::AxkObjectDirectory::maximum_leaf_entries);
        if (!listing)
            return std::unexpected(listing.error());
        bool matched{};
        for (const auto &candidate : *listing) {
            if (candidate.kind != axk::app::DirectoryEntryKind::file)
                continue;
            if (candidate.size.value_or(axk::AxkObjectDirectory::maximum_payload_bytes + 1U) >
                axk::AxkObjectDirectory::maximum_payload_bytes) {
                continue;
            }
            const axk::app::FileRef reference{directory.root_id, candidate.relative_path};
            auto opened = sandbox.open_file(reference);
            if (!opened)
                return std::unexpected(opened.error());
            bool required_missing_name{};
            bool required_continuation{};
            constexpr std::size_t object_header_bytes = 0x42U;
            if (opened->reader->size() >= object_header_bytes) {
                std::array<std::byte, object_header_bytes> prefix{};
                const auto read = opened->reader->read_exact_at(0U, prefix);
                if (read) {
                    const auto header = axk::decode_object_header(prefix);
                    if (header && header->type == axk::ObjectType::smpl) {
                        required_missing_name = missing_names.contains(header->name);
                        const auto header_size = static_cast<std::size_t>(header->header_size);
                        if (incomplete_identity_sizes.contains(header_size) && header_size <= opened->reader->size()) {
                            std::vector<std::byte> candidate_header(header_size);
                            if (const auto header_read = opened->reader->read_exact_at(0U, candidate_header);
                                header_read) {
                                const auto identity = normalized_segment_header_identity(candidate_header);
                                required_continuation = identity && incomplete_identities.contains(*identity);
                            }
                        }
                    }
                }
            }
            if (!required_continuation && !required_missing_name)
                continue;
            auto decoded =
                axk::StandaloneObject::open(opened->reader, candidate.relative_path,
                                            static_cast<std::size_t>(axk::AxkObjectDirectory::maximum_payload_bytes));
            if (!decoded)
                continue;
            const auto identity = incomplete_segment_identity(decoded->object());
            const bool exact_continuation =
                required_continuation && identity && incomplete_identities.contains(*identity);
            const bool exact_missing_name = required_missing_name &&
                                            decoded->object().decoded.header.type == axk::ObjectType::smpl &&
                                            missing_names.contains(decoded->object().decoded.header.name);
            if (!exact_continuation && !exact_missing_name)
                continue;
            entries.push_back(
                {std::format("companion-{}/{}", directory_index, candidate.name), std::move(opened->reader)});
            verifiers.push_back(std::move(opened->verify_unchanged));
            result.files.push_back(reference);
            matched = true;
        }
        if (matched)
            result.sources.push_back(companion);
    }
    return result;
}

axk::app::Result<std::vector<axk::app::DirectoryRef>>
immediate_sibling_directories(const axk::app::Sandbox &sandbox, const axk::app::ImageSourceRef &source) {
    auto source_path = axk::text::path_from_utf8(source.relative_path);
    if (!source_path || source_path->filename().empty())
        return std::vector<axk::app::DirectoryRef>{};
    const auto parent_relative_path = axk::text::path_to_utf8(source_path->parent_path());
    auto siblings = list_bounded_directory(sandbox, {source.root_id, parent_relative_path},
                                           axk::AxkObjectDirectory::maximum_entries);
    if (!siblings)
        return std::unexpected(siblings.error());
    std::vector<axk::app::DirectoryRef> result;
    for (const auto &sibling : *siblings) {
        if (sibling.kind == axk::app::DirectoryEntryKind::directory && sibling.relative_path != source.relative_path)
            result.push_back({source.root_id, sibling.relative_path});
    }
    return result;
}

std::string object_format_name(axk::ObjectFormat format) {
    switch (format) {
    case axk::ObjectFormat::current:
        return "current";
    case axk::ObjectFormat::alternating_byte:
        return "alternating-byte";
    case axk::ObjectFormat::unknown:
        return "unknown";
    }
    return "unknown";
}

std::string object_type_name(axk::ObjectType type) {
    switch (type) {
    case axk::ObjectType::smpl:
        return "SMPL";
    case axk::ObjectType::sbnk:
        return "SBNK";
    case axk::ObjectType::sbac:
        return "SBAC";
    case axk::ObjectType::prog:
        return "PROG";
    case axk::ObjectType::sequ:
        return "SEQU";
    case axk::ObjectType::prf3:
        return "PRF3";
    case axk::ObjectType::unknown:
        return "UNKNOWN";
    }
    return "Unknown";
}

std::string relationship_quality_wire_name(axk::RelationshipQuality quality) {
    switch (quality) {
    case axk::RelationshipQuality::known:
        return "KNOWN";
    case axk::RelationshipQuality::likely:
        return "LIKELY";
    case axk::RelationshipQuality::tentative:
        return "TENTATIVE";
    case axk::RelationshipQuality::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::optional<std::string> mapped_id(const std::unordered_map<std::string, std::string> &ids, const std::string &key) {
    if (const auto found = ids.find(key); found != ids.end())
        return found->second;
    return std::nullopt;
}

std::optional<std::uint8_t> partition_index_from_node_id(std::string_view node_id) {
    constexpr std::string_view prefix = "partition:";
    if (!node_id.starts_with(prefix))
        return std::nullopt;
    node_id.remove_prefix(prefix.size());
    const auto separator = node_id.find(':');
    const auto value_text = node_id.substr(0U, separator);
    unsigned int value{};
    const auto parsed = std::from_chars(value_text.data(), value_text.data() + value_text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != value_text.data() + value_text.size() ||
        value > std::numeric_limits<std::uint8_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

std::uint64_t record_cluster_count(const axk::Container &container, const axk::ObjectSnapshot &object) {
    const auto partition = std::ranges::find(container.partitions(), object.partition, &axk::Partition::index);
    if (partition == container.partitions().end())
        return 0U;
    const auto record = std::ranges::find(partition->records, object.sfs_id, &axk::IndexRecord::sfs_id);
    if (record == partition->records.end())
        return 0U;
    std::uint64_t result = record->continuation_clusters.size();
    for (const auto &extent : record->extents)
        result += extent.cluster_count;
    return result;
}

} // namespace axk::app::image_sessions_internal
