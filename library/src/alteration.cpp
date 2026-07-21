#include "axklib/alteration.hpp"

#include "axklib/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "axklib/catalog.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/object.hpp"
#include "axklib/package.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/package_relocation.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"
#include "writer_internal.hpp"

namespace axk {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

Error transaction_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

void rename_json_field(Json &object, std::string_view old_name, std::string_view new_name) {
    if (!object.is_object() || !object.contains(old_name))
        return;
    object[std::string{new_name}] = std::move(object[std::string{old_name}]);
    object.erase(std::string{old_name});
}

void migrate_legacy_program(Json &program) {
    if (!program.is_object() || !program.contains("assignments") || !program["assignments"].is_array())
        return;
    for (auto &assignment : program["assignments"]) {
        rename_json_field(assignment, "sample_bank", "sample");
        rename_json_field(assignment, "sample_bank_group", "sample_bank");
    }
}

void migrate_legacy_volume(Json &volume) {
    rename_json_field(volume, "sample_banks", "samples");
    rename_json_field(volume, "sample_bank_groups", "sample_banks");
    if (volume.contains("sample_banks") && volume["sample_banks"].is_array()) {
        for (auto &sample_bank : volume["sample_banks"]) {
            rename_json_field(sample_bank, "member_sample_bank", "member_sample");
            rename_json_field(sample_bank, "member_sample_banks", "member_samples");
        }
    }
    if (volume.contains("programs") && volume["programs"].is_array()) {
        for (auto &program : volume["programs"])
            migrate_legacy_program(program);
    }
}

bool migrate_legacy_alteration_manifest(Json &root) {
    if (!root.is_object() || root.value("schema_version", "") != "1.0")
        return false;
    if (root.contains("operations") && root["operations"].is_array()) {
        for (auto &operation : root["operations"]) {
            const auto type = operation.value("type", "");
            if (type == "delete_sbnk") {
                rename_json_field(operation, "sample_bank_name", "sample_name");
            } else if (type == "insert_sbnk") {
                rename_json_field(operation, "sample_bank", "sample");
            } else if (type == "rename_sbnk") {
                rename_json_field(operation, "sample_bank_name", "sample_name");
                rename_json_field(operation, "new_sample_bank_name", "new_sample_name");
            } else if (type == "delete_sbac") {
                rename_json_field(operation, "sample_bank_group_name", "sample_bank_name");
            } else if (type == "insert_sbac") {
                rename_json_field(operation, "sample_bank_group", "sample_bank");
                if (operation.contains("sample_bank"))
                    rename_json_field(operation["sample_bank"], "member_sample_banks", "member_samples");
            } else if (type == "rename_sbac") {
                rename_json_field(operation, "sample_bank_group_name", "sample_bank_name");
                rename_json_field(operation, "new_sample_bank_group_name", "new_sample_bank_name");
            } else if (type == "insert_volume" && operation.contains("volume")) {
                migrate_legacy_volume(operation["volume"]);
            } else if (type == "insert_program" && operation.contains("program")) {
                migrate_legacy_program(operation["program"]);
            }
        }
    }
    root["schema_version"] = "1.1";
    return true;
}

struct MutablePartition {
    const Partition *source{};
    std::optional<std::string> renamed_name;
    std::vector<std::byte> bitmap;
    std::set<SfsId> deleted;
    std::optional<std::vector<std::byte>> root_payload;
    std::optional<std::vector<std::byte>> root_index;
    struct InsertedRecord {
        SfsId id;
        std::vector<std::byte> raw_index;
        std::vector<std::byte> payload;
        std::vector<Extent> extents;
        std::vector<std::uint32_t> continuation_clusters;
        PayloadKind payload_kind{PayloadKind::unknown};
    };
    std::map<SfsId, InsertedRecord> inserted;
    std::map<SfsId, InsertedRecord> changed;
};

struct TransactionState {
    Container container;
    ObjectCatalog catalog;
    RelationshipGraph graph;
    std::vector<std::tuple<PartitionIndex, SfsId, SfsId>> known_edges;
    std::map<std::uint8_t, MutablePartition> partitions;
    std::vector<OperationReport> reports;
};

bool requires_object_graph(const AlterationManifest &manifest) {
    return std::ranges::any_of(manifest.operations, [](const AlterationOperation &operation) {
        return !std::holds_alternative<InsertVolumeOperation>(operation.data) &&
               !std::holds_alternative<RenameVolumeOperation>(operation.data) &&
               !std::holds_alternative<RenamePartitionOperation>(operation.data);
    });
}

struct OperationView {
    std::string id;
    std::string type;
    PartitionSelector partition;
    std::string volume_name;
    std::string new_volume_name;
    std::string partition_name;
    std::string new_partition_name;
    std::optional<VolumeSpec> volume;
    std::string sample_name;
    std::optional<SampleSpec> sample;
    std::optional<InsertWaveformSpec> waveform;
    std::string waveform_name;
    std::string new_waveform_name;
    std::string new_sample_name;
    std::string sample_bank_name;
    std::string new_sample_bank_name;
    std::optional<SampleBankSpec> sample_bank;
    std::optional<std::uint8_t> program_number;
    std::optional<ProgramSpec> program;
};

OperationView operation_view(const AlterationOperation &operation) {
    OperationView result;
    result.id = operation.id;
    result.type = std::string{operation_type_name(operation.data)};
    std::visit(
        [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            result.partition = value.partition;
            if constexpr (std::same_as<T, DeleteVolumeOperation>) {
                result.volume_name = value.volume_name;
            } else if constexpr (std::same_as<T, InsertVolumeOperation>) {
                result.volume_name = value.volume.name;
                result.volume = value.volume;
            } else if constexpr (std::same_as<T, RenameVolumeOperation>) {
                result.volume_name = value.volume_name;
                result.new_volume_name = value.new_volume_name;
            } else if constexpr (std::same_as<T, RenamePartitionOperation>) {
                result.partition_name = value.partition_name;
                result.new_partition_name = value.new_partition_name;
            } else if constexpr (std::same_as<T, DeleteSampleOperation>) {
                result.volume_name = value.volume_name;
                result.sample_name = value.sample_name;
            } else if constexpr (std::same_as<T, InsertSampleOperation>) {
                result.volume_name = value.volume_name;
                result.sample_name = value.sample.name;
                result.sample = value.sample;
            } else if constexpr (std::same_as<T, InsertWaveformOperation>) {
                result.volume_name = value.volume_name;
                result.waveform = value.waveform;
            } else if constexpr (std::same_as<T, DeleteWaveformOperation>) {
                result.volume_name = value.volume_name;
                result.waveform_name = value.waveform_name;
            } else if constexpr (std::same_as<T, RenameWaveformOperation>) {
                result.volume_name = value.volume_name;
                result.waveform_name = value.waveform_name;
                result.new_waveform_name = value.new_waveform_name;
            } else if constexpr (std::same_as<T, RenameSampleOperation>) {
                result.volume_name = value.volume_name;
                result.sample_name = value.sample_name;
                result.new_sample_name = value.new_sample_name;
            } else if constexpr (std::same_as<T, DeleteSampleBankOperation>) {
                result.volume_name = value.volume_name;
                result.sample_bank_name = value.sample_bank_name;
            } else if constexpr (std::same_as<T, InsertSampleBankOperation>) {
                result.volume_name = value.volume_name;
                result.sample_bank_name = value.sample_bank.name;
                result.sample_bank = value.sample_bank;
            } else if constexpr (std::same_as<T, RenameSampleBankOperation>) {
                result.volume_name = value.volume_name;
                result.sample_bank_name = value.sample_bank_name;
                result.new_sample_bank_name = value.new_sample_bank_name;
            } else if constexpr (std::same_as<T, DeleteProgramOperation>) {
                result.volume_name = value.volume_name;
                result.program_number = value.program_number;
            } else if constexpr (std::same_as<T, InsertProgramOperation>) {
                result.volume_name = value.volume_name;
                result.program_number = value.program.number;
                result.program = value.program;
            }
        },
        operation.data);
    return result;
}

AlterationOperation typed_operation(OperationView value) {
    AlterationOperationData data;
    if (value.type == "delete_volume") {
        data = DeleteVolumeOperation{std::move(value.partition), std::move(value.volume_name)};
    } else if (value.type == "insert_volume") {
        data = InsertVolumeOperation{std::move(value.partition), std::move(*value.volume)};
    } else if (value.type == "rename_volume") {
        data = RenameVolumeOperation{std::move(value.partition), std::move(value.volume_name),
                                     std::move(value.new_volume_name)};
    } else if (value.type == "rename_partition") {
        data = RenamePartitionOperation{std::move(value.partition), std::move(value.partition_name),
                                        std::move(value.new_partition_name)};
    } else if (value.type == "delete_sbnk") {
        data = DeleteSampleOperation{std::move(value.partition), std::move(value.volume_name),
                                     std::move(value.sample_name)};
    } else if (value.type == "insert_sbnk") {
        data =
            InsertSampleOperation{std::move(value.partition), std::move(value.volume_name), std::move(*value.sample)};
    } else if (value.type == "insert_waveform") {
        data = InsertWaveformOperation{std::move(value.partition), std::move(value.volume_name),
                                       std::move(*value.waveform)};
    } else if (value.type == "delete_waveform") {
        data = DeleteWaveformOperation{std::move(value.partition), std::move(value.volume_name),
                                       std::move(value.waveform_name)};
    } else if (value.type == "rename_waveform") {
        data = RenameWaveformOperation{std::move(value.partition), std::move(value.volume_name),
                                       std::move(value.waveform_name), std::move(value.new_waveform_name)};
    } else if (value.type == "rename_sbnk") {
        data = RenameSampleOperation{std::move(value.partition), std::move(value.volume_name),
                                     std::move(value.sample_name), std::move(value.new_sample_name)};
    } else if (value.type == "delete_sbac") {
        data = DeleteSampleBankOperation{std::move(value.partition), std::move(value.volume_name),
                                         std::move(value.sample_bank_name)};
    } else if (value.type == "insert_sbac") {
        data = InsertSampleBankOperation{std::move(value.partition), std::move(value.volume_name),
                                         std::move(*value.sample_bank)};
    } else if (value.type == "rename_sbac") {
        data = RenameSampleBankOperation{std::move(value.partition), std::move(value.volume_name),
                                         std::move(value.sample_bank_name), std::move(value.new_sample_bank_name)};
    } else if (value.type == "delete_program") {
        data = DeleteProgramOperation{std::move(value.partition), std::move(value.volume_name), *value.program_number};
    } else {
        data =
            InsertProgramOperation{std::move(value.partition), std::move(value.volume_name), std::move(*value.program)};
    }
    return {std::move(value.id), std::move(data)};
}

Result<std::string> required_text(const Json &row, std::string_view field, std::string_view context) {
    if (!row.contains(field) || !row[field].is_string() || row[field].get_ref<const std::string &>().empty()) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must be a non-empty string")};
    }
    return row[field].get<std::string>();
}

Result<void> exact_fields(const Json &row, std::initializer_list<std::string_view> expected, std::string_view context) {
    if (!row.is_object() || row.size() != expected.size()) {
        return std::unexpected{transaction_error(std::string{context} + " has invalid fields")};
    }
    for (const auto field : expected) {
        if (!row.contains(field)) {
            return std::unexpected{transaction_error(std::string{context} + " is missing field " + std::string{field})};
        }
    }
    return {};
}

Result<std::uint8_t> midi_value(const Json &row, std::string_view field, std::string_view context,
                                std::uint8_t default_value, bool required) {
    if (!row.contains(field)) {
        if (!required)
            return default_value;
        return std::unexpected{transaction_error(std::string{context} + " is missing field " + std::string{field})};
    }
    if (!row[field].is_number_integer()) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must be an integer")};
    }
    const auto value = row[field].get<int>();
    if (value < 0 || value > 127) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must be between 0 and 127")};
    }
    return static_cast<std::uint8_t>(value);
}

Result<std::uint8_t> program_value(const Json &row, std::string_view field, std::string_view context) {
    if (!row.contains(field) || !row[field].is_number_integer()) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must be an integer")};
    }
    const auto value = row[field].get<int>();
    if (value < 1 || value > 128) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must be between 1 and 128")};
    }
    return static_cast<std::uint8_t>(value);
}

Result<std::string> object_name(const Json &row, std::string_view field, std::string_view context) {
    auto result = required_text(row, field, context);
    if (!result)
        return std::unexpected{result.error()};
    if (result->size() > 16U || !std::ranges::all_of(*result, [](unsigned char value) { return value < 0x80U; })) {
        return std::unexpected{
            transaction_error(std::string{context} + "." + std::string{field} + " must fit 16 ASCII bytes")};
    }
    return result;
}

Result<std::string> partition_name(const Json &row, std::string_view field, std::string_view context) {
    auto result = required_text(row, field, context);
    if (!result)
        return std::unexpected{result.error()};
    const auto printable =
        std::ranges::all_of(*result, [](unsigned char value) { return value >= 0x20U && value <= 0x7eU; });
    if (result->size() > 16U || !printable || result->front() == ' ' || result->back() == ' ') {
        return std::unexpected{transaction_error(std::string{context} + "." + std::string{field} +
                                                 " must be 1..16 printable ASCII characters without outer spaces")};
    }
    return result;
}

const IndexRecord *record(const Partition &partition, SfsId id) {
    const auto found = std::ranges::find(partition.records, id, &IndexRecord::sfs_id);
    return found == partition.records.end() ? nullptr : &*found;
}

const MutablePartition::InsertedRecord *current_record(const MutablePartition &partition, SfsId id) {
    if (partition.deleted.contains(id))
        return nullptr;
    if (const auto found = partition.inserted.find(id); found != partition.inserted.end()) {
        return &found->second;
    }
    if (const auto found = partition.changed.find(id); found != partition.changed.end()) {
        return &found->second;
    }
    return nullptr;
}

bool record_exists(const MutablePartition &partition, SfsId id) {
    return current_record(partition, id) != nullptr ||
           (!partition.deleted.contains(id) && record(*partition.source, id) != nullptr);
}

Result<std::vector<std::byte>> current_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                               const CancellationToken &cancellation) {
    if (id.value == 1U && partition.root_payload)
        return *partition.root_payload;
    if (const auto *item = current_record(partition, id); item != nullptr)
        return item->payload;
    if (partition.deleted.contains(id) || record(*partition.source, id) == nullptr) {
        return std::unexpected{transaction_error("SFS record does not exist in transaction state")};
    }
    return state.container.read_record_data(partition.source->index, id, 64U * 1024U * 1024U, cancellation);
}

PayloadKind current_payload_kind(const MutablePartition &partition, SfsId id) {
    if (const auto *item = current_record(partition, id); item != nullptr) {
        return item->payload_kind;
    }
    const auto *source = record(*partition.source, id);
    return source == nullptr ? PayloadKind::unknown : source->payload_kind;
}

struct ParsedDirectoryEntry {
    SfsId id;
    std::string name;
    std::size_t offset{};
};

std::string directory_name(std::span<const std::byte> bytes) {
    if (bytes.size() < 32U)
        return {};
    const auto declared = static_cast<std::size_t>((std::to_integer<std::uint16_t>(bytes[2]) << 8U) |
                                                   std::to_integer<std::uint16_t>(bytes[3]));
    const auto count = std::min<std::size_t>(declared, 24U);
    std::string result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = std::to_integer<unsigned char>(bytes[8U + index]);
        if (value == 0U)
            break;
        result.push_back(static_cast<char>(value));
    }
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result;
}

Result<std::vector<ParsedDirectoryEntry>> parse_directory(std::span<const std::byte> payload, SfsId id) {
    std::vector<ParsedDirectoryEntry> result;
    for (std::size_t offset = 0; offset + 32U <= payload.size(); offset += 32U) {
        const auto row = payload.subspan(offset, 32U);
        if (std::ranges::all_of(row.first(8U), [](std::byte value) { return value == std::byte{0}; })) {
            break;
        }
        const auto link = (std::to_integer<std::uint32_t>(row[4]) << 24U) |
                          (std::to_integer<std::uint32_t>(row[5]) << 16U) |
                          (std::to_integer<std::uint32_t>(row[6]) << 8U) | std::to_integer<std::uint32_t>(row[7]);
        result.push_back({SfsId{link}, directory_name(row), offset});
    }
    if (result.empty() || result.front().name != ".") {
        return std::unexpected{
            transaction_error("SFS ID " + std::to_string(id.value) + " is not a readable directory")};
    }
    return result;
}

Result<std::vector<std::byte>> read_raw(const std::filesystem::path &path, std::uint64_t offset, std::size_t size) {
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    std::vector<std::byte> result(size);
    if (auto read = (*reader)->read_exact_at(offset, result); !read)
        return std::unexpected{read.error()};
    return result;
}

void set_bitmap(std::vector<std::byte> &bitmap, std::uint32_t cluster, bool used) {
    const auto mask = static_cast<std::uint8_t>(0x80U >> (cluster % 8U));
    auto value = std::to_integer<std::uint8_t>(bitmap[cluster / 8U]);
    value = used ? static_cast<std::uint8_t>(value | mask)
                 : static_cast<std::uint8_t>(value & static_cast<std::uint8_t>(~mask));
    bitmap[cluster / 8U] = static_cast<std::byte>(value);
}

bool bitmap_used(const std::vector<std::byte> &bitmap, std::uint32_t cluster) {
    return (std::to_integer<std::uint8_t>(bitmap[cluster / 8U]) & (0x80U >> (cluster % 8U))) != 0U;
}

void put_be16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 1U] = static_cast<std::byte>(value);
}

void put_be32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::byte>(value >> 24U);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 16U);
    bytes[offset + 2U] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 3U] = static_cast<std::byte>(value);
}

Result<std::vector<std::byte>> current_root_payload(TransactionState &state, MutablePartition &partition,
                                                    const CancellationToken &cancellation) {
    if (partition.root_payload)
        return *partition.root_payload;
    return state.container.read_record_data(partition.source->index, SfsId{1}, 64U * 1024U, cancellation);
}

Result<void> set_root_payload(TransactionState &state, MutablePartition &partition, std::vector<std::byte> payload,
                              const CancellationToken &cancellation) {
    const auto *root = record(*partition.source, SfsId{1});
    if (root == nullptr || root->extents.size() != 1U || !root->continuation_clusters.empty() ||
        payload.size() > static_cast<std::size_t>(root->extents[0].cluster_count) * 1024U) {
        return std::unexpected{transaction_error("partition root relocation is not enabled for this transaction")};
    }
    Result<std::vector<std::byte>> raw = partition.root_index
                                             ? Result<std::vector<std::byte>>{*partition.root_index}
                                             : read_raw(state.container.source_path(), root->record_offset.value, 72);
    if (!raw)
        return std::unexpected{raw.error()};
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    const auto size = static_cast<std::uint32_t>(payload.size());
    put_be32(*raw, 6, size);
    put_be32(*raw, 0x12, size);
    const auto count = static_cast<std::uint16_t>(payload.size() / 32U - 2U);
    put_be16(*raw, 0x46, count);
    partition.root_payload = std::move(payload);
    partition.root_index = std::move(*raw);
    return {};
}

