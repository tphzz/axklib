#include "support.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <unordered_set>
#include <utility>

#include "axklib/object.hpp"
#include "axklib/utf8.hpp"
#include "image_sessions_internal.hpp"

namespace axk::app::formats::a_series {
using image_sessions_internal::list_bounded_directory;

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
    const axk::AxkObjectDirectory &primary, const std::vector<std::shared_ptr<ImageSourceMember>> &companions,
    std::vector<axk::AxkObjectDirectoryEntry> &entries, const CancellationToken &cancellation) {
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
    for (std::size_t directory_index = 0U; directory_index < companions.size(); ++directory_index) {
        const auto &companion = *companions[directory_index];
        bool matched{};
        for (const auto &root : companion.roots)
            for (const auto &candidate : root.entries) {
                if (auto checked = cancellation.check(); !checked)
                    return std::unexpected(core_error(checked.error(), companion.reference));
                if (!candidate.reader || candidate.path.find('/') != std::string::npos)
                    continue;
                if (candidate.reader->size() > axk::AxkObjectDirectory::maximum_payload_bytes) {
                    continue;
                }
                const auto &reader = candidate.reader;
                bool required_missing_name{};
                bool required_continuation{};
                constexpr std::size_t object_header_bytes = 0x42U;
                if (reader->size() >= object_header_bytes) {
                    std::array<std::byte, object_header_bytes> prefix{};
                    const auto read = reader->read_exact_at(0U, prefix);
                    if (read) {
                        const auto header = axk::decode_object_header(prefix);
                        if (header && header->type == axk::ObjectType::smpl) {
                            required_missing_name = missing_names.contains(header->name);
                            const auto header_size = static_cast<std::size_t>(header->header_size);
                            if (incomplete_identity_sizes.contains(header_size) && header_size <= reader->size()) {
                                std::vector<std::byte> candidate_header(header_size);
                                if (const auto header_read = reader->read_exact_at(0U, candidate_header); header_read) {
                                    const auto identity = normalized_segment_header_identity(candidate_header);
                                    required_continuation = identity && incomplete_identities.contains(*identity);
                                }
                            }
                        }
                    }
                }
                if (!required_continuation && !required_missing_name)
                    continue;
                auto decoded = axk::StandaloneObject::open(
                    reader, candidate.path, static_cast<std::size_t>(axk::AxkObjectDirectory::maximum_payload_bytes));
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
                entries.push_back({std::format("companion-{}/{}", directory_index, candidate.path), reader,
                                   MediaFileLocation{companion.id, candidate.path}});
                matched = true;
            }
        if (matched)
            result.sources.push_back(companion.reference);
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

} // namespace axk::app::formats::a_series