Result<void> replace_record_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                    std::vector<std::byte> payload, const CancellationToken &cancellation) {
    if (partition.deleted.contains(id)) {
        return std::unexpected{transaction_error("cannot change a deleted SFS record")};
    }
    MutablePartition::InsertedRecord *target{};
    if (const auto found = partition.inserted.find(id); found != partition.inserted.end()) {
        target = &found->second;
    } else if (const auto changed = partition.changed.find(id); changed != partition.changed.end()) {
        target = &changed->second;
    } else {
        const auto *source = record(*partition.source, id);
        if (source == nullptr) {
            return std::unexpected{transaction_error("cannot change a missing SFS record")};
        }
        auto raw = read_raw(state.container.source_path(), source->record_offset.value, 72U);
        if (!raw)
            return std::unexpected{raw.error()};
        auto original = current_payload(state, partition, id, cancellation);
        if (!original)
            return std::unexpected{original.error()};
        auto [inserted, unused] = partition.changed.emplace(
            id, MutablePartition::InsertedRecord{id, std::move(*raw), std::move(*original), source->extents,
                                                 source->continuation_clusters, source->payload_kind});
        static_cast<void>(unused);
        target = &inserted->second;
    }

    std::uint64_t capacity{};
    for (const auto &extent : target->extents) {
        capacity += static_cast<std::uint64_t>(extent.cluster_count) * 1024U;
    }
    if (payload.size() > capacity) {
        return std::unexpected{transaction_error("record payload growth exceeds its current extent capacity")};
    }
    put_be32(target->raw_index, 6U, static_cast<std::uint32_t>(payload.size()));
    if (target->extents.size() <= 4U) {
        std::uint32_t remaining = static_cast<std::uint32_t>(payload.size());
        for (std::size_t index = 0; index < target->extents.size(); ++index) {
            const auto capacity_for_extent = target->extents[index].cluster_count * 1024U;
            const auto byte_count = remaining == 0U ? capacity_for_extent : std::min(remaining, capacity_for_extent);
            remaining = remaining > byte_count ? remaining - byte_count : 0U;
            put_be32(target->raw_index, 0x12U + index * 12U, byte_count);
        }
    }
    if (target->extents.size() == 1U) {
        put_be32(target->raw_index, 0x12U, static_cast<std::uint32_t>(payload.size()));
    }
    if (id.value == 1U) {
        const auto entries = parse_directory(payload, id);
        if (!entries)
            return std::unexpected{entries.error()};
        put_be16(target->raw_index, 0x46U, static_cast<std::uint16_t>(entries->size() - 2U));
    }
    target->payload = std::move(payload);
    return {};
}

Result<SfsId> unique_directory_child(TransactionState &state, MutablePartition &partition, SfsId directory,
                                     std::string_view name, const CancellationToken &cancellation) {
    if (current_payload_kind(partition, directory) != PayloadKind::directory) {
        return std::unexpected{transaction_error("directory path resolves to a non-directory record")};
    }
    auto payload = current_payload(state, partition, directory, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, directory);
    if (!entries)
        return std::unexpected{entries.error()};
    std::vector<SfsId> matches;
    for (const auto &entry : *entries) {
        if (entry.name == name)
            matches.push_back(entry.id);
    }
    if (matches.size() != 1U) {
        return std::unexpected{transaction_error("directory requires exactly one entry named " + std::string{name})};
    }
    if (!record_exists(partition, matches.front())) {
        return std::unexpected{transaction_error("directory entry references a missing SFS record")};
    }
    return matches.front();
}

Result<SfsId> volume_category(TransactionState &state, MutablePartition &partition, std::string_view volume_name,
                              std::string_view category_name, const CancellationToken &cancellation) {
    auto volume = unique_directory_child(state, partition, SfsId{1}, volume_name, cancellation);
    if (!volume)
        return std::unexpected{volume.error()};
    return unique_directory_child(state, partition, *volume, category_name, cancellation);
}

Result<std::pair<SfsId, SfsId>> category_object(TransactionState &state, MutablePartition &partition,
                                                std::string_view volume_name, std::string_view category_name,
                                                std::string_view object_name, std::string_view expected_type,
                                                const CancellationToken &cancellation) {
    auto category = volume_category(state, partition, volume_name, category_name, cancellation);
    if (!category)
        return std::unexpected{category.error()};
    auto object = unique_directory_child(state, partition, *category, object_name, cancellation);
    if (!object)
        return std::unexpected{object.error()};
    auto payload = current_payload(state, partition, *object, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (payload->size() < 16U ||
        !std::equal(expected_type.begin(), expected_type.end(), payload->begin() + 12U, [](char left, std::byte right) {
            return static_cast<unsigned char>(left) == std::to_integer<unsigned char>(right);
        })) {
        return std::unexpected{transaction_error(std::string{object_name} + " does not resolve to one readable " +
                                                 std::string{expected_type} + " record")};
    }
    return std::pair{*category, *object};
}

struct CategoryObject {
    std::string name;
    SfsId id;
    std::vector<std::byte> payload;
    DecodedObject decoded;
};

Result<std::vector<CategoryObject>> category_objects(TransactionState &state, MutablePartition &partition,
                                                     std::string_view volume_name, std::string_view category_name,
                                                     ObjectType expected_type, const CancellationToken &cancellation) {
    auto directory = volume_category(state, partition, volume_name, category_name, cancellation);
    if (!directory)
        return std::unexpected{directory.error()};
    auto payload = current_payload(state, partition, *directory, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, *directory);
    if (!entries)
        return std::unexpected{entries.error()};
    std::vector<CategoryObject> result;
    for (const auto &entry : *entries) {
        if (entry.name == "." || entry.name == "..")
            continue;
        auto object_payload = current_payload(state, partition, entry.id, cancellation);
        if (!object_payload)
            return std::unexpected{object_payload.error()};
        auto decoded = decode_object(*object_payload);
        if (!decoded || decoded->header.type != expected_type) {
            return std::unexpected{transaction_error("category contains an unresolved or incorrectly typed object")};
        }
        result.push_back(CategoryObject{entry.name, entry.id, std::move(*object_payload), std::move(*decoded)});
    }
    return result;
}

Result<void> replace_fixed_object_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                          std::vector<std::byte> payload, const CancellationToken &cancellation) {
    auto current = current_payload(state, partition, id, cancellation);
    if (!current)
        return std::unexpected{current.error()};
    if (payload.size() != current->size()) {
        return std::unexpected{transaction_error("fixed-size object metadata update changed payload size")};
    }
    return replace_record_payload(state, partition, id, std::move(payload), cancellation);
}

Result<bool> sbnk_program_bit(std::span<const std::byte> payload, std::uint8_t program) {
    const auto offset = 0xc0U + static_cast<std::size_t>((program - 1U) / 32U) * 4U;
    if (payload.size() < offset + 4U) {
        return std::unexpected{transaction_error("SBNK payload is too short for its Program-link bitmap")};
    }
    const auto word = (std::to_integer<std::uint32_t>(payload[offset]) << 24U) |
                      (std::to_integer<std::uint32_t>(payload[offset + 1U]) << 16U) |
                      (std::to_integer<std::uint32_t>(payload[offset + 2U]) << 8U) |
                      std::to_integer<std::uint32_t>(payload[offset + 3U]);
    return (word & (std::uint32_t{1} << ((program - 1U) % 32U))) != 0U;
}

Result<void> set_sbnk_program_bit(TransactionState &state, MutablePartition &partition, SfsId id, std::uint8_t program,
                                  bool enabled, const CancellationToken &cancellation) {
    auto payload = current_payload(state, partition, id, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto current = sbnk_program_bit(*payload, program);
    if (!current)
        return std::unexpected{current.error()};
    const auto offset = 0xc0U + static_cast<std::size_t>((program - 1U) / 32U) * 4U;
    auto word = (std::to_integer<std::uint32_t>((*payload)[offset]) << 24U) |
                (std::to_integer<std::uint32_t>((*payload)[offset + 1U]) << 16U) |
                (std::to_integer<std::uint32_t>((*payload)[offset + 2U]) << 8U) |
                std::to_integer<std::uint32_t>((*payload)[offset + 3U]);
    const auto mask = std::uint32_t{1} << ((program - 1U) % 32U);
    word = enabled ? word | mask : word & ~mask;
    put_be32(*payload, offset, word);
    return replace_fixed_object_payload(state, partition, id, std::move(*payload), cancellation);
}

Result<void> set_sbnk_sample_bank_flag(TransactionState &state, MutablePartition &partition, SfsId id, bool enabled,
                                       const CancellationToken &cancellation) {
    auto payload = current_payload(state, partition, id, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (payload->size() <= 0xd0U) {
        return std::unexpected{
            transaction_error("Sample (SBNK) payload is too short for its Sample Bank membership flag")};
    }
    auto value = std::to_integer<std::uint8_t>((*payload)[0xd0U]);
    value = enabled ? static_cast<std::uint8_t>(value | 1U) : static_cast<std::uint8_t>(value & 0xfeU);
    (*payload)[0xd0U] = static_cast<std::byte>(value);
    return replace_fixed_object_payload(state, partition, id, std::move(*payload), cancellation);
}

Result<void> remove_directory_entry(TransactionState &state, MutablePartition &partition, SfsId directory, SfsId child,
                                    std::string_view name, const CancellationToken &cancellation) {
    auto payload = current_payload(state, partition, directory, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, directory);
    if (!entries)
        return std::unexpected{entries.error()};
    const auto found = std::ranges::find_if(
        *entries, [&](const ParsedDirectoryEntry &entry) { return entry.id == child && entry.name == name; });
    if (found == entries->end()) {
        return std::unexpected{transaction_error("directory entry is absent from transaction state")};
    }
    payload->erase(payload->begin() + static_cast<std::ptrdiff_t>(found->offset),
                   payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 32U));
    return replace_record_payload(state, partition, directory, std::move(*payload), cancellation);
}

Result<PartitionIndex> resolve_partition(const TransactionState &state, const PartitionSelector &selector) {
    if (const auto *direct = std::get_if<PartitionIndex>(&selector))
        return *direct;
    const auto &reference = std::get<OperationReference>(selector);
    const auto found = std::ranges::find(state.reports, reference.operation_id, &OperationReport::id);
    if (found == state.reports.end()) {
        return std::unexpected{transaction_error("operation reference has no earlier result")};
    }
    return found->partition;
}

Result<std::set<SfsId>> volume_closure(const Partition &partition, const DirectoryEntry &volume) {
    std::set<SfsId> result;
    std::vector<SfsId> queue{SfsId{volume.link_id.value}};
    while (!queue.empty()) {
        const auto id = queue.front();
        queue.erase(queue.begin());
        if (result.contains(id))
            continue;
        const auto *item = record(partition, id);
        if (item == nullptr)
            return std::unexpected{transaction_error("volume closure references a missing SFS record")};
        result.insert(id);
        if (item->payload_kind != PayloadKind::directory)
            continue;
        for (const auto &child : item->directory_entries) {
            if (child.name != "." && child.name != "..")
                queue.push_back(SfsId{child.link_id.value});
        }
    }
    for (const auto &item : partition.records) {
        if (result.contains(item.sfs_id) || item.sfs_id.value == 1U || item.payload_kind != PayloadKind::directory)
            continue;
        for (const auto &child : item.directory_entries) {
            if (child.name != "." && child.name != ".." && result.contains(SfsId{child.link_id.value})) {
                return std::unexpected{transaction_error("a directory outside the volume references its closure")};
            }
        }
    }
    return result;
}

Result<OperationReport> delete_volume(TransactionState &state, const OperationView &operation,
                                      const CancellationToken &cancellation) {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto partition_state = state.partitions.find(partition_index->value);
    if (partition_state == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &mutable_partition = partition_state->second;
    const auto &partition = *mutable_partition.source;
    const auto *root = record(partition, SfsId{1});
    if (root == nullptr || root->payload_kind != PayloadKind::directory || root->extents.size() != 1U ||
        !root->continuation_clusters.empty()) {
        return std::unexpected{transaction_error("partition root must use one readable direct extent")};
    }
    std::vector<const DirectoryEntry *> matches;
    for (const auto &entry : root->directory_entries) {
        if (entry.name == operation.volume_name)
            matches.push_back(&entry);
    }
    if (matches.size() != 1U)
        return std::unexpected{transaction_error("volume name is not unique in the selected partition")};
    auto closure = volume_closure(partition, *matches.front());
    if (!closure)
        return std::unexpected{closure.error()};

    std::map<std::string, SfsId> ids;
    for (const auto &object : state.catalog.objects) {
        if (object.partition.value == partition.index.value)
            ids.emplace(object.key, object.sfs_id);
    }
    for (const auto &relation : state.graph.relationships) {
        if (relation.quality != RelationshipQuality::known || !relation.target_key)
            continue;
        const auto source = ids.find(relation.source_key);
        const auto target = ids.find(*relation.target_key);
        if (source == ids.end() || target == ids.end())
            continue;
        if (closure->contains(source->second) != closure->contains(target->second)) {
            return std::unexpected{transaction_error("a known object relationship crosses the volume closure")};
        }
    }
    auto payload = current_root_payload(state, mutable_partition, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    const auto offset = matches.front()->payload_relative_offset;
    if (offset > payload->size() || payload->size() - offset < 32U) {
        return std::unexpected{transaction_error("volume directory entry lies outside the root payload")};
    }
    payload->erase(payload->begin() + static_cast<std::ptrdiff_t>(offset),
                   payload->begin() + static_cast<std::ptrdiff_t>(offset + 32U));
    if (auto replaced = set_root_payload(state, mutable_partition, std::move(*payload), cancellation); !replaced)
        return std::unexpected{replaced.error()};
    std::uint64_t freed{};
    for (const auto id : *closure) {
        const auto *item = record(partition, id);
        for (const auto &extent : item->extents) {
            for (std::uint32_t cluster = extent.cluster_offset; cluster < extent.cluster_offset + extent.cluster_count;
                 ++cluster) {
                set_bitmap(mutable_partition.bitmap, cluster, false);
                ++freed;
            }
        }
        for (const auto cluster : item->continuation_clusters) {
            set_bitmap(mutable_partition.bitmap, cluster, false);
            ++freed;
        }
        mutable_partition.deleted.insert(id);
    }
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = partition.index;
    report.volume_name = operation.volume_name;
    report.freed_clusters = freed;
    report.removed_sfs_ids.assign(closure->begin(), closure->end());
    return report;
}

std::vector<SfsId> free_ids(const MutablePartition &partition, std::size_t count) {
    const auto capacity = (static_cast<std::uint64_t>(partition.source->directory_index_span_clusters) *
                           partition.source->sectors_per_cluster * 512U / 1024U) *
                          14U;
    std::vector<SfsId> result;
    for (std::uint32_t value = 3; value < capacity && result.size() < count; ++value) {
        const SfsId id{value};
        if (!partition.inserted.contains(id) && !partition.changed.contains(id) &&
            (partition.deleted.contains(id) || record(*partition.source, id) == nullptr)) {
            result.push_back(id);
        }
    }
    return result;
}

Result<std::vector<Extent>> allocate_extents(MutablePartition &partition, std::uint32_t count) {
    std::vector<std::uint32_t> selected;
    const auto first = partition.source->directory_index_cluster + partition.source->directory_index_span_clusters;
    for (std::uint32_t cluster = first; cluster < partition.source->cluster_count && selected.size() < count;
         ++cluster) {
        if (!bitmap_used(partition.bitmap, cluster))
            selected.push_back(cluster);
    }
    if (selected.size() != count)
        return std::unexpected{transaction_error("partition has insufficient free clusters")};
    std::vector<Extent> result;
    for (const auto cluster : selected) {
        set_bitmap(partition.bitmap, cluster, true);
        if (!result.empty() && result.back().cluster_offset + result.back().cluster_count == cluster) {
            ++result.back().cluster_count;
            result.back().byte_count += 1024U;
        } else {
            result.push_back({cluster, 1, 1024});
        }
    }
    return result;
}

Result<std::vector<std::uint32_t>> allocate_list_clusters(MutablePartition &partition, std::size_t count) {
    std::vector<std::uint32_t> result;
    const auto first = partition.source->directory_index_cluster + partition.source->directory_index_span_clusters;
    for (std::uint32_t cluster = first; cluster < partition.source->cluster_count && result.size() < count; ++cluster) {
        if (!bitmap_used(partition.bitmap, cluster))
            result.push_back(cluster);
    }
    if (result.size() != count) {
        return std::unexpected{transaction_error("partition has insufficient continuation-list clusters")};
    }
    for (const auto cluster : result)
        set_bitmap(partition.bitmap, cluster, true);
    return result;
}

std::vector<std::byte> index_for(const detail::PreparedRecord &source, const std::vector<Extent> &extents,
                                 std::uint32_t size, std::span<const std::uint32_t> list_clusters = {}) {
    std::vector<std::byte> result(72);
    put_be16(result, 0, static_cast<std::uint16_t>(extents.size()));
    std::uint32_t clusters{};
    for (const auto &extent : extents)
        clusters += extent.cluster_count;
    put_be16(result, 4, static_cast<std::uint16_t>(clusters));
    put_be32(result, 6, size);
    if (!list_clusters.empty()) {
        put_be32(result, 0x0aU, list_clusters.front());
        put_be32(result, 0x0eU, clusters);
        put_be32(result, 0x12U, size);
    } else {
        std::uint32_t remaining = size;
        for (std::size_t index = 0; index < extents.size(); ++index) {
            const auto &extent = extents[index];
            const auto capacity = extent.cluster_count * 1024U;
            const auto byte_count = remaining == 0U ? capacity : std::min(remaining, capacity);
            remaining = remaining > byte_count ? remaining - byte_count : 0U;
            const auto offset = 0x0aU + index * 12U;
            put_be32(result, offset, extent.cluster_offset);
            put_be32(result, offset + 4U, extent.cluster_count);
            put_be32(result, offset + 8U, byte_count);
        }
    }
    std::fill(result.begin() + 0x3a, result.begin() + 0x42, std::byte{0xff});
    if (source.kind == detail::RecordKind::directory) {
        result[0x42] = std::byte{0x94};
        result[0x43] = std::byte{'d'};
        result[0x44] = std::byte{'i'};
        result[0x45] = std::byte{'r'};
        put_be16(result, 0x46, source.tail);
    } else {
        result[0x42] = std::byte{0x9e};
        result[0x47] = std::byte{1};
    }
    return result;
}

Result<std::pair<SfsId, std::uint64_t>> allocate_record(MutablePartition &partition, std::vector<std::byte> payload,
                                                        PayloadKind payload_kind,
                                                        std::optional<SfsId> requested_id = {},
                                                        std::uint16_t directory_tail = 0U) {
    const auto ids = requested_id ? std::vector{*requested_id} : free_ids(partition, 1U);
    if (ids.empty() ||
        (requested_id && (record_exists(partition, *requested_id) || partition.inserted.contains(*requested_id)))) {
        return std::unexpected{transaction_error("partition has no free SFS record")};
    }
    const auto clusters = std::max<std::uint32_t>(2U, static_cast<std::uint32_t>((payload.size() + 1023U) / 1024U));
    auto extents = allocate_extents(partition, clusters);
    if (!extents)
        return std::unexpected{extents.error()};
    constexpr std::size_t extents_per_list_cluster = (1024U - 12U) / 12U;
    std::vector<std::uint32_t> list_clusters;
    if (extents->size() > 4U) {
        auto allocated_lists = allocate_list_clusters(partition, (extents->size() + extents_per_list_cluster - 1U) /
                                                                     extents_per_list_cluster);
        if (!allocated_lists)
            return std::unexpected{allocated_lists.error()};
        list_clusters = std::move(*allocated_lists);
    }
    detail::PreparedRecord prepared;
    prepared.kind = payload_kind == PayloadKind::directory ? detail::RecordKind::directory : detail::RecordKind::object;
    prepared.tail = directory_tail;
    auto raw = index_for(prepared, *extents, static_cast<std::uint32_t>(payload.size()), list_clusters);
    const auto id = ids.front();
    partition.deleted.erase(id);
    partition.inserted.emplace(id, MutablePartition::InsertedRecord{id, std::move(raw), std::move(payload),
                                                                    std::move(*extents), std::move(list_clusters),
                                                                    payload_kind});
    return std::pair{id, clusters + partition.inserted.at(id).continuation_clusters.size()};
}

Result<void> append_directory_entry(TransactionState &state, MutablePartition &partition, SfsId directory, SfsId child,
                                    std::string_view name, const CancellationToken &cancellation) {
    if (name.empty() || name.size() > 16U ||
        !std::ranges::all_of(name, [](unsigned char value) { return value < 0x80U; })) {
        return std::unexpected{transaction_error("SFS object name must fit 16 ASCII bytes")};
    }
    auto payload = current_payload(state, partition, directory, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, directory);
    if (!entries)
        return std::unexpected{entries.error()};
    if (std::ranges::any_of(*entries, [&](const ParsedDirectoryEntry &entry) { return entry.name == name; })) {
        return std::unexpected{transaction_error("directory already contains entry " + std::string{name})};
    }
    std::array<std::byte, 32> entry{};
    put_be16(entry, 0U, 0x20U);
    put_be16(entry, 2U, 17U);
    put_be32(entry, 4U, child.value);
    std::fill(entry.begin() + 8U, entry.begin() + 24U, std::byte{' '});
    std::ranges::transform(name, entry.begin() + 8U, [](char value) { return static_cast<std::byte>(value); });
    payload->insert(payload->end(), entry.begin(), entry.end());
    return replace_record_payload(state, partition, directory, std::move(*payload), cancellation);
}

Result<void> rename_directory_entry(TransactionState &state, MutablePartition &partition, SfsId directory, SfsId child,
                                    std::string_view old_name, std::string_view new_name,
                                    const CancellationToken &cancellation) {
    if (new_name.empty() || new_name.size() > 16U ||
        !std::ranges::all_of(new_name, [](unsigned char value) { return value < 0x80U; })) {
        return std::unexpected{transaction_error("SFS object name must fit 16 ASCII bytes")};
    }
    auto payload = current_payload(state, partition, directory, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, directory);
    if (!entries)
        return std::unexpected{entries.error()};
    if (std::ranges::any_of(*entries, [&](const auto &entry) { return entry.name == new_name && entry.id != child; })) {
        return std::unexpected{transaction_error("rename destination already exists")};
    }
    const auto found =
        std::ranges::find_if(*entries, [&](const auto &entry) { return entry.id == child && entry.name == old_name; });
    if (found == entries->end()) {
        return std::unexpected{transaction_error("rename source directory entry is absent")};
    }
    put_be16(*payload, found->offset + 2U, 17U);
    std::fill(payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 8U),
              payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 24U), std::byte{' '});
    std::ranges::transform(new_name, payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 8U),
                           [](char value) { return static_cast<std::byte>(value); });
    (*payload)[found->offset + 24U] = std::byte{0};
    return replace_record_payload(state, partition, directory, std::move(*payload), cancellation);
}

Result<void> rename_object_payload(TransactionState &state, MutablePartition &partition, SfsId id,
                                   std::string_view old_name, std::string_view new_name,
                                   const CancellationToken &cancellation) {
    auto payload = current_payload(state, partition, id, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (payload->size() < 0x42U) {
        return std::unexpected{transaction_error("object payload name is truncated")};
    }
    std::string actual;
    for (std::size_t offset = 0x32U; offset < 0x42U; ++offset) {
        const auto value = std::to_integer<unsigned char>((*payload)[offset]);
        if (value != 0U)
            actual.push_back(static_cast<char>(value));
    }
    while (!actual.empty() && actual.back() == ' ')
        actual.pop_back();
    if (actual != old_name) {
        return std::unexpected{transaction_error("object payload name disagrees with directory identity")};
    }
    std::fill(payload->begin() + 0x32, payload->begin() + 0x42, std::byte{' '});
    std::ranges::transform(new_name, payload->begin() + 0x32, [](char value) { return static_cast<std::byte>(value); });
    return replace_fixed_object_payload(state, partition, id, std::move(*payload), cancellation);
}

Result<std::uint64_t> release_record(MutablePartition &partition, SfsId id) {
    std::vector<Extent> extents;
    std::vector<std::uint32_t> continuation;
    if (const auto found = partition.inserted.find(id); found != partition.inserted.end()) {
        extents = found->second.extents;
        continuation = found->second.continuation_clusters;
        partition.inserted.erase(found);
    } else if (const auto changed = partition.changed.find(id); changed != partition.changed.end()) {
        extents = changed->second.extents;
        continuation = changed->second.continuation_clusters;
        partition.changed.erase(changed);
    } else if (const auto *source = record(*partition.source, id);
               source != nullptr && !partition.deleted.contains(id)) {
        extents = source->extents;
        continuation = source->continuation_clusters;
    } else {
        return std::unexpected{transaction_error("cannot release a missing SFS record")};
    }
    std::uint64_t released{};
    for (const auto &extent : extents) {
        for (std::uint32_t cluster = extent.cluster_offset; cluster < extent.cluster_offset + extent.cluster_count;
             ++cluster) {
            set_bitmap(partition.bitmap, cluster, false);
            ++released;
        }
    }
    for (const auto cluster : continuation) {
        set_bitmap(partition.bitmap, cluster, false);
        ++released;
    }
    partition.deleted.insert(id);
    return released;
}

std::vector<std::byte> remap_directory(std::vector<std::byte> payload, const std::map<std::uint32_t, SfsId> &ids) {
    const auto directory_id = payload.size() >= 8U ? (std::to_integer<std::uint32_t>(payload[4]) << 24U) |
                                                         (std::to_integer<std::uint32_t>(payload[5]) << 16U) |
                                                         (std::to_integer<std::uint32_t>(payload[6]) << 8U) |
                                                         std::to_integer<std::uint32_t>(payload[7])
                                                   : 0U;
    for (std::size_t offset = 0; offset + 32U <= payload.size(); offset += 32U) {
        const auto old = (std::to_integer<std::uint32_t>(payload[offset + 4U]) << 24U) |
                         (std::to_integer<std::uint32_t>(payload[offset + 5U]) << 16U) |
                         (std::to_integer<std::uint32_t>(payload[offset + 6U]) << 8U) |
                         std::to_integer<std::uint32_t>(payload[offset + 7U]);
        if (const auto found = ids.find(old); found != ids.end())
            put_be32(payload, offset + 4U, found->second.value);
        if (old == 1U && payload[offset + 8U] == std::byte{'.'} && payload[offset + 9U] == std::byte{'.'}) {
            const auto mapped = ids.find(directory_id);
            payload[offset + 11U] = static_cast<std::byte>(mapped == ids.end() ? 0U : mapped->second.value & 0xffU);
        }
    }
    return payload;
}

Result<OperationReport> insert_volume(TransactionState &state, const OperationView &operation,
                                      const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end() || !operation.volume)
        return std::unexpected{transaction_error("insert-volume target is invalid")};
    auto &partition = found->second;
    auto root_payload = current_root_payload(state, partition, cancellation);
    if (!root_payload)
        return std::unexpected{root_payload.error()};
    for (std::size_t offset = 64U; offset + 32U <= root_payload->size(); offset += 32U) {
        const auto length =
            static_cast<std::size_t>(std::to_integer<std::uint16_t>((*root_payload)[offset + 2U]) << 8U |
                                     std::to_integer<std::uint16_t>((*root_payload)[offset + 3U]));
        std::string name;
        for (std::size_t index = 0; index + 1U < length && index < 24U; ++index) {
            const auto character = std::to_integer<char>((*root_payload)[offset + 8U + index]);
            if (character != '\0')
                name += character;
        }
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (name == operation.volume->name)
            return std::unexpected{transaction_error("partition already contains the requested volume")};
    }
    HdsBuildManifest template_manifest{"1.1", minimum_hds_size, {{"AXK ALTER", {*operation.volume}}}};
    auto geometry = plan_hds_geometry(template_manifest);
    if (!geometry)
        return std::unexpected{geometry.error()};
    auto prepared = detail::prepare_partition_records(template_manifest.partitions[0], (*geometry)[0], 1, cancellation);
    if (!prepared)
        return std::unexpected{prepared.error()};
    std::vector<detail::PreparedRecord> templates;
    std::ranges::copy_if(*prepared, std::back_inserter(templates), [](const auto &item) { return item.id >= 3U; });
    const auto ids = free_ids(partition, templates.size());
    if (ids.size() != templates.size())
        return std::unexpected{transaction_error("partition has insufficient free SFS records")};
    std::map<std::uint32_t, SfsId> id_map;
    for (std::size_t index = 0; index < templates.size(); ++index)
        id_map.emplace(templates[index].id, ids[index]);
    std::uint64_t allocated{};
    for (std::size_t index = 0; index < templates.size(); ++index) {
        auto payload = templates[index].kind == detail::RecordKind::directory
                           ? remap_directory(templates[index].payload, id_map)
                           : templates[index].payload;
        auto stored = allocate_record(partition, std::move(payload),
                                      templates[index].kind == detail::RecordKind::directory ? PayloadKind::directory
                                                                                             : PayloadKind::object,
                                      ids[index], templates[index].tail);
        if (!stored)
            return std::unexpected{stored.error()};
        allocated += stored->second;
    }
    std::array<std::byte, 32> entry{};
    put_be16(entry, 0, 0x20);
    put_be16(entry, 2, 17);
    put_be32(entry, 4, id_map.at(3).value);
    const auto encoded = operation.volume->name;
    for (std::size_t index = 0; index < 16U; ++index)
        entry[8U + index] = index < encoded.size() ? static_cast<std::byte>(encoded[index]) : std::byte{' '};
    root_payload->insert(root_payload->end(), entry.begin(), entry.end());
    if (auto replaced = set_root_payload(state, partition, std::move(*root_payload), cancellation); !replaced)
        return std::unexpected{replaced.error()};
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume->name;
    report.inserted_sfs_ids = ids;
    report.allocated_clusters = allocated;
    return report;
}

Result<OperationReport> rename_volume(TransactionState &state, const OperationView &operation,
                                      const CancellationToken &cancellation) {
    if (operation.volume_name == operation.new_volume_name)
        return std::unexpected{transaction_error("new_volume_name must differ")};
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto payload = current_root_payload(state, partition, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, SfsId{1});
    if (!entries)
        return std::unexpected{entries.error()};
    const auto matches = std::ranges::count(*entries, operation.volume_name, &ParsedDirectoryEntry::name);
    if (matches != 1U)
        return std::unexpected{transaction_error("volume name is not unique in the selected partition")};
    const auto source = std::ranges::find(*entries, operation.volume_name, &ParsedDirectoryEntry::name);
    if (auto renamed = rename_directory_entry(state, partition, SfsId{1}, source->id, operation.volume_name,
                                              operation.new_volume_name, cancellation);
        !renamed) {
        return std::unexpected{renamed.error()};
    }
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.new_volume_name;
    return report;
}

Result<OperationReport> rename_partition(TransactionState &state, const OperationView &operation,
                                         const CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (operation.partition_name == operation.new_partition_name)
        return std::unexpected{transaction_error("new_partition_name must differ")};
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    const auto &current_name = partition.renamed_name ? *partition.renamed_name : partition.source->name;
    if (current_name != operation.partition_name)
        return std::unexpected{transaction_error("partition name changed since the alteration was prepared")};
    partition.renamed_name = operation.new_partition_name;
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    return report;
}

Result<OperationReport> delete_sbnk(TransactionState &state, const OperationView &operation,
                                    const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end()) {
        return std::unexpected{transaction_error("partition index does not exist")};
    }
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SBNK", operation.sample_name, "SBNK", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    const auto [directory_id, sample_id] = *located;
    auto payload = current_payload(state, partition, sample_id, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *sample = std::get_if<CurrentSbnk>(&decoded->payload);
    if (sample == nullptr) {
        return std::unexpected{transaction_error("Sample is not a current SBNK object")};
    }
    if (!sample->linked_program_numbers.empty()) {
        return std::unexpected{transaction_error("Sample is referenced by its Program link bitmap")};
    }
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        if (edge_partition == *partition_index && target == sample_id && record_exists(partition, source)) {
            return std::unexpected{transaction_error("Sample is referenced by a Program or Sample Bank")};
        }
    }
    if (auto removed =
            remove_directory_entry(state, partition, directory_id, sample_id, operation.sample_name, cancellation);
        !removed) {
        return std::unexpected{removed.error()};
    }
    auto freed = release_record(partition, sample_id);
    if (!freed)
        return std::unexpected{freed.error()};
    std::erase_if(state.known_edges, [&](const auto &edge) {
        const auto &[edge_partition, source, target] = edge;
        return edge_partition == *partition_index && (source == sample_id || target == sample_id);
    });
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.sample_name;
    report.removed_sfs_ids = {sample_id};
    report.freed_clusters = *freed;
    return report;
}

Result<detail::PreparedWaveformMember> waveform_member(TransactionState &state, MutablePartition &partition,
                                                       std::string_view volume_name, std::string_view waveform_name,
                                                       const CancellationToken &cancellation) {
    auto located = category_object(state, partition, volume_name, "SMPL", waveform_name, "SMPL", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *wave_data = std::get_if<CurrentSmpl>(&decoded->payload);
    if (wave_data == nullptr || wave_data->link_id.value == 0U) {
        return std::unexpected{transaction_error("waveform has no usable current SMPL link ID")};
    }
    return detail::PreparedWaveformMember{std::string{waveform_name}, wave_data->link_id.value,
                                          wave_data->duplicate_sample_rate.value, wave_data->wave_length_frames.value};
}

Result<OperationReport> insert_sbnk(TransactionState &state, const OperationView &operation,
                                    const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end() || !operation.sample) {
        return std::unexpected{transaction_error("insert-sbnk target is invalid")};
    }
    auto &partition = found->second;
    const auto &spec = *operation.sample;
    auto directory = volume_category(state, partition, operation.volume_name, "SBNK", cancellation);
    if (!directory)
        return std::unexpected{directory.error()};
    auto directory_payload = current_payload(state, partition, *directory, cancellation);
    if (!directory_payload)
        return std::unexpected{directory_payload.error()};
    auto entries = parse_directory(*directory_payload, *directory);
    if (!entries)
        return std::unexpected{entries.error()};
    if (std::ranges::any_of(*entries, [&](const ParsedDirectoryEntry &entry) { return entry.name == spec.name; })) {
        return std::unexpected{transaction_error("volume already contains the requested Sample")};
    }
    if (!spec.waveform_id) {
        return std::unexpected{transaction_error("Sample requires waveform_name")};
    }
    auto left = waveform_member(state, partition, operation.volume_name, *spec.waveform_id, cancellation);
    if (!left)
        return std::unexpected{left.error()};
    std::optional<detail::PreparedWaveformMember> right;
    if (spec.right_waveform_id) {
        auto member = waveform_member(state, partition, operation.volume_name, *spec.right_waveform_id, cancellation);
        if (!member)
            return std::unexpected{member.error()};
        if (member->sample_rate != left->sample_rate || member->frame_count != left->frame_count) {
            return std::unexpected{transaction_error("stereo Sample requires matching Wave Data sample "
                                                     "rates and frame counts")};
        }
        right = std::move(*member);
    }
    auto payload = detail::prepare_sbnk_payload(spec, *left, right);
    if (!payload)
        return std::unexpected{payload.error()};
    // The alteration contract preserves the complete current SBNK contract
    // window, while fresh-image records use the sampler's shorter object size.
    payload->resize(0x200U, std::byte{0});
    auto allocated = allocate_record(partition, std::move(*payload), PayloadKind::object);
    if (!allocated)
        return std::unexpected{allocated.error()};
    const auto [bank_id, cluster_count] = *allocated;
    if (auto appended = append_directory_entry(state, partition, *directory, bank_id, spec.name, cancellation);
        !appended) {
        return std::unexpected{appended.error()};
    }
    const auto left_object =
        category_object(state, partition, operation.volume_name, "SMPL", *spec.waveform_id, "SMPL", cancellation);
    if (!left_object)
        return std::unexpected{left_object.error()};
    state.known_edges.emplace_back(*partition_index, bank_id, left_object->second);
    if (spec.right_waveform_id) {
        const auto right_object = category_object(state, partition, operation.volume_name, "SMPL",
                                                  *spec.right_waveform_id, "SMPL", cancellation);
        if (!right_object)
            return std::unexpected{right_object.error()};
        state.known_edges.emplace_back(*partition_index, bank_id, right_object->second);
    }
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = spec.name;
    report.inserted_sfs_ids = {bank_id};
    report.allocated_clusters = cluster_count;
    return report;
}

Result<OperationReport> insert_waveform(TransactionState &state, const OperationView &operation,
                                        const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end() || !operation.waveform) {
        return std::unexpected{transaction_error("insert-waveform target is invalid")};
    }
    auto &partition = found->second;
    const auto &spec = *operation.waveform;
    auto directory = volume_category(state, partition, operation.volume_name, "SMPL", cancellation);
    if (!directory)
        return std::unexpected{directory.error()};
    auto directory_payload = current_payload(state, partition, *directory, cancellation);
    if (!directory_payload)
        return std::unexpected{directory_payload.error()};
    auto entries = parse_directory(*directory_payload, *directory);
    if (!entries)
        return std::unexpected{entries.error()};

    std::set<std::uint32_t> link_ids;
    for (const auto &entry : *entries) {
        if (entry.name == "." || entry.name == "..")
            continue;
        if (std::ranges::contains(spec.waveform_names, entry.name)) {
            return std::unexpected{transaction_error("volume already contains a requested waveform name")};
        }
        auto payload = current_payload(state, partition, entry.id, cancellation);
        if (!payload)
            return std::unexpected{transaction_error("existing SMPL record is unresolved; link-ID "
                                                     "allocation is unsafe")};
        auto decoded = decode_object(*payload);
        if (!decoded)
            return std::unexpected{decoded.error()};
        const auto *wave_data = std::get_if<CurrentSmpl>(&decoded->payload);
        if (wave_data == nullptr || wave_data->link_id.value == 0U) {
            return std::unexpected{transaction_error("existing waveform has no current SMPL link ID")};
        }
        link_ids.insert(wave_data->link_id.value);
    }

    AudioImportOptions options;
    options.expected_channels = static_cast<std::uint8_t>(spec.waveform_names.size());
    options.target_sample_rate = spec.target_sample_rate;
    auto audio = import_sampler_audio(spec.path, options);
    if (!audio)
        return std::unexpected{audio.error()};
    std::uint32_t candidate = 0x016b1dbcU;
    std::vector<SfsId> inserted;
    std::uint64_t allocated_clusters{};
    for (std::size_t channel = 0; channel < spec.waveform_names.size(); ++channel) {
        while (link_ids.contains(candidate))
            candidate += 0x100U;
        const auto link_id = candidate;
        link_ids.insert(link_id);
        candidate += 0x100U;

        auto mono = *audio;
        mono.source_channels = 1U;
        mono.pcm_channels = {audio->pcm_channels[channel]};
        WaveformSpec waveform;
        waveform.id = spec.waveform_names[channel];
        waveform.name = spec.waveform_names[channel];
        waveform.path = spec.path;
        waveform.root_key = spec.root_key;
        waveform.target_sample_rate = spec.target_sample_rate;
        auto payload = detail::prepare_smpl_payload(waveform, mono, link_id);
        if (!payload)
            return std::unexpected{payload.error()};
        auto stored = allocate_record(partition, std::move(*payload), PayloadKind::object);
        if (!stored)
            return std::unexpected{stored.error()};
        if (auto appended = append_directory_entry(state, partition, *directory, stored->first,
                                                   spec.waveform_names[channel], cancellation);
            !appended) {
            return std::unexpected{appended.error()};
        }
        inserted.push_back(stored->first);
        allocated_clusters += stored->second;
    }
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    for (std::size_t index = 0; index < spec.waveform_names.size(); ++index) {
        if (index != 0U)
            report.object_name += ';';
        report.object_name += spec.waveform_names[index];
    }
    report.inserted_sfs_ids = std::move(inserted);
    report.allocated_clusters = allocated_clusters;
    report.audio_import = AudioImportSummary{audio->source_path,
                                             audio->source_format,
                                             audio->source_subtype,
                                             audio->source_channels,
                                             audio->source_sample_rate,
                                             audio->output_sample_rate,
                                             audio->source_sample_width_bits,
                                             audio->output_sample_width_bits,
                                             audio->output_frames,
                                             audio->resampled,
                                             audio->quantized,
                                             audio->sample_width_converted,
                                             audio->source_channels == 2U,
                                             audio->dither_algorithm,
                                             audio->clipped_samples};
    return report;
}

Result<OperationReport> delete_waveform(TransactionState &state, const OperationView &operation,
                                        const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end()) {
        return std::unexpected{transaction_error("partition index does not exist")};
    }
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SMPL", operation.waveform_name, "SMPL", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    const auto [directory_id, waveform_id] = *located;
    auto waveform_payload = current_payload(state, partition, waveform_id, cancellation);
    if (!waveform_payload)
        return std::unexpected{waveform_payload.error()};
    auto waveform_object = decode_object(*waveform_payload);
    if (!waveform_object)
        return std::unexpected{waveform_object.error()};
    const auto *wave_data = std::get_if<CurrentSmpl>(&waveform_object->payload);
    if (wave_data == nullptr || wave_data->link_id.value == 0U) {
        return std::unexpected{transaction_error("waveform cannot be classified as known_unreferenced")};
    }

    auto sample_directory = volume_category(state, partition, operation.volume_name, "SBNK", cancellation);
    if (!sample_directory)
        return std::unexpected{sample_directory.error()};
    auto sample_directory_payload = current_payload(state, partition, *sample_directory, cancellation);
    if (!sample_directory_payload)
        return std::unexpected{sample_directory_payload.error()};
    auto sample_entries = parse_directory(*sample_directory_payload, *sample_directory);
    if (!sample_entries)
        return std::unexpected{sample_entries.error()};
    for (const auto &entry : *sample_entries) {
        if (entry.name == "." || entry.name == "..")
            continue;
        auto payload = current_payload(state, partition, entry.id, cancellation);
        if (!payload) {
            return std::unexpected{transaction_error("waveform ownership is ambiguous because an "
                                                     "SBNK is unreadable")};
        }
        auto object = decode_object(*payload);
        if (!object)
            return std::unexpected{object.error()};
        const auto *sample = std::get_if<CurrentSbnk>(&object->payload);
        if (sample == nullptr) {
            return std::unexpected{transaction_error("waveform ownership is ambiguous because an "
                                                     "SBNK entry is unresolved")};
        }
        const auto references = [&](const CurrentSbnkMember &member) {
            return member.smpl_link_id == wave_data->link_id.value || member.wave_data_name == operation.waveform_name;
        };
        if (references(sample->left) || (sample->right && references(*sample->right))) {
            return std::unexpected{transaction_error("waveform is referenced, not known_unreferenced")};
        }
    }
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        static_cast<void>(source);
        if (edge_partition == *partition_index && target == waveform_id) {
            return std::unexpected{transaction_error("waveform has a known incoming reference, "
                                                     "not known_unreferenced")};
        }
    }
    if (auto removed =
            remove_directory_entry(state, partition, directory_id, waveform_id, operation.waveform_name, cancellation);
        !removed) {
        return std::unexpected{removed.error()};
    }
    auto freed = release_record(partition, waveform_id);
    if (!freed)
        return std::unexpected{freed.error()};
    std::erase_if(state.known_edges, [&](const auto &edge) {
        const auto &[edge_partition, source, target] = edge;
        return edge_partition == *partition_index && (source == waveform_id || target == waveform_id);
    });
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.waveform_name;
    report.removed_sfs_ids = {waveform_id};
    report.freed_clusters = *freed;
    return report;
}

Result<OperationReport> delete_program(TransactionState &state, const OperationView &operation,
                                       const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end() || !operation.program_number) {
        return std::unexpected{transaction_error("delete-program target is invalid")};
    }
    auto &partition = found->second;
    const auto name = std::format("{:03}", *operation.program_number);
    auto located = category_object(state, partition, operation.volume_name, "PROG", name, "PROG", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *program = std::get_if<CurrentProg>(&decoded->payload);
    if (program == nullptr)
        return std::unexpected{transaction_error("Program is unreadable")};
    std::set<SfsId> assigned_samples;
    for (const auto &assignment : program->assignments) {
        if (assignment.name.empty() || assignment.kind != 0x10U)
            continue;
        auto sample =
            category_object(state, partition, operation.volume_name, "SBNK", assignment.name, "SBNK", cancellation);
        if (!sample)
            return std::unexpected{sample.error()};
        assigned_samples.insert(sample->second);
    }
    auto samples = category_objects(state, partition, operation.volume_name, "SBNK", ObjectType::sbnk, cancellation);
    if (!samples)
        return std::unexpected{samples.error()};
    std::set<SfsId> bitmap_samples;
    for (const auto &sample : *samples) {
        auto bit = sbnk_program_bit(sample.payload, *operation.program_number);
        if (!bit)
            return std::unexpected{bit.error()};
        if (*bit)
            bitmap_samples.insert(sample.id);
    }
    if (assigned_samples != bitmap_samples) {
        return std::unexpected{transaction_error("Program direct assignments do not match SBNK "
                                                 "Program-link bitmaps")};
    }
    for (const auto id : assigned_samples) {
        if (auto updated = set_sbnk_program_bit(state, partition, id, *operation.program_number, false, cancellation);
            !updated)
            return std::unexpected{updated.error()};
    }
    if (auto removed = remove_directory_entry(state, partition, located->first, located->second, name, cancellation);
        !removed)
        return std::unexpected{removed.error()};
    auto freed = release_record(partition, located->second);
    if (!freed)
        return std::unexpected{freed.error()};
    std::erase_if(state.known_edges, [&](const auto &edge) {
        const auto &[edge_partition, source, target] = edge;
        return edge_partition == *partition_index && (source == located->second || target == located->second);
    });
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = name;
    report.removed_sfs_ids = {located->second};
    report.freed_clusters = *freed;
    return report;
}

Result<OperationReport> insert_program(TransactionState &state, const OperationView &operation,
                                       const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end() || !operation.program) {
        return std::unexpected{transaction_error("insert-program target is invalid")};
    }
    auto &partition = found->second;
    const auto &spec = *operation.program;
    const auto name = std::format("{:03}", spec.number);
    auto directory = volume_category(state, partition, operation.volume_name, "PROG", cancellation);
    if (!directory)
        return std::unexpected{directory.error()};
    auto directory_payload = current_payload(state, partition, *directory, cancellation);
    if (!directory_payload)
        return std::unexpected{directory_payload.error()};
    auto entries = parse_directory(*directory_payload, *directory);
    if (!entries)
        return std::unexpected{entries.error()};
    if (std::ranges::any_of(*entries, [&](const auto &entry) { return entry.name == name; })) {
        return std::unexpected{transaction_error("volume already contains Program " + name)};
    }
    if (spec.assignments.size() != 2U) {
        return std::unexpected{transaction_error("Program requires exactly two assignments")};
    }
    const auto &sample_bank_assignment = spec.assignments[0];
    const auto &sample_assignment = spec.assignments[1];
    auto sample_bank = category_object(state, partition, operation.volume_name, "SBAC",
                                       sample_bank_assignment.target_name, "SBAC", cancellation);
    if (!sample_bank)
        return std::unexpected{sample_bank.error()};
    auto sample = category_object(state, partition, operation.volume_name, "SBNK", sample_assignment.target_name,
                                  "SBNK", cancellation);
    if (!sample)
        return std::unexpected{sample.error()};
    auto existing_programs =
        category_objects(state, partition, operation.volume_name, "PROG", ObjectType::prog, cancellation);
    if (!existing_programs)
        return std::unexpected{existing_programs.error()};
    for (const auto &existing : *existing_programs) {
        const auto *decoded_program = std::get_if<CurrentProg>(&existing.decoded.payload);
        for (const auto &assignment : decoded_program->assignments) {
            if ((assignment.kind == 0x11U && assignment.name == sample_bank_assignment.target_name) ||
                (assignment.kind == 0x10U && assignment.name == sample_assignment.target_name)) {
                return std::unexpected{transaction_error("Program target is already assigned by another Program")};
            }
        }
    }
    auto sample_payload = current_payload(state, partition, sample->second, cancellation);
    if (!sample_payload)
        return std::unexpected{sample_payload.error()};
    auto bit = sbnk_program_bit(*sample_payload, spec.number);
    if (!bit)
        return std::unexpected{bit.error()};
    if (*bit)
        return std::unexpected{transaction_error("SBNK already links this Program")};
    auto payload = detail::prepare_prog_payload(spec);
    if (!payload)
        return std::unexpected{payload.error()};
    auto allocated = allocate_record(partition, std::move(*payload), PayloadKind::object);
    if (!allocated)
        return std::unexpected{allocated.error()};
    if (auto appended = append_directory_entry(state, partition, *directory, allocated->first, name, cancellation);
        !appended)
        return std::unexpected{appended.error()};
    if (auto updated = set_sbnk_program_bit(state, partition, sample->second, spec.number, true, cancellation);
        !updated)
        return std::unexpected{updated.error()};
    state.known_edges.emplace_back(*partition_index, allocated->first, sample_bank->second);
    state.known_edges.emplace_back(*partition_index, allocated->first, sample->second);
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = name;
    report.inserted_sfs_ids = {allocated->first};
    report.allocated_clusters = allocated->second;
    return report;
}

Result<OperationReport> delete_sbac(TransactionState &state, const OperationView &operation,
                                    const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end()) {
        return std::unexpected{transaction_error("partition index does not exist")};
    }
    auto &partition = found->second;
    auto located = category_object(state, partition, operation.volume_name, "SBAC", operation.sample_bank_name, "SBAC",
                                   cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto decoded = decode_object(*payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *sample_bank = std::get_if<CurrentSbac>(&decoded->payload);
    if (sample_bank == nullptr || sample_bank->active_slot_count > sample_bank->maximum_slot_count) {
        return std::unexpected{transaction_error("Sample Bank slots are unreadable")};
    }
    auto programs = category_objects(state, partition, operation.volume_name, "PROG", ObjectType::prog, cancellation);
    if (!programs)
        return std::unexpected{programs.error()};
    for (const auto &program_row : *programs) {
        const auto *program = std::get_if<CurrentProg>(&program_row.decoded.payload);
        if (std::ranges::any_of(program->assignments, [&](const ProgAssignment &assignment) {
                return assignment.kind == 0x11U && assignment.name == operation.sample_bank_name;
            })) {
            return std::unexpected{transaction_error("Sample Bank is referenced by a Program")};
        }
    }
    auto sample_banks =
        category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac, cancellation);
    if (!sample_banks)
        return std::unexpected{sample_banks.error()};
    std::set<std::string> members;
    for (const auto &slot : sample_bank->slots)
        members.insert(slot.name);
    for (const auto &other : *sample_banks) {
        if (other.id == located->second)
            continue;
        const auto *other_sample_bank = std::get_if<CurrentSbac>(&other.decoded.payload);
        for (const auto &slot : other_sample_bank->slots) {
            if (members.contains(slot.name)) {
                return std::unexpected{transaction_error("another Sample Bank shares a Sample")};
            }
        }
    }
    for (const auto &slot : sample_bank->slots) {
        auto sample = category_object(state, partition, operation.volume_name, "SBNK", slot.name, "SBNK", cancellation);
        if (!sample)
            return std::unexpected{sample.error()};
        auto sample_payload = current_payload(state, partition, sample->second, cancellation);
        if (!sample_payload)
            return std::unexpected{sample_payload.error()};
        if (sample_payload->size() <= 0xd0U || (std::to_integer<std::uint8_t>((*sample_payload)[0xd0U]) & 1U) == 0U) {
            return std::unexpected{transaction_error("member Sample is missing its Sample Bank membership flag")};
        }
        if (auto updated = set_sbnk_sample_bank_flag(state, partition, sample->second, false, cancellation); !updated)
            return std::unexpected{updated.error()};
    }
    if (auto removed = remove_directory_entry(state, partition, located->first, located->second,
                                              operation.sample_bank_name, cancellation);
        !removed)
        return std::unexpected{removed.error()};
    auto freed = release_record(partition, located->second);
    if (!freed)
        return std::unexpected{freed.error()};
    std::erase_if(state.known_edges, [&](const auto &edge) {
        const auto &[edge_partition, source, target] = edge;
        return edge_partition == *partition_index && (source == located->second || target == located->second);
    });
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.sample_bank_name;
    report.removed_sfs_ids = {located->second};
    report.freed_clusters = *freed;
    return report;
}

Result<OperationReport> insert_sbac(TransactionState &state, const OperationView &operation,
                                    const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end() || !operation.sample_bank) {
        return std::unexpected{transaction_error("insert-sbac target is invalid")};
    }
    auto &partition = found->second;
    const auto &spec = *operation.sample_bank;
    auto directory = volume_category(state, partition, operation.volume_name, "SBAC", cancellation);
    if (!directory)
        return std::unexpected{directory.error()};
    auto existing_sample_banks =
        category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac, cancellation);
    if (!existing_sample_banks)
        return std::unexpected{existing_sample_banks.error()};
    std::set<std::string> existing_members;
    for (const auto &existing : *existing_sample_banks) {
        if (existing.name == spec.name) {
            return std::unexpected{transaction_error("Sample Bank already exists")};
        }
        const auto *sample_bank = std::get_if<CurrentSbac>(&existing.decoded.payload);
        for (const auto &slot : sample_bank->slots)
            existing_members.insert(slot.name);
    }
    std::map<std::string, SampleSpec> sample_specs;
    std::vector<SfsId> member_ids;
    for (const auto &name : spec.member_samples) {
        if (existing_members.contains(name)) {
            return std::unexpected{transaction_error("Sample already belongs to another Sample Bank")};
        }
        auto sample = category_object(state, partition, operation.volume_name, "SBNK", name, "SBNK", cancellation);
        if (!sample)
            return std::unexpected{sample.error()};
        auto sample_payload = current_payload(state, partition, sample->second, cancellation);
        if (!sample_payload)
            return std::unexpected{sample_payload.error()};
        auto decoded = decode_object(*sample_payload);
        if (!decoded)
            return std::unexpected{decoded.error()};
        const auto *current_sample = std::get_if<CurrentSbnk>(&decoded->payload);
        if (current_sample == nullptr || current_sample->right_slot_present ||
            (std::to_integer<std::uint8_t>((*sample_payload)[0xd0U]) & 1U) != 0U) {
            return std::unexpected{
                transaction_error("Sample Bank profile requires mono Samples without existing membership")};
        }
        SampleSpec placeholder;
        placeholder.name = name;
        sample_specs.emplace(name, std::move(placeholder));
        member_ids.push_back(sample->second);
    }
    auto payload = detail::prepare_sbac_payload(spec, sample_specs);
    if (!payload)
        return std::unexpected{payload.error()};
    auto allocated = allocate_record(partition, std::move(*payload), PayloadKind::object);
    if (!allocated)
        return std::unexpected{allocated.error()};
    for (const auto id : member_ids) {
        if (auto updated = set_sbnk_sample_bank_flag(state, partition, id, true, cancellation); !updated)
            return std::unexpected{updated.error()};
        state.known_edges.emplace_back(*partition_index, allocated->first, id);
    }
    if (auto appended = append_directory_entry(state, partition, *directory, allocated->first, spec.name, cancellation);
        !appended)
        return std::unexpected{appended.error()};
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = spec.name;
    report.inserted_sfs_ids = {allocated->first};
    report.allocated_clusters = allocated->second;
    return report;
}

void put_padded_name(std::span<std::byte> payload, std::size_t offset, std::string_view name) {
    std::fill(payload.begin() + static_cast<std::ptrdiff_t>(offset),
              payload.begin() + static_cast<std::ptrdiff_t>(offset + 16U), std::byte{' '});
    std::ranges::transform(name, payload.begin() + static_cast<std::ptrdiff_t>(offset),
                           [](char value) { return static_cast<std::byte>(value); });
}

Result<OperationReport> rename_waveform(TransactionState &state, const OperationView &operation,
                                        const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SMPL", operation.waveform_name, "SMPL", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto waveform_payload = current_payload(state, partition, located->second, cancellation);
    if (!waveform_payload)
        return std::unexpected{waveform_payload.error()};
    auto waveform_object = decode_object(*waveform_payload);
    if (!waveform_object)
        return std::unexpected{waveform_object.error()};
    const auto *wave_data = std::get_if<CurrentSmpl>(&waveform_object->payload);
    if (wave_data == nullptr || wave_data->link_id.value == 0U)
        return std::unexpected{transaction_error("waveform has no current link identity")};
    auto waveforms = category_objects(state, partition, operation.volume_name, "SMPL", ObjectType::smpl, cancellation);
    if (!waveforms)
        return std::unexpected{waveforms.error()};
    for (const auto &other : *waveforms) {
        if (other.id == located->second)
            continue;
        const auto *other_wave_data = std::get_if<CurrentSmpl>(&other.decoded.payload);
        if (other.name == operation.new_waveform_name || other_wave_data->link_id.value == wave_data->link_id.value) {
            return std::unexpected{transaction_error("waveform rename identity is not unique")};
        }
    }
    auto samples = category_objects(state, partition, operation.volume_name, "SBNK", ObjectType::sbnk, cancellation);
    if (!samples)
        return std::unexpected{samples.error()};
    std::set<SfsId> updated_samples;
    for (const auto &sample_row : *samples) {
        const auto *sample = std::get_if<CurrentSbnk>(&sample_row.decoded.payload);
        std::vector<std::size_t> offsets;
        const auto inspect = [&](const CurrentSbnkMember &member, std::size_t offset) -> Result<void> {
            const auto name_matches = member.wave_data_name == operation.waveform_name;
            const auto link_matches = member.smpl_link_id == wave_data->link_id.value;
            if (name_matches != link_matches)
                return std::unexpected{transaction_error("SBNK waveform name and link identity disagree")};
            if (name_matches)
                offsets.push_back(offset);
            return {};
        };
        if (auto checked = inspect(sample->left, 0x78U); !checked)
            return std::unexpected{checked.error()};
        if (sample->right) {
            if (auto checked = inspect(*sample->right, 0x88U); !checked)
                return std::unexpected{checked.error()};
        }
        if (!offsets.empty()) {
            auto payload = sample_row.payload;
            for (const auto offset : offsets)
                put_padded_name(payload, offset, operation.new_waveform_name);
            if (auto replaced =
                    replace_fixed_object_payload(state, partition, sample_row.id, std::move(payload), cancellation);
                !replaced)
                return std::unexpected{replaced.error()};
            updated_samples.insert(sample_row.id);
        }
    }
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        if (edge_partition == *partition_index && target == located->second && !updated_samples.contains(source))
            return std::unexpected{transaction_error("known waveform references exceed exact rename set")};
    }
    if (auto renamed = rename_object_payload(state, partition, located->second, operation.waveform_name,
                                             operation.new_waveform_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    if (auto renamed = rename_directory_entry(state, partition, located->first, located->second,
                                              operation.waveform_name, operation.new_waveform_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.new_waveform_name;
    return report;
}

Result<OperationReport> rename_sbac(TransactionState &state, const OperationView &operation,
                                    const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto located = category_object(state, partition, operation.volume_name, "SBAC", operation.sample_bank_name, "SBAC",
                                   cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto sample_banks =
        category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac, cancellation);
    if (!sample_banks)
        return std::unexpected{sample_banks.error()};
    if (std::ranges::any_of(*sample_banks, [&](const auto &sample_bank) {
            return sample_bank.id != located->second && sample_bank.name == operation.new_sample_bank_name;
        }))
        return std::unexpected{transaction_error("Sample Bank rename destination exists")};
    auto sample_bank_payload = current_payload(state, partition, located->second, cancellation);
    if (!sample_bank_payload)
        return std::unexpected{sample_bank_payload.error()};
    auto sample_bank_object = decode_object(*sample_bank_payload);
    if (!sample_bank_object)
        return std::unexpected{sample_bank_object.error()};
    const auto *sample_bank = std::get_if<CurrentSbac>(&sample_bank_object->payload);
    if (sample_bank == nullptr || sample_bank->slots.empty() || sample_bank->slots.size() > 3U ||
        sample_bank->slots.size() != sample_bank->active_slot_count)
        return std::unexpected{transaction_error("SBAC rename requires 1..3 readable slots")};
    std::set<SfsId> member_ids;
    for (const auto &slot : sample_bank->slots) {
        if (slot.raw_handle != 0U)
            return std::unexpected{transaction_error("SBAC member has unsupported nonzero handle")};
        auto member = category_object(state, partition, operation.volume_name, "SBNK", slot.name, "SBNK", cancellation);
        if (!member)
            return std::unexpected{member.error()};
        auto member_payload = current_payload(state, partition, member->second, cancellation);
        if (!member_payload)
            return std::unexpected{member_payload.error()};
        if (member_payload->size() <= 0xd0U || (std::to_integer<std::uint8_t>((*member_payload)[0xd0U]) & 1U) == 0U) {
            return std::unexpected{transaction_error("Sample Bank member is missing its membership flag")};
        }
        member_ids.insert(member->second);
    }
    for (const auto &other : *sample_banks) {
        if (other.id == located->second)
            continue;
        const auto *other_sample_bank = std::get_if<CurrentSbac>(&other.decoded.payload);
        if (std::ranges::any_of(other_sample_bank->slots, [&](const SbacSlot &slot) {
                return std::ranges::any_of(sample_bank->slots,
                                           [&](const SbacSlot &own) { return own.name == slot.name; });
            })) {
            return std::unexpected{transaction_error("another SBAC shares a rename member")};
        }
    }
    std::set<SfsId> known_members;
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        if (edge_partition == *partition_index && source == located->second) {
            known_members.insert(target);
        }
    }
    if (known_members != member_ids) {
        return std::unexpected{transaction_error("SBAC raw members disagree with known edges")};
    }
    auto programs = category_objects(state, partition, operation.volume_name, "PROG", ObjectType::prog, cancellation);
    if (!programs)
        return std::unexpected{programs.error()};
    std::set<SfsId> updated_programs;
    for (const auto &program_row : *programs) {
        const auto *program = std::get_if<CurrentProg>(&program_row.decoded.payload);
        auto payload = program_row.payload;
        bool changed{};
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
            const auto &assignment = program->assignments[index];
            if (assignment.kind != 0x11U)
                continue;
            if (assignment.name == operation.new_sample_bank_name)
                return std::unexpected{transaction_error("Program already assigns rename destination")};
            if (assignment.name != operation.sample_bank_name)
                continue;
            if (assignment.raw_handle != 0U)
                return std::unexpected{transaction_error("Program assignment has unsupported nonzero handle")};
            put_padded_name(payload, 0x120U + index * 0x38U, operation.new_sample_bank_name);
            changed = true;
        }
        if (changed) {
            if (auto replaced =
                    replace_fixed_object_payload(state, partition, program_row.id, std::move(payload), cancellation);
                !replaced)
                return std::unexpected{replaced.error()};
            updated_programs.insert(program_row.id);
        }
    }
    std::set<SfsId> known_programs;
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        if (edge_partition == *partition_index && target == located->second) {
            known_programs.insert(source);
        }
    }
    if (known_programs != updated_programs) {
        return std::unexpected{transaction_error("SBAC raw Program references disagree with known edges")};
    }
    if (auto renamed = rename_object_payload(state, partition, located->second, operation.sample_bank_name,
                                             operation.new_sample_bank_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    if (auto renamed = rename_directory_entry(state, partition, located->first, located->second,
                                              operation.sample_bank_name, operation.new_sample_bank_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.new_sample_bank_name;
    return report;
}

Result<OperationReport> rename_sbnk(TransactionState &state, const OperationView &operation,
                                    const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SBNK", operation.sample_name, "SBNK", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto samples = category_objects(state, partition, operation.volume_name, "SBNK", ObjectType::sbnk, cancellation);
    if (!samples)
        return std::unexpected{samples.error()};
    if (std::ranges::any_of(*samples, [&](const auto &sample) {
            return sample.id != located->second && sample.name == operation.new_sample_name;
        }))
        return std::unexpected{transaction_error("SBNK rename destination exists")};
    auto sample_payload = current_payload(state, partition, located->second, cancellation);
    if (!sample_payload)
        return std::unexpected{sample_payload.error()};
    auto sample_object = decode_object(*sample_payload);
    if (!sample_object)
        return std::unexpected{sample_object.error()};
    const auto *sample = std::get_if<CurrentSbnk>(&sample_object->payload);
    if (sample == nullptr)
        return std::unexpected{transaction_error("SBNK is unreadable")};
    const auto banked = (std::to_integer<std::uint8_t>((*sample_payload)[0xd0U]) & 1U) != 0U;

    auto sample_banks =
        category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac, cancellation);
    if (!sample_banks)
        return std::unexpected{sample_banks.error()};
    std::set<SfsId> sample_bank_references;
    for (const auto &sample_bank_row : *sample_banks) {
        const auto *sample_bank = std::get_if<CurrentSbac>(&sample_bank_row.decoded.payload);
        auto payload = sample_bank_row.payload;
        bool changed{};
        for (const auto &slot : sample_bank->slots) {
            if (slot.name == operation.new_sample_name)
                return std::unexpected{transaction_error("SBAC already references SBNK rename destination")};
            if (slot.name != operation.sample_name)
                continue;
            if (slot.raw_handle != 0U)
                return std::unexpected{transaction_error("SBAC member has unsupported nonzero handle")};
            put_padded_name(payload, slot.offset, operation.new_sample_name);
            changed = true;
        }
        if (changed) {
            if (auto replaced = replace_fixed_object_payload(state, partition, sample_bank_row.id, std::move(payload),
                                                             cancellation);
                !replaced)
                return std::unexpected{replaced.error()};
            sample_bank_references.insert(sample_bank_row.id);
        }
    }
    if (sample_bank_references.size() != (banked ? 1U : 0U))
        return std::unexpected{transaction_error("Sample membership flag disagrees with exact Sample Bank membership")};

    auto programs = category_objects(state, partition, operation.volume_name, "PROG", ObjectType::prog, cancellation);
    if (!programs)
        return std::unexpected{programs.error()};
    std::set<SfsId> program_references;
    std::set<std::uint8_t> direct_numbers;
    for (const auto &program_row : *programs) {
        int number{};
        const auto parsed_number =
            std::from_chars(program_row.name.data(), program_row.name.data() + program_row.name.size(), number);
        if (parsed_number.ec != std::errc{} || parsed_number.ptr != program_row.name.data() + program_row.name.size()) {
            return std::unexpected{transaction_error("Program slot name is unsupported")};
        }
        if (number < 1 || number > 128 || std::format("{:03}", number) != program_row.name)
            return std::unexpected{transaction_error("Program slot name is unsupported")};
        const auto *program = std::get_if<CurrentProg>(&program_row.decoded.payload);
        auto payload = program_row.payload;
        bool changed{};
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
            const auto &assignment = program->assignments[index];
            if (assignment.kind != 0x10U)
                continue;
            if (assignment.name == operation.new_sample_name)
                return std::unexpected{transaction_error("Program already assigns SBNK rename destination")};
            if (assignment.name != operation.sample_name)
                continue;
            if (assignment.raw_handle != 0U)
                return std::unexpected{transaction_error("Program assignment has unsupported nonzero handle")};
            put_padded_name(payload, 0x120U + index * 0x38U, operation.new_sample_name);
            changed = true;
        }
        if (changed) {
            if (auto replaced =
                    replace_fixed_object_payload(state, partition, program_row.id, std::move(payload), cancellation);
                !replaced)
                return std::unexpected{replaced.error()};
            program_references.insert(program_row.id);
            direct_numbers.insert(static_cast<std::uint8_t>(number));
        }
    }
    std::set<std::uint8_t> bitmap_numbers(sample->linked_program_numbers.begin(), sample->linked_program_numbers.end());
    if (bitmap_numbers != direct_numbers)
        return std::unexpected{transaction_error("SBNK Program bitmap disagrees with exact Program assignments")};
    std::set<SfsId> expected = sample_bank_references;
    expected.insert(program_references.begin(), program_references.end());
    std::set<SfsId> known_incoming;
    for (const auto &[edge_partition, source, target] : state.known_edges) {
        if (edge_partition == *partition_index && target == located->second) {
            known_incoming.insert(source);
        }
    }
    if (known_incoming != expected) {
        return std::unexpected{transaction_error("SBNK raw references disagree with known edges")};
    }
    if (auto renamed = rename_object_payload(state, partition, located->second, operation.sample_name,
                                             operation.new_sample_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    if (auto renamed = rename_directory_entry(state, partition, located->first, located->second, operation.sample_name,
                                              operation.new_sample_name, cancellation);
        !renamed)
        return std::unexpected{renamed.error()};
    OperationReport report;
    report.id = operation.id;
    report.type = operation.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.new_sample_name;
    return report;
}

Result<void> write_bytes(const std::filesystem::path &output, std::uint64_t offset, std::span<const std::byte> data) {
    return detail::write_temporary_file_at(output, offset, data);
}

Result<void> write_continuation_lists(const std::filesystem::path &output, const Partition &partition,
                                      const MutablePartition::InsertedRecord &record) {
    constexpr std::size_t extents_per_cluster = (1024U - 12U) / 12U;
    for (std::size_t list_index = 0; list_index < record.continuation_clusters.size(); ++list_index) {
        const auto extent_begin = list_index * extents_per_cluster;
        const auto extent_count = std::min(extents_per_cluster, record.extents.size() - extent_begin);
        std::vector<std::byte> block(1024U);
        put_be32(block, 0U, static_cast<std::uint32_t>(extent_count));
        const auto next =
            list_index + 1U < record.continuation_clusters.size() ? record.continuation_clusters[list_index + 1U] : 0U;
        put_be32(block, 8U, next);
        for (std::size_t index = 0; index < extent_count; ++index) {
            const auto &extent = record.extents[extent_begin + index];
            const auto offset = 12U + index * 12U;
            put_be32(block, offset, extent.cluster_offset);
            put_be32(block, offset + 4U, extent.cluster_count);
            put_be32(block, offset + 8U, extent.byte_count);
        }
        const auto absolute =
            (static_cast<std::uint64_t>(partition.start_sector) +
             static_cast<std::uint64_t>(record.continuation_clusters[list_index]) * partition.sectors_per_cluster) *
            512U;
        if (auto written = write_bytes(output, absolute, block); !written) {
            return written;
        }
    }
    return {};
}

Result<std::filesystem::path> copy_to_unique_temporary(const std::filesystem::path &source,
                                                       const std::filesystem::path &output) {
    auto input = FileReader::open(source);
    if (!input)
        return std::unexpected{input.error()};
    auto temporary = detail::reserve_temporary_file(output);
    if (!temporary)
        return std::unexpected{temporary.error()};
    if (auto resized = detail::resize_temporary_file(*temporary, (*input)->size()); !resized) {
        detail::discard_temporary_file(*temporary);
        return std::unexpected{resized.error()};
    }
    std::vector<std::byte> buffer(static_cast<std::size_t>(std::min<std::uint64_t>((*input)->size(), 1024U * 1024U)));
    for (std::uint64_t offset = 0U; offset < (*input)->size(); offset += buffer.size()) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), (*input)->size() - offset));
        auto bytes = std::span{buffer}.first(count);
        if (auto read = (*input)->read_exact_at(offset, bytes); !read) {
            detail::discard_temporary_file(*temporary);
            return std::unexpected{read.error()};
        }
        if (auto written = detail::write_temporary_file_at(*temporary, offset, bytes); !written) {
            detail::discard_temporary_file(*temporary);
            return std::unexpected{written.error()};
        }
    }
    return temporary;
}

Result<void> flush_file_to_disk(const std::filesystem::path &path) { return detail::flush_file_to_disk(path); }

Result<void> publish_temporary(const std::filesystem::path &temporary, const std::filesystem::path &output,
                               bool overwrite) {
    return detail::publish_temporary_file(temporary, output, overwrite);
}

Result<void> validate_temporary(const std::filesystem::path &temporary, const TransactionState &state,
                                const CancellationToken &cancellation) {
    OpenOptions options;
    options.cancellation = cancellation;
    auto actual = open_image(temporary, options);
    if (!actual)
        return std::unexpected{actual.error()};
    for (const auto &[index, expected] : state.partitions) {
        const auto partition = std::ranges::find(actual->partitions(), PartitionIndex{index}, &Partition::index);
        if (partition == actual->partitions().end())
            return std::unexpected{transaction_error("post-write validation lost a planned partition")};
        if (partition->allocation.conflicting_cluster_count != 0U ||
            partition->allocation.invalid_extent_record_count != 0U ||
            partition->allocation.extent_total_mismatch_count != 0U ||
            !partition->allocation.stored_not_reconstructed.empty() ||
            !partition->allocation.reconstructed_not_stored.empty()) {
            return std::unexpected{transaction_error("post-write allocation validation is not clean")};
        }
        if (expected.renamed_name && (partition->name != *expected.renamed_name || !partition->backup_header_matches)) {
            return std::unexpected{transaction_error("post-write partition name differs from the transaction plan")};
        }
        std::set<SfsId> expected_ids;
        for (const auto &source_record : expected.source->records) {
            if (!expected.deleted.contains(source_record.sfs_id))
                expected_ids.insert(source_record.sfs_id);
        }
        for (const auto &[id, inserted] : expected.inserted) {
            static_cast<void>(inserted);
            expected_ids.insert(id);
        }
        std::set<SfsId> actual_ids;
        for (const auto &actual_record : partition->records)
            actual_ids.insert(actual_record.sfs_id);
        if (actual_ids != expected_ids)
            return std::unexpected{transaction_error("post-write SFS record set differs from the transaction plan")};

        const auto compare_payload = [&](SfsId id, std::span<const std::byte> payload) -> Result<void> {
            auto written = actual->read_record_data(partition->index, id, 64U * 1024U * 1024U, cancellation);
            if (!written)
                return std::unexpected{written.error()};
            if (!std::ranges::equal(*written, payload))
                return std::unexpected{transaction_error("post-write object payload differs from "
                                                         "the transaction plan")};
            return {};
        };
        if (expected.root_payload) {
            if (auto compared = compare_payload(SfsId{1}, *expected.root_payload); !compared)
                return compared;
        }
        for (const auto &[id, changed] : expected.changed) {
            if (auto compared = compare_payload(id, changed.payload); !compared)
                return compared;
        }
        for (const auto &[id, inserted] : expected.inserted) {
            if (auto compared = compare_payload(id, inserted.payload); !compared)
                return compared;
        }
        for (const auto &source_record : expected.source->records) {
            if (expected.deleted.contains(source_record.sfs_id) || expected.changed.contains(source_record.sfs_id) ||
                expected.inserted.contains(source_record.sfs_id) ||
                (source_record.sfs_id.value == 1U && expected.root_payload)) {
                continue;
            }
            const auto written_record =
                std::ranges::find(partition->records, source_record.sfs_id, &IndexRecord::sfs_id);
            if (written_record == partition->records.end())
                return std::unexpected{transaction_error("post-write validation lost an unchanged SFS record")};
            auto source_index = read_raw(state.container.source_path(), source_record.record_offset.value, 72U);
            if (!source_index)
                return std::unexpected{source_index.error()};
            auto written_index = read_raw(temporary, written_record->record_offset.value, 72U);
            if (!written_index)
                return std::unexpected{written_index.error()};
            if (*source_index != *written_index)
                return std::unexpected{transaction_error("post-write validation changed untouched SFS ID " +
                                                         std::to_string(source_record.sfs_id.value) + " index record")};
            constexpr std::size_t comparison_chunk_size = 1024U * 1024U;
            for (std::uint64_t offset = 0U; offset < source_record.data_size;) {
                const auto count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(comparison_chunk_size, source_record.data_size - offset));
                auto source_data = state.container.read_record_range(expected.source->index, source_record.sfs_id,
                                                                     offset, count, cancellation);
                if (!source_data)
                    return std::unexpected{source_data.error()};
                auto written_data =
                    actual->read_record_range(partition->index, source_record.sfs_id, offset, count, cancellation);
                if (!written_data)
                    return std::unexpected{written_data.error()};
                if (*source_data != *written_data)
                    return std::unexpected{transaction_error("post-write validation changed untouched SFS ID " +
                                                             std::to_string(source_record.sfs_id.value) + " payload")};
                offset += count;
            }
        }
        const auto bitmap_size_u64 = (static_cast<std::uint64_t>(partition->cluster_count) + 7U) / 8U;
        if (bitmap_size_u64 > std::numeric_limits<std::size_t>::max())
            return std::unexpected{transaction_error("post-write allocation bitmap exceeds platform limits")};
        const auto bitmap_size = static_cast<std::size_t>(bitmap_size_u64);
        const auto bitmap_offset =
            (static_cast<std::uint64_t>(partition->start_sector) +
             static_cast<std::uint64_t>(partition->bitmap_cluster) * partition->sectors_per_cluster) *
            512U;
        auto bitmap = read_raw(temporary, bitmap_offset, bitmap_size);
        if (!bitmap)
            return std::unexpected{bitmap.error()};
        if (*bitmap != expected.bitmap)
            return std::unexpected{transaction_error("post-write allocation bitmap differs from "
                                                     "the transaction plan")};
    }
    return {};
}

Result<TransactionState> open_transaction_state(const std::filesystem::path &source_path,
                                                const CancellationToken &cancellation, ProgressSink *progress,
                                                bool include_object_graph) {
    OpenOptions options;
    options.cancellation = cancellation;
    options.progress = progress;
    auto container = open_image(source_path, options);
    if (!container)
        return std::unexpected{container.error()};
    TransactionState state{std::move(*container), {}, {}, {}, {}, {}};
    if (include_object_graph) {
        auto catalog = build_object_catalog(state.container, 64U * 1024U * 1024U, cancellation);
        if (!catalog)
            return std::unexpected{catalog.error()};
        state.catalog = std::move(*catalog);
        state.graph = build_relationship_graph(state.catalog);
    }
    std::map<std::string, std::pair<PartitionIndex, SfsId>> object_ids;
    for (const auto &object : state.catalog.objects)
        object_ids.emplace(object.key, std::pair{object.partition, object.sfs_id});
    for (const auto &relationship : state.graph.relationships) {
        if (relationship.quality != RelationshipQuality::known || !relationship.target_key)
            continue;
        const auto source = object_ids.find(relationship.source_key);
        const auto target = object_ids.find(*relationship.target_key);
        if (source != object_ids.end() && target != object_ids.end() && source->second.first == target->second.first) {
            state.known_edges.emplace_back(source->second.first, source->second.second, target->second.second);
        }
    }
    for (const auto &partition : state.container.partitions()) {
        if (partition.allocation.conflicting_cluster_count != 0U ||
            partition.allocation.invalid_extent_record_count != 0U ||
            partition.allocation.extent_total_mismatch_count != 0U ||
            !partition.allocation.stored_not_reconstructed.empty() ||
            !partition.allocation.reconstructed_not_stored.empty()) {
            return std::unexpected{transaction_error("source allocation cannot safely support alteration")};
        }
        const auto bitmap_size_u64 = (static_cast<std::uint64_t>(partition.cluster_count) + 7U) / 8U;
        if (bitmap_size_u64 > std::numeric_limits<std::size_t>::max())
            return std::unexpected{transaction_error("source allocation bitmap exceeds platform limits")};
        const auto bitmap_size = static_cast<std::size_t>(bitmap_size_u64);
        const auto bitmap_offset =
            (static_cast<std::uint64_t>(partition.start_sector) +
             static_cast<std::uint64_t>(partition.bitmap_cluster) * partition.sectors_per_cluster) *
            512U;
        auto bitmap = read_raw(source_path, bitmap_offset, bitmap_size);
        if (!bitmap)
            return std::unexpected{bitmap.error()};
        MutablePartition mutable_partition;
        mutable_partition.source = &partition;
        mutable_partition.bitmap = std::move(*bitmap);
        state.partitions.emplace(partition.index.value, std::move(mutable_partition));
    }
    return state;
}

Result<void> publish(const TransactionState &state, const std::filesystem::path &output_path,
                     const CancellationToken &cancellation, bool overwrite = false,
                     const std::function<Result<void>(const std::filesystem::path &)> &temporary_validator = {},
                     ProgressSink *progress = nullptr) {
    if (!overwrite && std::filesystem::exists(output_path))
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "alteration output already exists")};
    std::error_code error;
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), error);
    if (error)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create alteration output directory")};
    if (progress) {
        progress->report(
            {ProgressPhase::writing, 0U, state.partitions.size(), "writing alteration image", output_path.string()});
    }
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    auto temporary_result = copy_to_unique_temporary(state.container.source_path(), output_path);
    if (!temporary_result)
        return std::unexpected{temporary_result.error()};
    const auto temporary = std::move(*temporary_result);
    const auto cleanup = [&]() { detail::discard_temporary_file(temporary); };
    std::uint64_t completed_partitions{};
    for (const auto &[index, item] : state.partitions) {
        static_cast<void>(index);
        if (const auto check = cancellation.check(); !check) {
            cleanup();
            return std::unexpected{check.error()};
        }
        const auto &partition = *item.source;
        if (item.renamed_name) {
            std::array<std::byte, 16> encoded_name{};
            std::ranges::fill(encoded_name, std::byte{' '});
            for (std::size_t name_index = 0; name_index < item.renamed_name->size(); ++name_index)
                encoded_name[name_index] = static_cast<std::byte>((*item.renamed_name)[name_index]);
            const auto header_offset = static_cast<std::uint64_t>(partition.start_sector) * 512U + 0x40U;
            if (auto written = write_bytes(temporary, header_offset, encoded_name); !written) {
                cleanup();
                return written;
            }
            if (auto written = write_bytes(temporary, header_offset + 1024U, encoded_name); !written) {
                cleanup();
                return written;
            }
        }
        for (const auto id : item.deleted) {
            const auto *source_record = record(partition, id);
            if (source_record == nullptr)
                continue;
            std::array<std::byte, 72> zero{};
            if (auto written = write_bytes(temporary, source_record->record_offset.value, zero); !written) {
                cleanup();
                return written;
            }
        }
        if (item.root_index && item.root_payload) {
            const auto *root = record(partition, SfsId{1});
            if (auto written = write_bytes(temporary, root->record_offset.value, *item.root_index); !written) {
                cleanup();
                return written;
            }
            const auto payload_offset =
                (static_cast<std::uint64_t>(partition.start_sector) +
                 static_cast<std::uint64_t>(root->extents[0].cluster_offset) * partition.sectors_per_cluster) *
                512U;
            if (auto written = write_bytes(temporary, payload_offset, *item.root_payload); !written) {
                cleanup();
                return written;
            }
        }
        const auto index_base =
            (static_cast<std::uint64_t>(partition.start_sector) +
             static_cast<std::uint64_t>(partition.directory_index_cluster) * partition.sectors_per_cluster) *
            512U;
        for (const auto &[id, changed] : item.changed) {
            const auto *source_record = record(partition, id);
            if (source_record == nullptr) {
                cleanup();
                return std::unexpected{transaction_error("changed record has no source index location")};
            }
            if (auto written = write_bytes(temporary, source_record->record_offset.value, changed.raw_index);
                !written) {
                cleanup();
                return written;
            }
            std::size_t payload_offset{};
            for (const auto &extent : changed.extents) {
                const auto capacity = static_cast<std::size_t>(extent.cluster_count) * 1024U;
                const auto count = std::min(capacity, changed.payload.size() - payload_offset);
                const auto absolute =
                    (static_cast<std::uint64_t>(partition.start_sector) +
                     static_cast<std::uint64_t>(extent.cluster_offset) * partition.sectors_per_cluster) *
                    512U;
                if (auto written =
                        write_bytes(temporary, absolute, std::span{changed.payload}.subspan(payload_offset, count));
                    !written) {
                    cleanup();
                    return written;
                }
                payload_offset += count;
                if (payload_offset == changed.payload.size())
                    break;
            }
            if (auto written = write_continuation_lists(temporary, partition, changed); !written) {
                cleanup();
                return written;
            }
        }
        for (const auto &[id, inserted] : item.inserted) {
            const auto index_offset = index_base + (id.value / 14U) * 1024U + (id.value % 14U) * 72U;
            if (auto written = write_bytes(temporary, index_offset, inserted.raw_index); !written) {
                cleanup();
                return written;
            }
            std::size_t payload_offset{};
            for (const auto &extent : inserted.extents) {
                const auto capacity = static_cast<std::size_t>(extent.cluster_count) * 1024U;
                const auto count = std::min(capacity, inserted.payload.size() - payload_offset);
                const auto absolute =
                    (static_cast<std::uint64_t>(partition.start_sector) +
                     static_cast<std::uint64_t>(extent.cluster_offset) * partition.sectors_per_cluster) *
                    512U;
                if (auto written =
                        write_bytes(temporary, absolute, std::span{inserted.payload}.subspan(payload_offset, count));
                    !written) {
                    cleanup();
                    return written;
                }
                payload_offset += count;
            }
            if (auto written = write_continuation_lists(temporary, partition, inserted); !written) {
                cleanup();
                return written;
            }
        }
        const auto bitmap_offset =
            (static_cast<std::uint64_t>(partition.start_sector) +
             static_cast<std::uint64_t>(partition.bitmap_cluster) * partition.sectors_per_cluster) *
            512U;
        if (auto written = write_bytes(temporary, bitmap_offset, item.bitmap); !written) {
            cleanup();
            return written;
        }
        const auto mirror_offset = static_cast<std::uint64_t>(partition.start_sector) * 512U + 2048U;
        if (auto written = write_bytes(temporary, mirror_offset,
                                       std::span{item.bitmap}.first(std::min<std::size_t>(512, item.bitmap.size())));
            !written) {
            cleanup();
            return written;
        }
        ++completed_partitions;
        if (progress) {
            progress->report({ProgressPhase::writing, completed_partitions, state.partitions.size(),
                              "writing alteration image", output_path.string()});
        }
    }
    if (auto flushed = flush_file_to_disk(temporary); !flushed) {
        cleanup();
        return std::unexpected{flushed.error()};
    }
    if (const auto check = cancellation.check(); !check) {
        cleanup();
        return std::unexpected{check.error()};
    }
    auto validated = validate_temporary(temporary, state, cancellation);
    if (!validated) {
        cleanup();
        return std::unexpected{validated.error()};
    }
    if (temporary_validator) {
        auto package_validated = temporary_validator(temporary);
        if (!package_validated) {
            cleanup();
            return std::unexpected{package_validated.error()};
        }
    }
    if (const auto check = cancellation.check(); !check) {
        cleanup();
        return std::unexpected{check.error()};
    }
    if (auto published = publish_temporary(temporary, output_path, overwrite); !published) {
        cleanup();
        return std::unexpected{published.error()};
    }
    return {};
}

bool has_action(const PlannedPackageObject &object, PackageImportObjectAction action) {
    return std::ranges::contains(object.actions, action);
}

const PackageNode *package_node(const PortablePackage &package, std::string_view node_id) {
    const auto found = std::ranges::find(package.nodes, node_id, &PackageNode::node_id);
    return found == package.nodes.end() ? nullptr : &*found;
}

const PlannedPackageObject *planned_node(const PackageImportPlan &plan, const PlannedPackageObject &owner,
                                         std::string_view node_id) {
    const auto found = std::ranges::find_if(plan.objects, [&](const auto &candidate) {
        return candidate.package_index == owner.package_index && candidate.root_index == owner.root_index &&
               candidate.node_id == node_id && candidate.partition_index == owner.partition_index &&
               candidate.group_name == owner.group_name && candidate.volume_name == owner.volume_name &&
               candidate.raw_group == owner.raw_group && candidate.raw_volume == owner.raw_volume;
    });
    return found == plan.objects.end() ? nullptr : &*found;
}

Result<package_internal::PackageNodeRelocationContext>
relocation_context(const PortablePackage &package, const PackageImportPlan &plan, const PlannedPackageObject &owner) {
    package_internal::PackageNodeRelocationContext context;
    context.destination_name = owner.destination_name;
    context.smpl_link_id = owner.target_link_id;
    context.linked_program_numbers = owner.target_program_numbers;
    context.sample_bank_member = owner.target_sample_bank_member;
    for (const auto &edge : package.relationships) {
        if (edge.source_node_id != owner.node_id)
            continue;
        const auto *target = planned_node(plan, owner, edge.target_node_id);
        if (target == nullptr) {
            return std::unexpected{transaction_error("package import plan omits a relationship target action")};
        }
        context.edge_target_names.emplace(edge.edge_id, target->destination_name);
        if (target->target_link_id)
            context.edge_target_link_ids.emplace(edge.edge_id, *target->target_link_id);
    }
    return context;
}

Result<std::string> normalized_payload_digest(std::span<const std::byte> payload) {
    auto decoded = decode_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    auto profile = package_internal::build_relocation_profile(*decoded, payload);
    if (!profile)
        return std::unexpected{profile.error()};
    return package_internal::hex_digest(package_internal::sha256(profile->normalized_payload));
}

std::optional<std::pair<std::string, std::string>> package_iso_raw_scope(const ObjectSnapshot &snapshot) {
    if (!snapshot.placement)
        return std::nullopt;
    const auto &path = snapshot.placement->container_directory;
    const auto separator = path.find('/');
    if (separator == std::string::npos || path.find('/', separator + 1U) != std::string::npos)
        return std::nullopt;
    return std::pair{path.substr(0, separator), path.substr(separator + 1U)};
}

Result<void> validate_flat_package_result(const std::filesystem::path &temporary,
                                          std::span<const PortablePackage> packages, const PackageImportPlan &plan,
                                          const CancellationToken &cancellation) {
    auto media = open_media(temporary, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (media->kind() != plan.target_kind)
        return std::unexpected{transaction_error("package result reopened as the wrong media kind")};
    auto catalog = build_object_catalog(*media, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected{catalog.error()};
    const auto graph = build_relationship_graph(*catalog);
    std::map<std::string, const ObjectSnapshot *, std::less<>> actual_by_action;
    for (const auto &object : plan.objects) {
        std::vector<const ObjectSnapshot *> matches;
        for (const auto &snapshot : catalog->objects) {
            const auto scope = package_iso_raw_scope(snapshot);
            const auto scope_matches =
                plan.target_kind != MediaKind::iso9660 ||
                (scope && scope->first == object.raw_group && scope->second == object.raw_volume);
            if (snapshot.object.header.raw_type == object.object_type &&
                snapshot.object.header.name == object.destination_name && scope_matches) {
                matches.push_back(&snapshot);
            }
        }
        if (matches.size() != 1U) {
            return std::unexpected{transaction_error("post-write package object is not unique in its media scope")};
        }
        auto normalized = normalized_payload_digest(matches.front()->raw_payload);
        if (!normalized)
            return std::unexpected{normalized.error()};
        if (*normalized != object.normalized_sha256)
            return std::unexpected{transaction_error("post-write package object changed identity")};
        if (object.target_link_id) {
            const auto *wave_data = std::get_if<CurrentSmpl>(&matches.front()->object.payload);
            if (wave_data == nullptr || wave_data->link_id.value != *object.target_link_id) {
                return std::unexpected{transaction_error("post-write SMPL link ID differs from the import plan")};
            }
        }
        if (object.object_type == "SBNK") {
            const auto *sample = std::get_if<CurrentSbnk>(&matches.front()->object.payload);
            if (sample == nullptr || sample->linked_program_numbers != object.target_program_numbers ||
                (((sample->sample_flags & 1U) != 0U) != object.target_sample_bank_member)) {
                return std::unexpected{transaction_error("post-write SBNK graph metadata differs "
                                                         "from the import plan")};
            }
        }
        actual_by_action.emplace(object.action_id, matches.front());
    }
    for (const auto &owner : plan.objects) {
        const auto &package = packages[owner.package_index];
        for (const auto &edge : package.relationships) {
            if (edge.source_node_id != owner.node_id)
                continue;
            const auto *target_plan = planned_node(plan, owner, edge.target_node_id);
            if (target_plan == nullptr)
                return std::unexpected{transaction_error("post-write edge has no target action")};
            const auto source = actual_by_action.find(owner.action_id);
            const auto target = actual_by_action.find(target_plan->action_id);
            if (source == actual_by_action.end() || target == actual_by_action.end())
                return std::unexpected{transaction_error("post-write edge endpoint is missing")};
            const auto relationships = graph.children(source->second->key);
            if (std::ranges::find_if(relationships, [&](const Relationship *actual) {
                    return actual->type == edge.role && actual->quality == RelationshipQuality::known &&
                           actual->target_key && *actual->target_key == target->second->key;
                }) == relationships.end()) {
                return std::unexpected{
                    transaction_error(std::format("post-write {} relationship from {} to {} "
                                                  "differs from the package plan",
                                                  edge.role, owner.destination_name, target_plan->destination_name))};
            }
        }
    }
    return {};
}

Result<PackageImportReport> apply_fat12_package_import(const std::filesystem::path &target_path,
                                                       std::span<const PortablePackage> packages,
                                                       const PackageImportPlan &plan,
                                                       const std::filesystem::path &output_path, bool overwrite,
                                                       const CancellationToken &cancellation, ProgressSink *progress) {
    auto media = open_media(target_path, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (media->kind() != MediaKind::fat12_floppy)
        return std::unexpected{transaction_error("FAT12 package target changed media kind")};
    auto media_objects = media->objects(64U * 1024U * 1024U, cancellation);
    if (!media_objects)
        return std::unexpected{media_objects.error()};

    detail::PreparedMediaImage prepared;
    prepared.manifest.schema_version = "1.1";
    prepared.manifest.format = MediaImageFormat::fat12_floppy;
    std::map<std::string, std::size_t, std::less<>> object_indices;
    std::set<std::string, std::less<>> object_paths;
    for (const auto &object : *media_objects) {
        object_indices.emplace(object.key, prepared.objects.size());
        object_paths.insert(object.logical_path);
        prepared.objects.push_back({object.decoded.header.type, object.decoded.header.name, object.raw_payload});
    }
    const auto &fat = std::get<FatImage>(media->storage());
    for (const auto &file : fat.files()) {
        if (object_paths.contains(file.path))
            continue;
        auto payload = fat.read_file(file, cancellation);
        if (!payload)
            return std::unexpected{payload.error()};
        prepared.retained_files.push_back({file.path, std::move(*payload)});
    }

    std::set<std::string, std::less<>> updated_physical_objects;
    std::uint64_t completed{};
    for (const auto &object : plan.objects) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        if (!object.canonical_action_id) {
            const auto physical_key =
                object.existing_object_key ? "existing:" + *object.existing_object_key : "planned:" + object.action_id;
            if (updated_physical_objects.insert(physical_key).second &&
                (std::ranges::contains(object.actions, PackageImportObjectAction::insert) ||
                 std::ranges::contains(object.actions, PackageImportObjectAction::relocate))) {
                const auto &package = packages[object.package_index];
                const auto *node = package_node(package, object.node_id);
                if (node == nullptr)
                    return std::unexpected{transaction_error("FAT12 package node is missing")};
                auto context = relocation_context(package, plan, object);
                if (!context)
                    return std::unexpected{context.error()};
                auto payload = package_internal::relocate_package_node(package, *node, *context);
                if (!payload)
                    return std::unexpected{payload.error()};
                auto decoded = decode_object(*payload);
                if (!decoded)
                    return std::unexpected{decoded.error()};
                if (object.existing_object_key) {
                    const auto existing = object_indices.find(*object.existing_object_key);
                    if (existing == object_indices.end())
                        return std::unexpected{transaction_error("planned FAT12 reused object is absent")};
                    prepared.objects[existing->second] = {decoded->header.type, object.destination_name,
                                                          std::move(*payload)};
                } else {
                    prepared.objects.push_back({decoded->header.type, object.destination_name, std::move(*payload)});
                }
            }
        }
        ++completed;
        if (progress) {
            progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                              "rebuilding FAT12 portable package graph", output_path.string()});
        }
    }
    std::ranges::sort(prepared.objects, [](const auto &left, const auto &right) {
        return std::tuple{left.type, left.name, left.payload.size()} <
               std::tuple{right.type, right.name, right.payload.size()};
    });
    std::ranges::sort(prepared.retained_files, {}, &detail::PreparedMediaFile::path);
    const auto validator = [&](const std::filesystem::path &temporary) {
        return validate_flat_package_result(temporary, packages, plan, cancellation);
    };
    auto written = detail::write_prepared_media_image(prepared, output_path, overwrite, cancellation, validator);
    if (!written)
        return std::unexpected{written.error()};
    auto reader = FileReader::open(output_path);
    if (!reader)
        return std::unexpected{reader.error()};
    auto digest = package_internal::sha256_reader(**reader, cancellation);
    if (!digest)
        return std::unexpected{digest.error()};
    return PackageImportReport{
        target_path, output_path,  plan.plan_id,   plan.target_snapshot_id, package_internal::hex_digest(*digest),
        true,        plan.objects, plan.allocation};
}

Result<PackageImportReport>
apply_iso9660_package_import(const std::filesystem::path &target_path, std::span<const PortablePackage> packages,
                             const PackageImportPlan &plan, const std::filesystem::path &output_path, bool overwrite,
                             const CancellationToken &cancellation, ProgressSink *progress) {
    auto media = open_media(target_path, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (media->kind() != MediaKind::iso9660)
        return std::unexpected{transaction_error("ISO9660 package target changed media kind")};
    auto media_objects = media->objects(64U * 1024U * 1024U, cancellation);
    if (!media_objects)
        return std::unexpected{media_objects.error()};
    const auto &iso = std::get<IsoImage>(media->storage());

    detail::PreparedMediaImage prepared;
    prepared.manifest.schema_version = "1.1";
    prepared.manifest.format = MediaImageFormat::iso9660;
    prepared.manifest.iso_volume_id = iso.volume_id();

    std::map<std::string, std::string, std::less<>> group_labels;
    for (const auto &[raw, label] : iso.group_labels())
        group_labels.emplace(raw, label);
    std::map<std::pair<std::string, std::string>, std::string> volume_labels;
    for (const auto &[raw_path, label] : iso.volume_labels()) {
        const auto separator = raw_path.find('/');
        if (separator != std::string::npos)
            volume_labels.emplace(std::pair{raw_path.substr(0, separator), raw_path.substr(separator + 1U)}, label);
    }
    std::map<std::pair<std::string, std::string>, std::size_t> volume_indices;
    std::map<std::string, std::size_t, std::less<>> existing_group_volume_counts;
    for (const auto &file : iso.files()) {
        if (!file.is_directory)
            continue;
        const auto separator = file.path.find('/');
        if (separator == std::string::npos || file.path.find('/', separator + 1U) != std::string::npos)
            continue;
        const auto scope = std::pair{file.path.substr(0, separator), file.path.substr(separator + 1U)};
        const auto group = group_labels.find(scope.first);
        const auto volume = volume_labels.find(scope);
        if (group == group_labels.end() || volume == volume_labels.end())
            return std::unexpected{transaction_error("cataloged ISO9660 volume labels changed after planning")};
        volume_indices.emplace(scope, prepared.iso_volumes.size());
        ++existing_group_volume_counts[scope.first];
        prepared.iso_volumes.push_back({scope.first, group->second, scope.second, volume->second, {}});
    }
    for (const auto &destination : plan.destinations) {
        const auto scope = std::pair{destination.raw_group, destination.raw_volume};
        if (volume_indices.contains(scope))
            continue;
        if (!destination.create)
            return std::unexpected{transaction_error("planned ISO9660 destination is absent")};
        volume_indices.emplace(scope, prepared.iso_volumes.size());
        prepared.iso_volumes.push_back(
            {destination.raw_group, destination.group_name, destination.raw_volume, destination.volume_name, {}});
    }
    std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>> object_indices;
    std::set<std::string, std::less<>> generated_files;
    std::map<std::string, std::size_t, std::less<>> group_volume_counts;
    for (const auto &volume : prepared.iso_volumes)
        ++group_volume_counts[volume.raw_group];
    for (const auto &[group, count] : group_volume_counts) {
        generated_files.insert(group + "/0000");
        generated_files.insert(group + "/" + std::format("F{:03}", count + 1U));
    }
    for (const auto &[group, count] : existing_group_volume_counts)
        generated_files.insert(group + "/" + std::format("F{:03}", count + 1U));
    for (const auto &object : *media_objects) {
        const auto scope = std::pair{object.raw_group, object.raw_volume};
        const auto volume = volume_indices.find(scope);
        if (volume == volume_indices.end())
            return std::unexpected{transaction_error("existing ISO9660 object has no cataloged raw volume")};
        auto &objects = prepared.iso_volumes[volume->second].objects;
        object_indices.emplace(object.key, std::pair{volume->second, objects.size()});
        objects.push_back({object.decoded.header.type, object.decoded.header.name, object.raw_payload});
        generated_files.insert(object.logical_path);
        const auto separator = object.logical_path.rfind('/');
        if (separator != std::string::npos)
            generated_files.insert(object.logical_path.substr(0, separator) + "/0000");
    }
    for (const auto &file : iso.files()) {
        if (file.is_directory || generated_files.contains(file.path))
            continue;
        auto payload = iso.read_file(file, cancellation);
        if (!payload)
            return std::unexpected{payload.error()};
        prepared.retained_files.push_back({file.path, std::move(*payload)});
    }

    std::set<std::string, std::less<>> updated_physical_objects;
    std::uint64_t completed{};
    for (const auto &object : plan.objects) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        if (!object.canonical_action_id) {
            const auto physical_key =
                object.existing_object_key ? "existing:" + *object.existing_object_key : "planned:" + object.action_id;
            if (updated_physical_objects.insert(physical_key).second &&
                (has_action(object, PackageImportObjectAction::insert) ||
                 has_action(object, PackageImportObjectAction::relocate))) {
                const auto &package = packages[object.package_index];
                const auto *node = package_node(package, object.node_id);
                if (node == nullptr)
                    return std::unexpected{transaction_error("ISO9660 package node is missing")};
                auto context = relocation_context(package, plan, object);
                if (!context)
                    return std::unexpected{context.error()};
                auto payload = package_internal::relocate_package_node(package, *node, *context);
                if (!payload)
                    return std::unexpected{payload.error()};
                auto decoded = decode_object(*payload);
                if (!decoded)
                    return std::unexpected{decoded.error()};
                if (object.existing_object_key) {
                    const auto existing = object_indices.find(*object.existing_object_key);
                    if (existing == object_indices.end())
                        return std::unexpected{transaction_error("planned ISO9660 reused object is absent")};
                    auto &[volume_index, object_index] = existing->second;
                    prepared.iso_volumes[volume_index].objects[object_index] = {
                        decoded->header.type, object.destination_name, std::move(*payload)};
                } else {
                    const auto volume = volume_indices.find({object.raw_group, object.raw_volume});
                    if (volume == volume_indices.end())
                        return std::unexpected{transaction_error("planned ISO9660 insertion volume is absent")};
                    prepared.iso_volumes[volume->second].objects.push_back(
                        {decoded->header.type, object.destination_name, std::move(*payload)});
                }
            }
        }
        ++completed;
        if (progress) {
            progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                              "rebuilding ISO9660 portable package graph", output_path.string()});
        }
    }
    std::ranges::sort(prepared.iso_volumes, [](const auto &left, const auto &right) {
        return std::tie(left.raw_group, left.raw_volume) < std::tie(right.raw_group, right.raw_volume);
    });
    for (auto &volume : prepared.iso_volumes) {
        std::ranges::sort(volume.objects, [](const auto &left, const auto &right) {
            return std::tuple{left.type, left.name, left.payload.size()} <
                   std::tuple{right.type, right.name, right.payload.size()};
        });
    }
    std::ranges::sort(prepared.retained_files, {}, &detail::PreparedMediaFile::path);
    const auto validator = [&](const std::filesystem::path &temporary) -> Result<void> {
        if (plan.allocation.empty())
            return std::unexpected{transaction_error("ISO9660 package plan has no projected allocation")};
        std::error_code size_error;
        const auto actual_size = std::filesystem::file_size(temporary, size_error);
        const auto expected_size = plan.allocation.front().projected_image_size_bytes;
        if (size_error || actual_size != expected_size ||
            std::ranges::any_of(plan.allocation, [&](const auto &allocation) {
                return allocation.projected_image_size_bytes != expected_size;
            })) {
            return std::unexpected{transaction_error(
                size_error ? std::format("cannot inspect rebuilt ISO9660 size: {}", size_error.message())
                           : std::format("rebuilt ISO9660 size differs from the "
                                         "package plan: planned "
                                         "{} bytes, emitted {} bytes",
                                         expected_size, actual_size))};
        }
        return validate_flat_package_result(temporary, packages, plan, cancellation);
    };
    auto written = detail::write_prepared_media_image(prepared, output_path, overwrite, cancellation, validator);
    if (!written)
        return std::unexpected{written.error()};
    auto reader = FileReader::open(output_path);
    if (!reader)
        return std::unexpected{reader.error()};
    auto digest = package_internal::sha256_reader(**reader, cancellation);
    if (!digest)
        return std::unexpected{digest.error()};
    return PackageImportReport{
        target_path, output_path,  plan.plan_id,   plan.target_snapshot_id, package_internal::hex_digest(*digest),
        true,        plan.objects, plan.allocation};
}

Result<void> validate_package_result(const std::filesystem::path &temporary, std::span<const PortablePackage> packages,
                                     const PackageImportPlan &plan, const CancellationToken &cancellation) {
    auto media = open_media(temporary, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (media->kind() != MediaKind::sfs)
        return std::unexpected{transaction_error("package result is not an SFS image")};
    auto catalog = build_object_catalog(*media, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected{catalog.error()};
    const auto graph = build_relationship_graph(*catalog);
    std::map<std::string, const ObjectSnapshot *, std::less<>> actual_by_action;
    for (const auto &object : plan.objects) {
        std::vector<const ObjectSnapshot *> matches;
        for (const auto &snapshot : catalog->objects) {
            if (!object.target_sfs_id || !snapshot.placement || snapshot.partition.value != object.partition_index ||
                snapshot.sfs_id.value != *object.target_sfs_id ||
                snapshot.placement->volume_name != object.volume_name ||
                snapshot.object.header.raw_type != object.object_type ||
                snapshot.object.header.name != object.destination_name) {
                continue;
            }
            matches.push_back(&snapshot);
        }
        if (matches.size() != 1U) {
            return std::unexpected{transaction_error("post-write package object does not match "
                                                     "its planned placement")};
        }
        auto normalized = normalized_payload_digest(matches.front()->raw_payload);
        if (!normalized)
            return std::unexpected{normalized.error()};
        if (*normalized != object.normalized_sha256) {
            return std::unexpected{transaction_error("post-write package object changed normalized identity")};
        }
        if (object.target_link_id) {
            const auto *wave_data = std::get_if<CurrentSmpl>(&matches.front()->object.payload);
            if (wave_data == nullptr || wave_data->link_id.value != *object.target_link_id) {
                return std::unexpected{transaction_error("post-write SMPL link ID differs from the import plan")};
            }
        }
        if (object.object_type == "SBNK") {
            const auto *sample = std::get_if<CurrentSbnk>(&matches.front()->object.payload);
            if (sample == nullptr || sample->linked_program_numbers != object.target_program_numbers ||
                (((sample->sample_flags & 1U) != 0U) != object.target_sample_bank_member)) {
                return std::unexpected{transaction_error("post-write SBNK graph metadata differs "
                                                         "from the import plan")};
            }
        }
        actual_by_action.emplace(object.action_id, matches.front());
    }

    for (const auto &owner : plan.objects) {
        const auto &package = packages[owner.package_index];
        for (const auto &edge : package.relationships) {
            if (edge.source_node_id != owner.node_id)
                continue;
            const auto *target_plan = planned_node(plan, owner, edge.target_node_id);
            if (target_plan == nullptr)
                return std::unexpected{transaction_error("post-write edge has no target action")};
            const auto source = actual_by_action.find(owner.action_id);
            const auto target = actual_by_action.find(target_plan->action_id);
            if (source == actual_by_action.end() || target == actual_by_action.end())
                return std::unexpected{transaction_error("post-write edge endpoint is missing")};
            const auto relationships = graph.children(source->second->key);
            const auto matched = std::ranges::find_if(relationships, [&](const Relationship *actual) {
                return actual->type == edge.role && actual->quality == RelationshipQuality::known &&
                       actual->target_key && *actual->target_key == target->second->key;
            });
            if (matched == relationships.end()) {
                return std::unexpected{
                    transaction_error(std::format("post-write {} relationship from {} to {} "
                                                  "differs from the package plan",
                                                  edge.role, owner.destination_name, target_plan->destination_name))};
            }
        }
    }
    return {};
}

Result<std::string> file_snapshot_id(const std::filesystem::path &path, const CancellationToken &cancellation) {
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    auto digest = package_internal::sha256_reader(**reader, cancellation);
    if (!digest)
        return std::unexpected{digest.error()};
    return package_internal::hex_digest(*digest);
}

Result<TransactionState> prepare_alteration(const std::filesystem::path &source_path,
                                            const AlterationManifest &manifest, const CancellationToken &cancellation,
                                            ProgressSink *progress, std::string_view initial_message,
                                            std::optional<std::string> progress_path = std::nullopt) {
    auto opened = open_transaction_state(source_path, cancellation, progress, requires_object_graph(manifest));
    if (!opened)
        return std::unexpected{opened.error()};
    auto state = std::move(*opened);
    if (progress) {
        progress->report(
            {ProgressPhase::allocating, 0U, manifest.operations.size(), std::string{initial_message}, progress_path});
    }
    using OperationHandler =
        Result<OperationReport> (*)(TransactionState &, const OperationView &, const CancellationToken &);
    constexpr std::array<OperationHandler, std::variant_size_v<AlterationOperationData>> handlers{
        delete_volume,   insert_volume,   delete_sbnk,    insert_sbnk,   insert_waveform,
        delete_waveform, rename_waveform, rename_sbnk,    delete_sbac,   insert_sbac,
        rename_sbac,     delete_program,  insert_program, rename_volume, rename_partition,
    };
    for (std::size_t operation_index = 0; operation_index < manifest.operations.size(); ++operation_index) {
        const auto &typed_operation = manifest.operations[operation_index];
        const auto operation = operation_view(typed_operation);
        auto report = handlers[typed_operation.data.index()](state, operation, cancellation);
        if (!report)
            return std::unexpected{report.error()};
        state.reports.push_back(std::move(*report));
        if (progress) {
            progress->report({ProgressPhase::allocating, operation_index + 1U, manifest.operations.size(),
                              operation.type, progress_path});
        }
    }
    return state;
}

} // namespace

std::string_view operation_type_name(const AlterationOperationData &operation) noexcept {
    constexpr std::array names{
        std::string_view{"delete_volume"},   std::string_view{"insert_volume"},   std::string_view{"delete_sbnk"},
        std::string_view{"insert_sbnk"},     std::string_view{"insert_waveform"}, std::string_view{"delete_waveform"},
        std::string_view{"rename_waveform"}, std::string_view{"rename_sbnk"},     std::string_view{"delete_sbac"},
        std::string_view{"insert_sbac"},     std::string_view{"rename_sbac"},     std::string_view{"delete_program"},
        std::string_view{"insert_program"},  std::string_view{"rename_volume"},   std::string_view{"rename_partition"},
    };
    return names[operation.index()];
}

Result<std::string> serialize_alteration_manifest_template() {
    try {
        OrderedJson operation = OrderedJson::object();
        operation["id"] = "rename-waveform";
        operation["type"] = "rename_waveform";
        operation["partition_index"] = 0;
        operation["volume_name"] = "Volume";
        operation["waveform_name"] = "Old Wave";
        operation["new_waveform_name"] = "New Wave";

        OrderedJson manifest = OrderedJson::object();
        manifest["schema_version"] = "1.1";
        manifest["operations"] = OrderedJson::array({std::move(operation)});
        return manifest.dump(2) + "\n";
    } catch (const OrderedJson::exception &error) {
        return std::unexpected{
            transaction_error(std::string{"could not serialize alteration manifest template: "} + error.what())};
    }
}

Result<void> write_alteration_manifest_template(const std::filesystem::path &output_path, bool overwrite) {
    auto serialized = serialize_alteration_manifest_template();
    if (!serialized)
        return std::unexpected{serialized.error()};

    std::error_code filesystem_error;
    if (!overwrite && std::filesystem::exists(output_path, filesystem_error)) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                       "refusing to replace existing alteration manifest: " + text::path_to_utf8(output_path))};
    }
    if (filesystem_error) {
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "could not inspect alteration manifest output path")};
    }
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "could not create alteration manifest output directory")};
    }
    auto temporary = detail::write_temporary_file(output_path, [&](const detail::TemporaryFileSink &sink) {
        return sink(std::as_bytes(std::span{serialized->data(), serialized->size()}));
    });
    if (!temporary)
        return std::unexpected{temporary.error()};
    if (const auto published = detail::publish_temporary_file(*temporary, output_path, overwrite); !published) {
        std::filesystem::remove(*temporary, filesystem_error);
        return std::unexpected{published.error()};
    }
    return {};
}

Result<AlterationManifest> parse_alteration_manifest(std::string_view json,
                                                     const std::filesystem::path &base_directory) {
    try {
        auto root = Json::parse(json);
        const bool legacy = migrate_legacy_alteration_manifest(root);
        if (auto valid = exact_fields(root, {"schema_version", "operations"}, "manifest"); !valid)
            return std::unexpected{valid.error()};
        auto version = required_text(root, "schema_version", "manifest");
        if (!version)
            return std::unexpected{version.error()};
        if (*version != "1.1")
            return std::unexpected{transaction_error("manifest schema version must be 1.0 or 1.1")};
        if (!root["operations"].is_array() || root["operations"].empty())
            return std::unexpected{transaction_error("manifest.operations must be a non-empty array")};
        AlterationManifest result{legacy ? "1.0" : *version, {}};
        std::set<std::string> seen;
        for (std::size_t index = 0; index < root["operations"].size(); ++index) {
            const auto &row = root["operations"][index];
            const auto context = "manifest.operations[" + std::to_string(index) + "]";
            if (!row.is_object())
                return std::unexpected{transaction_error(context + " must be an object")};
            auto id = required_text(row, "id", context);
            auto type = required_text(row, "type", context);
            if (!id)
                return std::unexpected{id.error()};
            if (!type)
                return std::unexpected{type.error()};
            if (!seen.insert(*id).second)
                return std::unexpected{transaction_error("duplicate operation id")};
            if (*type != "delete_volume" && *type != "insert_volume" && *type != "delete_sbnk" &&
                *type != "insert_sbnk" && *type != "insert_waveform" && *type != "delete_waveform" &&
                *type != "delete_program" && *type != "insert_program" && *type != "delete_sbac" &&
                *type != "insert_sbac" && *type != "rename_waveform" && *type != "rename_sbnk" &&
                *type != "rename_sbac" && *type != "rename_volume" && *type != "rename_partition") {
                return std::unexpected{transaction_error("operation type is not implemented by "
                                                         "the native transaction engine")};
            }
            PartitionSelector selector;
            if (row["partition_index"].is_number_integer()) {
                const auto value = row["partition_index"].get<int>();
                if (value < 0 || value > 7)
                    return std::unexpected{transaction_error("partition index must be 0..7")};
                selector = PartitionIndex{static_cast<std::uint8_t>(value)};
            } else if (row["partition_index"].is_object() && row["partition_index"].size() == 1U &&
                       row["partition_index"].contains("operation_ref")) {
                auto reference = required_text(row["partition_index"], "operation_ref", context + ".partition_index");
                if (!reference)
                    return std::unexpected{reference.error()};
                if (!seen.contains(*reference))
                    return std::unexpected{transaction_error("operation_ref must name an earlier operation")};
                selector = OperationReference{*reference};
            } else
                return std::unexpected{transaction_error("partition selector is invalid")};
            OperationView operation;
            operation.id = *id;
            operation.type = *type;
            operation.partition = std::move(selector);
            if (*type == "delete_volume") {
                if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume_name"}, context); !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                operation.volume_name = *volume;
            } else if (*type == "rename_volume") {
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "new_volume_name"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto volume = object_name(row, "volume_name", context);
                auto new_volume = object_name(row, "new_volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!new_volume)
                    return std::unexpected{new_volume.error()};
                if (*volume == *new_volume)
                    return std::unexpected{transaction_error("new_volume_name must differ")};
                operation.volume_name = std::move(*volume);
                operation.new_volume_name = std::move(*new_volume);
            } else if (*type == "rename_partition") {
                if (auto valid = exact_fields(
                        row, {"id", "type", "partition_index", "partition_name", "new_partition_name"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto old_name = partition_name(row, "partition_name", context);
                auto new_name = partition_name(row, "new_partition_name", context);
                if (!old_name)
                    return std::unexpected{old_name.error()};
                if (!new_name)
                    return std::unexpected{new_name.error()};
                if (*old_name == *new_name)
                    return std::unexpected{transaction_error("new_partition_name must differ")};
                operation.partition_name = std::move(*old_name);
                operation.new_partition_name = std::move(*new_name);
            } else if (*type == "insert_volume") {
                if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume"}, context); !valid)
                    return std::unexpected{valid.error()};
                Json wrapper{
                    {"schema_version", "1.1"},
                    {"size_bytes", minimum_hds_size},
                    {"partitions", Json::array({
                                       {{"name", "AXK ALTER"}, {"volumes", Json::array({row["volume"]})}},
                                   })},
                };
                auto parsed = parse_hds_build_manifest(wrapper.dump(), base_directory);
                if (!parsed)
                    return std::unexpected{parsed.error()};
                operation.volume = parsed->partitions[0].volumes[0];
                operation.volume_name = operation.volume->name;
            } else if (*type == "delete_sbnk") {
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "sample_name"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto volume = required_text(row, "volume_name", context);
                auto sample = object_name(row, "sample_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!sample)
                    return std::unexpected{sample.error()};
                operation.volume_name = std::move(*volume);
                operation.sample_name = std::move(*sample);
            } else if (*type == "insert_sbnk") {
                if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume_name", "sample"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto volume = required_text(row, "volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                const auto &sample = row["sample"];
                if (!sample.is_object()) {
                    return std::unexpected{transaction_error(context + ".sample must be an object")};
                }
                const std::set<std::string> required{"name", "waveform_name", "root_key", "key_low", "key_high"};
                const std::set<std::string> optional{"right_waveform_name", "level"};
                for (const auto &field : required) {
                    if (!sample.contains(field)) {
                        return std::unexpected{transaction_error(context + ".sample is missing field " + field)};
                    }
                }
                for (const auto &[field, unused] : sample.items()) {
                    static_cast<void>(unused);
                    if (!required.contains(field) && !optional.contains(field)) {
                        return std::unexpected{transaction_error(context + ".sample has unknown field " + field)};
                    }
                }
                const auto sample_context = context + ".sample";
                auto name = object_name(sample, "name", sample_context);
                auto waveform = object_name(sample, "waveform_name", sample_context);
                auto root_key = midi_value(sample, "root_key", sample_context, 0U, true);
                auto key_low = midi_value(sample, "key_low", sample_context, 0U, true);
                auto key_high = midi_value(sample, "key_high", sample_context, 0U, true);
                auto level = midi_value(sample, "level", sample_context, 100U, false);
                if (!name)
                    return std::unexpected{name.error()};
                if (!waveform)
                    return std::unexpected{waveform.error()};
                if (!root_key)
                    return std::unexpected{root_key.error()};
                if (!key_low)
                    return std::unexpected{key_low.error()};
                if (!key_high)
                    return std::unexpected{key_high.error()};
                if (!level)
                    return std::unexpected{level.error()};
                if (*key_high < *key_low) {
                    return std::unexpected{transaction_error(sample_context + ".key_high must not be below key_low")};
                }
                SampleSpec spec;
                spec.name = std::move(*name);
                spec.waveform_id = std::move(*waveform);
                if (sample.contains("right_waveform_name")) {
                    auto right = object_name(sample, "right_waveform_name", sample_context);
                    if (!right)
                        return std::unexpected{right.error()};
                    spec.right_waveform_id = std::move(*right);
                }
                spec.root_key = *root_key;
                spec.key_low = *key_low;
                spec.key_high = *key_high;
                spec.level = *level;
                operation.volume_name = std::move(*volume);
                operation.sample_name = spec.name;
                operation.sample = std::move(spec);
            } else if (*type == "rename_waveform") {
                if (auto valid = exact_fields(
                        row, {"id", "type", "partition_index", "volume_name", "waveform_name", "new_waveform_name"},
                        context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                auto old_name = object_name(row, "waveform_name", context);
                auto new_name = object_name(row, "new_waveform_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!old_name)
                    return std::unexpected{old_name.error()};
                if (!new_name)
                    return std::unexpected{new_name.error()};
                if (*old_name == *new_name)
                    return std::unexpected{transaction_error("new_waveform_name must differ")};
                operation.volume_name = std::move(*volume);
                operation.waveform_name = std::move(*old_name);
                operation.new_waveform_name = std::move(*new_name);
            } else if (*type == "rename_sbnk") {
                if (auto valid = exact_fields(
                        row, {"id", "type", "partition_index", "volume_name", "sample_name", "new_sample_name"},
                        context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                auto old_name = object_name(row, "sample_name", context);
                auto new_name = object_name(row, "new_sample_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!old_name)
                    return std::unexpected{old_name.error()};
                if (!new_name)
                    return std::unexpected{new_name.error()};
                if (*old_name == *new_name)
                    return std::unexpected{transaction_error("new_sample_name must differ")};
                operation.volume_name = std::move(*volume);
                operation.sample_name = std::move(*old_name);
                operation.new_sample_name = std::move(*new_name);
            } else if (*type == "rename_sbac") {
                if (auto valid = exact_fields(
                        row,
                        {"id", "type", "partition_index", "volume_name", "sample_bank_name", "new_sample_bank_name"},
                        context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                auto old_name = object_name(row, "sample_bank_name", context);
                auto new_name = object_name(row, "new_sample_bank_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!old_name)
                    return std::unexpected{old_name.error()};
                if (!new_name)
                    return std::unexpected{new_name.error()};
                if (*old_name == *new_name)
                    return std::unexpected{transaction_error("new_sample_bank_name must differ")};
                operation.volume_name = std::move(*volume);
                operation.sample_bank_name = std::move(*old_name);
                operation.new_sample_bank_name = std::move(*new_name);
            } else if (*type == "delete_program") {
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "program_number"}, context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                auto number = program_value(row, "program_number", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!number)
                    return std::unexpected{number.error()};
                operation.volume_name = std::move(*volume);
                operation.program_number = *number;
            } else if (*type == "delete_sbac") {
                if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume_name", "sample_bank_name"},
                                              context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                auto sample_bank = object_name(row, "sample_bank_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!sample_bank)
                    return std::unexpected{sample_bank.error()};
                operation.volume_name = std::move(*volume);
                operation.sample_bank_name = std::move(*sample_bank);
            } else if (*type == "insert_sbac") {
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "sample_bank"}, context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                const auto &sample_bank = row["sample_bank"];
                if (auto valid = exact_fields(sample_bank, {"name", "member_samples"}, context + ".sample_bank");
                    !valid)
                    return std::unexpected{valid.error()};
                auto name = object_name(sample_bank, "name", context + ".sample_bank");
                if (!name)
                    return std::unexpected{name.error()};
                if (!sample_bank["member_samples"].is_array() || sample_bank["member_samples"].empty() ||
                    sample_bank["member_samples"].size() > 3U)
                    return std::unexpected{transaction_error("member_samples must contain 1..3 names")};
                SampleBankSpec spec;
                spec.name = *name;
                for (std::size_t member_index = 0; member_index < sample_bank["member_samples"].size();
                     ++member_index) {
                    Json wrapper{{"name", sample_bank["member_samples"][member_index]}};
                    auto member = object_name(wrapper, "name", context + ".sample_bank.member_samples");
                    if (!member)
                        return std::unexpected{member.error()};
                    if (std::ranges::contains(spec.member_samples, *member))
                        return std::unexpected{transaction_error("member_samples must be distinct")};
                    spec.member_samples.push_back(std::move(*member));
                }
                operation.volume_name = std::move(*volume);
                operation.sample_bank_name = spec.name;
                operation.sample_bank = std::move(spec);
            } else if (*type == "insert_program") {
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "program"}, context);
                    !valid)
                    return std::unexpected{valid.error()};
                auto volume = required_text(row, "volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                const auto &program = row["program"];
                if (auto valid = exact_fields(program, {"number", "assignments"}, context + ".program"); !valid)
                    return std::unexpected{valid.error()};
                auto number = program_value(program, "number", context + ".program");
                if (!number)
                    return std::unexpected{number.error()};
                if (!program["assignments"].is_array() || program["assignments"].size() != 2U)
                    return std::unexpected{transaction_error("Program requires exactly two assignments")};
                ProgramSpec spec;
                spec.number = *number;
                constexpr std::array<std::string_view, 2> target_fields{"sample_bank", "sample"};
                for (std::size_t assignment_index = 0; assignment_index < 2U; ++assignment_index) {
                    const auto &assignment = program["assignments"][assignment_index];
                    const auto field = target_fields[assignment_index];
                    if (auto valid =
                            exact_fields(assignment, {field, "receive_channel"}, context + ".program.assignments");
                        !valid)
                        return std::unexpected{valid.error()};
                    auto target = object_name(assignment, field, context + ".program.assignments");
                    if (!target)
                        return std::unexpected{target.error()};
                    auto channel =
                        midi_value(assignment, "receive_channel", context + ".program.assignments", 0U, true);
                    const auto expected_channel = static_cast<std::uint8_t>(assignment_index + 1U);
                    if (!channel || *channel != expected_channel)
                        return std::unexpected{transaction_error("Program assignments must be SBAC/channel 1 then "
                                                                 "SBNK/channel 2")};
                    spec.assignments.push_back(
                        {assignment_index == 0U ? "SBAC" : "SBNK", std::move(*target), *channel});
                }
                operation.volume_name = std::move(*volume);
                operation.program_number = spec.number;
                operation.program = std::move(spec);
            } else if (*type == "delete_waveform") {
                if (auto valid =
                        exact_fields(row, {"id", "type", "partition_index", "volume_name", "waveform_name"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto volume = required_text(row, "volume_name", context);
                auto waveform = object_name(row, "waveform_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                if (!waveform)
                    return std::unexpected{waveform.error()};
                operation.volume_name = std::move(*volume);
                operation.waveform_name = std::move(*waveform);
            } else {
                if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume_name", "audio"}, context);
                    !valid) {
                    return std::unexpected{valid.error()};
                }
                auto volume = required_text(row, "volume_name", context);
                if (!volume)
                    return std::unexpected{volume.error()};
                const auto &audio = row["audio"];
                if (!audio.is_object()) {
                    return std::unexpected{transaction_error(context + ".audio must be an object")};
                }
                const std::set<std::string> required{"path", "waveform_names", "root_key"};
                const std::set<std::string> optional{"target_sample_rate"};
                for (const auto &field : required) {
                    if (!audio.contains(field)) {
                        return std::unexpected{transaction_error(context + ".audio is missing field " + field)};
                    }
                }
                for (const auto &[field, unused] : audio.items()) {
                    static_cast<void>(unused);
                    if (!required.contains(field) && !optional.contains(field)) {
                        return std::unexpected{transaction_error(context + ".audio has unknown field " + field)};
                    }
                }
                if (!audio["waveform_names"].is_array() ||
                    (audio["waveform_names"].size() != 1U && audio["waveform_names"].size() != 2U)) {
                    return std::unexpected{
                        transaction_error(context + ".audio.waveform_names must contain one or two names")};
                }
                InsertWaveformSpec spec;
                for (std::size_t name_index = 0; name_index < audio["waveform_names"].size(); ++name_index) {
                    Json wrapper{{"name", audio["waveform_names"][name_index]}};
                    auto name = object_name(wrapper, "name",
                                            context + ".audio.waveform_names[" + std::to_string(name_index) + "]");
                    if (!name)
                        return std::unexpected{name.error()};
                    if (std::ranges::contains(spec.waveform_names, *name)) {
                        return std::unexpected{transaction_error(context + ".audio.waveform_names must be distinct")};
                    }
                    spec.waveform_names.push_back(std::move(*name));
                }
                auto path = required_text(audio, "path", context + ".audio");
                auto root_key = midi_value(audio, "root_key", context + ".audio", 0U, true);
                if (!path)
                    return std::unexpected{path.error()};
                if (!root_key)
                    return std::unexpected{root_key.error()};
                auto audio_path = axk::text::path_from_utf8(*path);
                if (!audio_path)
                    return std::unexpected{transaction_error(context + ".audio.path must be valid UTF-8")};
                spec.path = std::move(*audio_path);
                if (spec.path.is_relative())
                    spec.path = base_directory / spec.path;
                spec.root_key = *root_key;
                if (audio.contains("target_sample_rate")) {
                    if (!audio["target_sample_rate"].is_number_integer()) {
                        return std::unexpected{
                            transaction_error(context + ".audio.target_sample_rate must be an integer")};
                    }
                    const auto rate = audio["target_sample_rate"].get<std::int64_t>();
                    if (rate <= 0 || rate > std::numeric_limits<std::uint32_t>::max()) {
                        return std::unexpected{
                            transaction_error(context + ".audio.target_sample_rate is out of range")};
                    }
                    spec.target_sample_rate = static_cast<std::uint32_t>(rate);
                }
                operation.volume_name = std::move(*volume);
                operation.waveform = std::move(spec);
            }
            result.operations.push_back(typed_operation(std::move(operation)));
        }
        return result;
    } catch (const Json::exception &error) {
        return std::unexpected{transaction_error(std::string{"invalid alteration JSON: "} + error.what())};
    }
}

Result<AlterationManifest> load_alteration_manifest(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not open alteration manifest")};
    std::ostringstream text;
    text << input.rdbuf();
    return parse_alteration_manifest(text.str(), path.parent_path());
}

Result<AlterationResult> alter_hds(const std::filesystem::path &source_path, const AlterationManifest &manifest,
                                   const std::filesystem::path &output_path, const CancellationToken &cancellation,
                                   ProgressSink *progress, bool overwrite) {
    if (std::filesystem::absolute(source_path).lexically_normal() ==
        std::filesystem::absolute(output_path).lexically_normal()) {
        return std::unexpected{transaction_error("alteration output must differ from source")};
    }
    auto prepared = prepare_alteration(source_path, manifest, cancellation, progress, "planning alteration",
                                       text::path_to_utf8(output_path));
    if (!prepared)
        return std::unexpected{prepared.error()};
    auto state = std::move(*prepared);
    auto applied = publish(state, output_path, cancellation, overwrite, {}, progress);
    if (!applied)
        return std::unexpected{applied.error()};
    return AlterationResult{source_path, output_path, true, std::move(state.reports)};
}

Result<AlterationInspection> inspect_hds_alteration(const std::filesystem::path &source_path,
                                                    const AlterationManifest &manifest,
                                                    const CancellationToken &cancellation, ProgressSink *progress) {
    auto prepared = prepare_alteration(source_path, manifest, cancellation, progress, "inspecting alteration");
    if (!prepared)
        return std::unexpected{prepared.error()};
    return AlterationInspection{source_path, std::move(prepared->reports)};
}

Result<PackageImportReport> apply_package_import(const std::filesystem::path &target_path,
                                                 std::span<const PortablePackage> packages,
                                                 const PackageImportPlan &plan,
                                                 const std::filesystem::path &output_path, bool overwrite,
                                                 const CancellationToken &cancellation, ProgressSink *progress) {
    try {
        if (std::filesystem::absolute(target_path).lexically_normal() ==
            std::filesystem::absolute(output_path).lexically_normal()) {
            return std::unexpected{transaction_error("package import output must differ from its source image")};
        }
        if (!overwrite && std::filesystem::exists(output_path)) {
            return std::unexpected{
                make_error(ErrorCode::io_open_failed, ErrorCategory::io, "package import output already exists")};
        }
        if (const auto verified = verify_package_import_plan(plan); !verified)
            return std::unexpected{verified.error()};
        if (!plan.valid())
            return std::unexpected{transaction_error("a conflicting package import plan cannot apply")};
        if (plan.target_kind != MediaKind::sfs && plan.target_kind != MediaKind::fat12_floppy &&
            plan.target_kind != MediaKind::iso9660)
            return std::unexpected{transaction_error("package import target adapter is unsupported")};
        if (packages.size() != plan.package_ids.size())
            return std::unexpected{transaction_error("package import inputs do not match the "
                                                     "planned package count")};
        for (std::size_t index = 0; index < packages.size(); ++index) {
            if (const auto verified = verify_portable_package(packages[index]); !verified)
                return std::unexpected{verified.error()};
            if (packages[index].package_id != plan.package_ids[index]) {
                return std::unexpected{transaction_error("package import input identity differs from the plan")};
            }
        }
        auto snapshot = file_snapshot_id(target_path, cancellation);
        if (!snapshot)
            return std::unexpected{snapshot.error()};
        if (*snapshot != plan.target_snapshot_id)
            return std::unexpected{transaction_error("package import plan is stale for this target")};

        if (plan.target_kind == MediaKind::fat12_floppy) {
            return apply_fat12_package_import(target_path, packages, plan, output_path, overwrite, cancellation,
                                              progress);
        }
        if (plan.target_kind == MediaKind::iso9660) {
            return apply_iso9660_package_import(target_path, packages, plan, output_path, overwrite, cancellation,
                                                progress);
        }

        auto opened = open_transaction_state(target_path, cancellation, progress, true);
        if (!opened)
            return std::unexpected{opened.error()};
        auto state = std::move(*opened);
        auto confirmed_snapshot = file_snapshot_id(target_path, cancellation);
        if (!confirmed_snapshot)
            return std::unexpected{confirmed_snapshot.error()};
        if (*confirmed_snapshot != plan.target_snapshot_id)
            return std::unexpected{transaction_error("package import target changed before "
                                                     "transaction preparation")};

        std::size_t completed{};
        for (const auto &destination : plan.destinations) {
            if (!destination.create)
                continue;
            if (const auto checked = cancellation.check(); !checked)
                return std::unexpected{checked.error()};
            OperationView operation;
            operation.id =
                std::format("package-destination-{}-{}", destination.partition_index, destination.volume_name);
            operation.type = "insert_volume";
            operation.partition = PartitionIndex{destination.partition_index};
            operation.volume_name = destination.volume_name;
            operation.volume = VolumeSpec{destination.volume_name, {}, {}, {}, {}};
            auto inserted = insert_volume(state, operation, cancellation);
            if (!inserted)
                return std::unexpected{inserted.error()};
            std::vector<std::uint32_t> actual_ids;
            for (const auto id : inserted->inserted_sfs_ids)
                actual_ids.push_back(id.value);
            if (actual_ids != destination.infrastructure_sfs_ids ||
                inserted->allocated_clusters != destination.infrastructure_clusters) {
                return std::unexpected{transaction_error("actual destination volume allocation "
                                                         "differs from the import plan")};
            }
            state.reports.push_back(std::move(*inserted));
            if (progress) {
                progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                                  "creating package destination volume", output_path.string()});
            }
        }
        std::set<std::pair<std::uint8_t, std::uint32_t>> updated_reused_objects;
        for (const auto &object : plan.objects) {
            if (const auto checked = cancellation.check(); !checked)
                return std::unexpected{checked.error()};
            if (!has_action(object, PackageImportObjectAction::insert)) {
                if (has_action(object, PackageImportObjectAction::reuse) &&
                    has_action(object, PackageImportObjectAction::relocate)) {
                    if (!object.target_sfs_id || object.object_type != "SBNK") {
                        return std::unexpected{transaction_error("planned reused relocation is "
                                                                 "not a fixed SBNK object")};
                    }
                    const auto physical_key = std::pair{object.partition_index, *object.target_sfs_id};
                    if (updated_reused_objects.emplace(physical_key).second) {
                        const auto &package = packages[object.package_index];
                        const auto *node = package_node(package, object.node_id);
                        if (node == nullptr)
                            return std::unexpected{transaction_error("package import action node is missing")};
                        auto context = relocation_context(package, plan, object);
                        if (!context)
                            return std::unexpected{context.error()};
                        auto payload = package_internal::relocate_package_node(package, *node, *context);
                        if (!payload)
                            return std::unexpected{payload.error()};
                        auto normalized = normalized_payload_digest(*payload);
                        if (!normalized)
                            return std::unexpected{normalized.error()};
                        if (*normalized != object.normalized_sha256) {
                            return std::unexpected{transaction_error("relocated reused node differs from its "
                                                                     "planned identity")};
                        }
                        const auto partition = state.partitions.find(object.partition_index);
                        if (partition == state.partitions.end()) {
                            return std::unexpected{transaction_error("package import partition is invalid")};
                        }
                        if (auto replaced =
                                replace_fixed_object_payload(state, partition->second, SfsId{*object.target_sfs_id},
                                                             std::move(*payload), cancellation);
                            !replaced) {
                            return std::unexpected{replaced.error()};
                        }
                    }
                }
                ++completed;
                if (progress) {
                    progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                                      "reusing portable package object", output_path.string()});
                }
                continue;
            }
            const auto &package = packages[object.package_index];
            const auto *node = package_node(package, object.node_id);
            if (node == nullptr)
                return std::unexpected{transaction_error("package import action node is missing")};
            auto context = relocation_context(package, plan, object);
            if (!context)
                return std::unexpected{context.error()};
            auto payload = package_internal::relocate_package_node(package, *node, *context);
            if (!payload)
                return std::unexpected{payload.error()};
            auto normalized = normalized_payload_digest(*payload);
            if (!normalized)
                return std::unexpected{normalized.error()};
            if (*normalized != object.normalized_sha256) {
                return std::unexpected{transaction_error("relocated package node differs from its "
                                                         "planned identity")};
            }
            const auto partition = state.partitions.find(object.partition_index);
            if (partition == state.partitions.end() || !object.target_sfs_id)
                return std::unexpected{transaction_error("package import partition or SFS ID is invalid")};
            auto &mutable_partition = partition->second;
            auto allocated = allocate_record(mutable_partition, std::move(*payload), PayloadKind::object,
                                             SfsId{*object.target_sfs_id});
            if (!allocated)
                return std::unexpected{allocated.error()};
            const auto inserted = mutable_partition.inserted.find(SfsId{*object.target_sfs_id});
            if (inserted == mutable_partition.inserted.end())
                return std::unexpected{transaction_error("package insertion did not reserve its record")};
            std::uint64_t payload_clusters{};
            for (const auto &extent : inserted->second.extents)
                payload_clusters += extent.cluster_count;
            if (payload_clusters != object.payload_clusters ||
                inserted->second.continuation_clusters.size() != object.continuation_clusters ||
                allocated->second != object.payload_clusters + object.continuation_clusters) {
                return std::unexpected{transaction_error("actual package allocation differs from the import plan")};
            }
            auto directory =
                volume_category(state, mutable_partition, object.volume_name, object.object_type, cancellation);
            if (!directory)
                return std::unexpected{directory.error()};
            if (auto appended =
                    append_directory_entry(state, mutable_partition, *directory, SfsId{*object.target_sfs_id},
                                           object.destination_name, cancellation);
                !appended) {
                return std::unexpected{appended.error()};
            }
            ++completed;
            if (progress) {
                progress->report({ProgressPhase::writing, completed, plan.objects.size(),
                                  "importing portable package object", output_path.string()});
            }
        }

        std::set<std::tuple<std::uint8_t, SfsId, SfsId>> added_edges;
        for (const auto &owner : plan.objects) {
            const auto &package = packages[owner.package_index];
            for (const auto &edge : package.relationships) {
                if (edge.source_node_id != owner.node_id)
                    continue;
                const auto *target = planned_node(plan, owner, edge.target_node_id);
                if (target == nullptr || !owner.target_sfs_id || !target->target_sfs_id)
                    return std::unexpected{transaction_error("package relationship lacks a planned SFS endpoint")};
                const auto tuple =
                    std::tuple{owner.partition_index, SfsId{*owner.target_sfs_id}, SfsId{*target->target_sfs_id}};
                if (added_edges.emplace(tuple).second &&
                    !std::ranges::contains(state.known_edges,
                                           std::tuple{PartitionIndex{owner.partition_index},
                                                      SfsId{*owner.target_sfs_id}, SfsId{*target->target_sfs_id}})) {
                    state.known_edges.emplace_back(PartitionIndex{owner.partition_index}, SfsId{*owner.target_sfs_id},
                                                   SfsId{*target->target_sfs_id});
                }
            }
        }

        const auto validator = [&](const std::filesystem::path &temporary) {
            return validate_package_result(temporary, packages, plan, cancellation);
        };
        if (auto published = publish(state, output_path, cancellation, overwrite, validator, progress); !published) {
            return std::unexpected{published.error()};
        }
        auto output_snapshot = file_snapshot_id(output_path, cancellation);
        if (!output_snapshot)
            return std::unexpected{output_snapshot.error()};
        return PackageImportReport{
            target_path, output_path,  plan.plan_id,   plan.target_snapshot_id, std::move(*output_snapshot),
            true,        plan.objects, plan.allocation};
    } catch (const std::exception &error) {
        return std::unexpected{transaction_error(std::string{"package import callback failed: "} + error.what())};
    } catch (...) {
        return std::unexpected{transaction_error("package import callback failed")};
    }
}

} // namespace axk
