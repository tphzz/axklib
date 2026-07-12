#include "axklib/alteration.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <format>
#include <fstream>
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
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"
#include "writer_internal.hpp"

namespace axk {
namespace {

using Json = nlohmann::json;

Error transaction_error(std::string message) {
  return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                    std::move(message));
}

struct MutablePartition {
  const Partition *source{};
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

struct OperationView {
  std::string id;
  std::string type;
  PartitionSelector partition;
  std::string volume_name;
  std::optional<VolumeSpec> volume;
  std::string sample_bank_name;
  std::optional<SampleBankSpec> sample_bank;
  std::optional<InsertWaveformSpec> waveform;
  std::string waveform_name;
  std::string new_waveform_name;
  std::string new_sample_bank_name;
  std::string sample_bank_group_name;
  std::string new_sample_bank_group_name;
  std::optional<SampleBankGroupSpec> sample_bank_group;
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
        } else if constexpr (std::same_as<T, DeleteSampleBankOperation>) {
          result.volume_name = value.volume_name;
          result.sample_bank_name = value.sample_bank_name;
        } else if constexpr (std::same_as<T, InsertSampleBankOperation>) {
          result.volume_name = value.volume_name;
          result.sample_bank_name = value.sample_bank.name;
          result.sample_bank = value.sample_bank;
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
        } else if constexpr (std::same_as<T, RenameSampleBankOperation>) {
          result.volume_name = value.volume_name;
          result.sample_bank_name = value.sample_bank_name;
          result.new_sample_bank_name = value.new_sample_bank_name;
        } else if constexpr (std::same_as<T, DeleteSampleBankGroupOperation>) {
          result.volume_name = value.volume_name;
          result.sample_bank_group_name = value.sample_bank_group_name;
        } else if constexpr (std::same_as<T, InsertSampleBankGroupOperation>) {
          result.volume_name = value.volume_name;
          result.sample_bank_group_name = value.sample_bank_group.name;
          result.sample_bank_group = value.sample_bank_group;
        } else if constexpr (std::same_as<T, RenameSampleBankGroupOperation>) {
          result.volume_name = value.volume_name;
          result.sample_bank_group_name = value.sample_bank_group_name;
          result.new_sample_bank_group_name = value.new_sample_bank_group_name;
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
  } else if (value.type == "delete_sbnk") {
    data = DeleteSampleBankOperation{std::move(value.partition), std::move(value.volume_name),
                                     std::move(value.sample_bank_name)};
  } else if (value.type == "insert_sbnk") {
    data = InsertSampleBankOperation{std::move(value.partition), std::move(value.volume_name),
                                     std::move(*value.sample_bank)};
  } else if (value.type == "insert_waveform") {
    data = InsertWaveformOperation{std::move(value.partition), std::move(value.volume_name),
                                   std::move(*value.waveform)};
  } else if (value.type == "delete_waveform") {
    data = DeleteWaveformOperation{std::move(value.partition), std::move(value.volume_name),
                                   std::move(value.waveform_name)};
  } else if (value.type == "rename_waveform") {
    data =
        RenameWaveformOperation{std::move(value.partition), std::move(value.volume_name),
                                std::move(value.waveform_name), std::move(value.new_waveform_name)};
  } else if (value.type == "rename_sbnk") {
    data = RenameSampleBankOperation{std::move(value.partition), std::move(value.volume_name),
                                     std::move(value.sample_bank_name),
                                     std::move(value.new_sample_bank_name)};
  } else if (value.type == "delete_sbac") {
    data = DeleteSampleBankGroupOperation{std::move(value.partition), std::move(value.volume_name),
                                          std::move(value.sample_bank_group_name)};
  } else if (value.type == "insert_sbac") {
    data = InsertSampleBankGroupOperation{std::move(value.partition), std::move(value.volume_name),
                                          std::move(*value.sample_bank_group)};
  } else if (value.type == "rename_sbac") {
    data = RenameSampleBankGroupOperation{std::move(value.partition), std::move(value.volume_name),
                                          std::move(value.sample_bank_group_name),
                                          std::move(value.new_sample_bank_group_name)};
  } else if (value.type == "delete_program") {
    data = DeleteProgramOperation{std::move(value.partition), std::move(value.volume_name),
                                  *value.program_number};
  } else {
    data = InsertProgramOperation{std::move(value.partition), std::move(value.volume_name),
                                  std::move(*value.program)};
  }
  return {std::move(value.id), std::move(data)};
}

Result<std::string> required_text(const Json &row, std::string_view field,
                                  std::string_view context) {
  if (!row.contains(field) || !row[field].is_string() ||
      row[field].get_ref<const std::string &>().empty()) {
    return std::unexpected{transaction_error(std::string{context} + "." + std::string{field} +
                                             " must be a non-empty string")};
  }
  return row[field].get<std::string>();
}

Result<void> exact_fields(const Json &row, std::initializer_list<std::string_view> expected,
                          std::string_view context) {
  if (!row.is_object() || row.size() != expected.size()) {
    return std::unexpected{transaction_error(std::string{context} + " has invalid fields")};
  }
  for (const auto field : expected) {
    if (!row.contains(field)) {
      return std::unexpected{
          transaction_error(std::string{context} + " is missing field " + std::string{field})};
    }
  }
  return {};
}

Result<std::uint8_t> midi_value(const Json &row, std::string_view field, std::string_view context,
                                std::uint8_t default_value, bool required) {
  if (!row.contains(field)) {
    if (!required)
      return default_value;
    return std::unexpected{
        transaction_error(std::string{context} + " is missing field " + std::string{field})};
  }
  if (!row[field].is_number_integer()) {
    return std::unexpected{
        transaction_error(std::string{context} + "." + std::string{field} + " must be an integer")};
  }
  const auto value = row[field].get<int>();
  if (value < 0 || value > 127) {
    return std::unexpected{transaction_error(std::string{context} + "." + std::string{field} +
                                             " must be between 0 and 127")};
  }
  return static_cast<std::uint8_t>(value);
}

Result<std::uint8_t> program_value(const Json &row, std::string_view field,
                                   std::string_view context) {
  if (!row.contains(field) || !row[field].is_number_integer()) {
    return std::unexpected{
        transaction_error(std::string{context} + "." + std::string{field} + " must be an integer")};
  }
  const auto value = row[field].get<int>();
  if (value < 1 || value > 128) {
    return std::unexpected{transaction_error(std::string{context} + "." + std::string{field} +
                                             " must be between 1 and 128")};
  }
  return static_cast<std::uint8_t>(value);
}

Result<std::string> object_name(const Json &row, std::string_view field, std::string_view context) {
  auto result = required_text(row, field, context);
  if (!result)
    return std::unexpected{result.error()};
  if (result->size() > 16U ||
      !std::ranges::all_of(*result, [](unsigned char value) { return value < 0x80U; })) {
    return std::unexpected{transaction_error(std::string{context} + "." + std::string{field} +
                                             " must fit 16 ASCII bytes")};
  }
  return result;
}

const IndexRecord *record(const Partition &partition, SfsId id) {
  const auto found = std::ranges::find(partition.records, id, &IndexRecord::sfs_id);
  return found == partition.records.end() ? nullptr : &*found;
}

const MutablePartition::InsertedRecord *current_record(const MutablePartition &partition,
                                                       SfsId id) {
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

Result<std::vector<std::byte>> current_payload(TransactionState &state, MutablePartition &partition,
                                               SfsId id, const CancellationToken &cancellation) {
  if (id.value == 1U && partition.root_payload)
    return *partition.root_payload;
  if (const auto *item = current_record(partition, id); item != nullptr)
    return item->payload;
  if (partition.deleted.contains(id) || record(*partition.source, id) == nullptr) {
    return std::unexpected{transaction_error("SFS record does not exist in transaction state")};
  }
  return state.container.read_record_data(partition.source->index, id, 64U * 1024U * 1024U,
                                          cancellation);
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

Result<std::vector<ParsedDirectoryEntry>> parse_directory(std::span<const std::byte> payload,
                                                          SfsId id) {
  std::vector<ParsedDirectoryEntry> result;
  for (std::size_t offset = 0; offset + 32U <= payload.size(); offset += 32U) {
    const auto row = payload.subspan(offset, 32U);
    if (std::ranges::all_of(row.first(8U), [](std::byte value) { return value == std::byte{0}; })) {
      break;
    }
    const auto link = (std::to_integer<std::uint32_t>(row[4]) << 24U) |
                      (std::to_integer<std::uint32_t>(row[5]) << 16U) |
                      (std::to_integer<std::uint32_t>(row[6]) << 8U) |
                      std::to_integer<std::uint32_t>(row[7]);
    result.push_back({SfsId{link}, directory_name(row), offset});
  }
  if (result.empty() || result.front().name != ".") {
    return std::unexpected{
        transaction_error("SFS ID " + std::to_string(id.value) + " is not a readable directory")};
  }
  return result;
}

Result<std::vector<std::byte>> read_raw(const std::filesystem::path &path, std::uint64_t offset,
                                        std::size_t size) {
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

Result<std::vector<std::byte>> current_root_payload(TransactionState &state,
                                                    MutablePartition &partition,
                                                    const CancellationToken &cancellation) {
  if (partition.root_payload)
    return *partition.root_payload;
  return state.container.read_record_data(partition.source->index, SfsId{1}, 64U * 1024U,
                                          cancellation);
}

Result<void> set_root_payload(TransactionState &state, MutablePartition &partition,
                              std::vector<std::byte> payload,
                              const CancellationToken &cancellation) {
  const auto *root = record(*partition.source, SfsId{1});
  if (root == nullptr || root->extents.size() != 1U || !root->continuation_clusters.empty() ||
      payload.size() > static_cast<std::size_t>(root->extents[0].cluster_count) * 1024U) {
    return std::unexpected{
        transaction_error("partition root relocation is not enabled for this transaction")};
  }
  Result<std::vector<std::byte>> raw =
      partition.root_index ? Result<std::vector<std::byte>>{*partition.root_index}
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
                                    std::vector<std::byte> payload,
                                    const CancellationToken &cancellation) {
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
        id,
        MutablePartition::InsertedRecord{id, std::move(*raw), std::move(*original), source->extents,
                                         source->continuation_clusters, source->payload_kind});
    static_cast<void>(unused);
    target = &inserted->second;
  }

  std::uint64_t capacity{};
  for (const auto &extent : target->extents) {
    capacity += static_cast<std::uint64_t>(extent.cluster_count) * 1024U;
  }
  if (payload.size() > capacity) {
    return std::unexpected{
        transaction_error("record payload growth exceeds its current extent capacity")};
  }
  put_be32(target->raw_index, 6U, static_cast<std::uint32_t>(payload.size()));
  if (target->extents.size() <= 4U) {
    std::uint32_t remaining = static_cast<std::uint32_t>(payload.size());
    for (std::size_t index = 0; index < target->extents.size(); ++index) {
      const auto capacity_for_extent = target->extents[index].cluster_count * 1024U;
      const auto byte_count =
          remaining == 0U ? capacity_for_extent : std::min(remaining, capacity_for_extent);
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

Result<SfsId> unique_directory_child(TransactionState &state, MutablePartition &partition,
                                     SfsId directory, std::string_view name,
                                     const CancellationToken &cancellation) {
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
    return std::unexpected{
        transaction_error("directory requires exactly one entry named " + std::string{name})};
  }
  if (!record_exists(partition, matches.front())) {
    return std::unexpected{transaction_error("directory entry references a missing SFS record")};
  }
  return matches.front();
}

Result<SfsId> volume_category(TransactionState &state, MutablePartition &partition,
                              std::string_view volume_name, std::string_view category_name,
                              const CancellationToken &cancellation) {
  auto volume = unique_directory_child(state, partition, SfsId{1}, volume_name, cancellation);
  if (!volume)
    return std::unexpected{volume.error()};
  return unique_directory_child(state, partition, *volume, category_name, cancellation);
}

Result<std::pair<SfsId, SfsId>>
category_object(TransactionState &state, MutablePartition &partition, std::string_view volume_name,
                std::string_view category_name, std::string_view object_name,
                std::string_view expected_type, const CancellationToken &cancellation) {
  auto category = volume_category(state, partition, volume_name, category_name, cancellation);
  if (!category)
    return std::unexpected{category.error()};
  auto object = unique_directory_child(state, partition, *category, object_name, cancellation);
  if (!object)
    return std::unexpected{object.error()};
  auto payload = current_payload(state, partition, *object, cancellation);
  if (!payload)
    return std::unexpected{payload.error()};
  if (payload->size() < 16U || !std::equal(expected_type.begin(), expected_type.end(),
                                           payload->begin() + 12U, [](char left, std::byte right) {
                                             return static_cast<unsigned char>(left) ==
                                                    std::to_integer<unsigned char>(right);
                                           })) {
    return std::unexpected{transaction_error(std::string{object_name} +
                                             " does not resolve to one readable " +
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

Result<std::vector<CategoryObject>>
category_objects(TransactionState &state, MutablePartition &partition, std::string_view volume_name,
                 std::string_view category_name, ObjectType expected_type,
                 const CancellationToken &cancellation) {
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
      return std::unexpected{
          transaction_error("category contains an unresolved or incorrectly typed object")};
    }
    result.push_back(
        CategoryObject{entry.name, entry.id, std::move(*object_payload), std::move(*decoded)});
  }
  return result;
}

Result<void> replace_fixed_object_payload(TransactionState &state, MutablePartition &partition,
                                          SfsId id, std::vector<std::byte> payload,
                                          const CancellationToken &cancellation) {
  auto current = current_payload(state, partition, id, cancellation);
  if (!current)
    return std::unexpected{current.error()};
  if (payload.size() != current->size()) {
    return std::unexpected{
        transaction_error("fixed-size object metadata update changed payload size")};
  }
  return replace_record_payload(state, partition, id, std::move(payload), cancellation);
}

Result<bool> sbnk_program_bit(std::span<const std::byte> payload, std::uint8_t program) {
  const auto offset = 0xc0U + static_cast<std::size_t>((program - 1U) / 32U) * 4U;
  if (payload.size() < offset + 4U) {
    return std::unexpected{
        transaction_error("SBNK payload is too short for its Program-link bitmap")};
  }
  const auto word = (std::to_integer<std::uint32_t>(payload[offset]) << 24U) |
                    (std::to_integer<std::uint32_t>(payload[offset + 1U]) << 16U) |
                    (std::to_integer<std::uint32_t>(payload[offset + 2U]) << 8U) |
                    std::to_integer<std::uint32_t>(payload[offset + 3U]);
  return (word & (std::uint32_t{1} << ((program - 1U) % 32U))) != 0U;
}

Result<void> set_sbnk_program_bit(TransactionState &state, MutablePartition &partition, SfsId id,
                                  std::uint8_t program, bool enabled,
                                  const CancellationToken &cancellation) {
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

Result<void> set_sbnk_group_flag(TransactionState &state, MutablePartition &partition, SfsId id,
                                 bool enabled, const CancellationToken &cancellation) {
  auto payload = current_payload(state, partition, id, cancellation);
  if (!payload)
    return std::unexpected{payload.error()};
  if (payload->size() <= 0xd0U) {
    return std::unexpected{
        transaction_error("SBNK payload is too short for its sample-bank-group flag")};
  }
  auto value = std::to_integer<std::uint8_t>((*payload)[0xd0U]);
  value =
      enabled ? static_cast<std::uint8_t>(value | 1U) : static_cast<std::uint8_t>(value & 0xfeU);
  (*payload)[0xd0U] = static_cast<std::byte>(value);
  return replace_fixed_object_payload(state, partition, id, std::move(*payload), cancellation);
}

Result<void> remove_directory_entry(TransactionState &state, MutablePartition &partition,
                                    SfsId directory, SfsId child, std::string_view name,
                                    const CancellationToken &cancellation) {
  auto payload = current_payload(state, partition, directory, cancellation);
  if (!payload)
    return std::unexpected{payload.error()};
  auto entries = parse_directory(*payload, directory);
  if (!entries)
    return std::unexpected{entries.error()};
  const auto found = std::ranges::find_if(*entries, [&](const ParsedDirectoryEntry &entry) {
    return entry.id == child && entry.name == name;
  });
  if (found == entries->end()) {
    return std::unexpected{transaction_error("directory entry is absent from transaction state")};
  }
  payload->erase(payload->begin() + static_cast<std::ptrdiff_t>(found->offset),
                 payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 32U));
  return replace_record_payload(state, partition, directory, std::move(*payload), cancellation);
}

Result<PartitionIndex> resolve_partition(const TransactionState &state,
                                         const PartitionSelector &selector) {
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
    if (result.contains(item.sfs_id) || item.sfs_id.value == 1U ||
        item.payload_kind != PayloadKind::directory)
      continue;
    for (const auto &child : item.directory_entries) {
      if (child.name != "." && child.name != ".." && result.contains(SfsId{child.link_id.value})) {
        return std::unexpected{
            transaction_error("a directory outside the volume references its closure")};
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
  if (root == nullptr || root->payload_kind != PayloadKind::directory ||
      root->extents.size() != 1U || !root->continuation_clusters.empty()) {
    return std::unexpected{transaction_error("partition root must use one readable direct extent")};
  }
  std::vector<const DirectoryEntry *> matches;
  for (const auto &entry : root->directory_entries) {
    if (entry.name == operation.volume_name)
      matches.push_back(&entry);
  }
  if (matches.size() != 1U)
    return std::unexpected{
        transaction_error("volume name is not unique in the selected partition")};
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
      return std::unexpected{
          transaction_error("a known object relationship crosses the volume closure")};
    }
  }
  auto payload = current_root_payload(state, mutable_partition, cancellation);
  if (!payload)
    return std::unexpected{payload.error()};
  const auto offset = matches.front()->payload_relative_offset;
  if (offset > payload->size() || payload->size() - offset < 32U) {
    return std::unexpected{
        transaction_error("volume directory entry lies outside the root payload")};
  }
  payload->erase(payload->begin() + static_cast<std::ptrdiff_t>(offset),
                 payload->begin() + static_cast<std::ptrdiff_t>(offset + 32U));
  if (auto replaced = set_root_payload(state, mutable_partition, std::move(*payload), cancellation);
      !replaced)
    return std::unexpected{replaced.error()};
  std::uint64_t freed{};
  for (const auto id : *closure) {
    const auto *item = record(partition, id);
    for (const auto &extent : item->extents) {
      for (std::uint32_t cluster = extent.cluster_offset;
           cluster < extent.cluster_offset + extent.cluster_count; ++cluster) {
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
  const auto capacity =
      (static_cast<std::uint64_t>(partition.source->directory_index_span_clusters) *
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
  const auto first =
      partition.source->directory_index_cluster + partition.source->directory_index_span_clusters;
  for (std::uint32_t cluster = first;
       cluster < partition.source->cluster_count && selected.size() < count; ++cluster) {
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

Result<std::vector<std::uint32_t>> allocate_list_clusters(MutablePartition &partition,
                                                          std::size_t count) {
  std::vector<std::uint32_t> result;
  const auto first =
      partition.source->directory_index_cluster + partition.source->directory_index_span_clusters;
  for (std::uint32_t cluster = first;
       cluster < partition.source->cluster_count && result.size() < count; ++cluster) {
    if (!bitmap_used(partition.bitmap, cluster))
      result.push_back(cluster);
  }
  if (result.size() != count) {
    return std::unexpected{
        transaction_error("partition has insufficient continuation-list clusters")};
  }
  for (const auto cluster : result)
    set_bitmap(partition.bitmap, cluster, true);
  return result;
}

std::vector<std::byte> index_for(const detail::PreparedRecord &source,
                                 const std::vector<Extent> &extents, std::uint32_t size,
                                 std::span<const std::uint32_t> list_clusters = {}) {
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

Result<std::pair<SfsId, std::uint64_t>> allocate_record(MutablePartition &partition,
                                                        std::vector<std::byte> payload,
                                                        PayloadKind payload_kind,
                                                        std::optional<SfsId> requested_id = {},
                                                        std::uint16_t directory_tail = 0U) {
  const auto ids = requested_id ? std::vector{*requested_id} : free_ids(partition, 1U);
  if (ids.empty() || (requested_id && partition.inserted.contains(*requested_id))) {
    return std::unexpected{transaction_error("partition has no free SFS record")};
  }
  const auto clusters =
      std::max<std::uint32_t>(2U, static_cast<std::uint32_t>((payload.size() + 1023U) / 1024U));
  auto extents = allocate_extents(partition, clusters);
  if (!extents)
    return std::unexpected{extents.error()};
  constexpr std::size_t extents_per_list_cluster = (1024U - 12U) / 12U;
  std::vector<std::uint32_t> list_clusters;
  if (extents->size() > 4U) {
    auto allocated_lists = allocate_list_clusters(
        partition, (extents->size() + extents_per_list_cluster - 1U) / extents_per_list_cluster);
    if (!allocated_lists)
      return std::unexpected{allocated_lists.error()};
    list_clusters = std::move(*allocated_lists);
  }
  detail::PreparedRecord prepared;
  prepared.kind = payload_kind == PayloadKind::directory ? detail::RecordKind::directory
                                                         : detail::RecordKind::object;
  prepared.tail = directory_tail;
  auto raw =
      index_for(prepared, *extents, static_cast<std::uint32_t>(payload.size()), list_clusters);
  const auto id = ids.front();
  partition.deleted.erase(id);
  partition.inserted.emplace(id, MutablePartition::InsertedRecord{
                                     id, std::move(raw), std::move(payload), std::move(*extents),
                                     std::move(list_clusters), payload_kind});
  return std::pair{id, clusters + partition.inserted.at(id).continuation_clusters.size()};
}

Result<void> append_directory_entry(TransactionState &state, MutablePartition &partition,
                                    SfsId directory, SfsId child, std::string_view name,
                                    const CancellationToken &cancellation) {
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
  if (std::ranges::any_of(*entries,
                          [&](const ParsedDirectoryEntry &entry) { return entry.name == name; })) {
    return std::unexpected{
        transaction_error("directory already contains entry " + std::string{name})};
  }
  std::array<std::byte, 32> entry{};
  put_be16(entry, 0U, 0x20U);
  put_be16(entry, 2U, 17U);
  put_be32(entry, 4U, child.value);
  std::fill(entry.begin() + 8U, entry.begin() + 24U, std::byte{' '});
  std::ranges::transform(name, entry.begin() + 8U,
                         [](char value) { return static_cast<std::byte>(value); });
  payload->insert(payload->end(), entry.begin(), entry.end());
  return replace_record_payload(state, partition, directory, std::move(*payload), cancellation);
}

Result<void> rename_directory_entry(TransactionState &state, MutablePartition &partition,
                                    SfsId directory, SfsId child, std::string_view old_name,
                                    std::string_view new_name,
                                    const CancellationToken &cancellation) {
  auto payload = current_payload(state, partition, directory, cancellation);
  if (!payload)
    return std::unexpected{payload.error()};
  auto entries = parse_directory(*payload, directory);
  if (!entries)
    return std::unexpected{entries.error()};
  if (std::ranges::any_of(*entries, [&](const auto &entry) {
        return entry.name == new_name && entry.id != child;
      })) {
    return std::unexpected{transaction_error("rename destination already exists")};
  }
  const auto found = std::ranges::find_if(
      *entries, [&](const auto &entry) { return entry.id == child && entry.name == old_name; });
  if (found == entries->end()) {
    return std::unexpected{transaction_error("rename source directory entry is absent")};
  }
  put_be16(*payload, found->offset + 2U, 17U);
  std::fill(payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 8U),
            payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 24U), std::byte{' '});
  std::ranges::transform(new_name,
                         payload->begin() + static_cast<std::ptrdiff_t>(found->offset + 8U),
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
    return std::unexpected{
        transaction_error("object payload name disagrees with directory identity")};
  }
  std::fill(payload->begin() + 0x32, payload->begin() + 0x42, std::byte{' '});
  std::ranges::transform(new_name, payload->begin() + 0x32,
                         [](char value) { return static_cast<std::byte>(value); });
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
    for (std::uint32_t cluster = extent.cluster_offset;
         cluster < extent.cluster_offset + extent.cluster_count; ++cluster) {
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

std::vector<std::byte> remap_directory(std::vector<std::byte> payload,
                                       const std::map<std::uint32_t, SfsId> &ids) {
  const auto directory_id = payload.size() >= 8U
                                ? (std::to_integer<std::uint32_t>(payload[4]) << 24U) |
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
    if (old == 1U && payload[offset + 8U] == std::byte{'.'} &&
        payload[offset + 9U] == std::byte{'.'}) {
      const auto mapped = ids.find(directory_id);
      payload[offset + 11U] =
          static_cast<std::byte>(mapped == ids.end() ? 0U : mapped->second.value & 0xffU);
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
    const auto length = static_cast<std::size_t>(
        std::to_integer<std::uint16_t>((*root_payload)[offset + 2U]) << 8U |
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
  HdsBuildManifest template_manifest{"1.0", minimum_hds_size, {{"AXK ALTER", {*operation.volume}}}};
  auto geometry = plan_hds_geometry(template_manifest);
  if (!geometry)
    return std::unexpected{geometry.error()};
  auto prepared = detail::prepare_partition_records(template_manifest.partitions[0], (*geometry)[0],
                                                    1, cancellation);
  if (!prepared)
    return std::unexpected{prepared.error()};
  std::vector<detail::PreparedRecord> templates;
  std::ranges::copy_if(*prepared, std::back_inserter(templates),
                       [](const auto &item) { return item.id >= 3U; });
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
                                  templates[index].kind == detail::RecordKind::directory
                                      ? PayloadKind::directory
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
    entry[8U + index] =
        index < encoded.size() ? static_cast<std::byte>(encoded[index]) : std::byte{' '};
  root_payload->insert(root_payload->end(), entry.begin(), entry.end());
  if (auto replaced = set_root_payload(state, partition, std::move(*root_payload), cancellation);
      !replaced)
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
  auto located = category_object(state, partition, operation.volume_name, "SBNK",
                                 operation.sample_bank_name, "SBNK", cancellation);
  if (!located)
    return std::unexpected{located.error()};
  const auto [directory_id, bank_id] = *located;
  auto payload = current_payload(state, partition, bank_id, cancellation);
  if (!payload)
    return std::unexpected{payload.error()};
  auto decoded = decode_object(*payload);
  if (!decoded)
    return std::unexpected{decoded.error()};
  const auto *bank = std::get_if<CurrentSbnk>(&decoded->payload);
  if (bank == nullptr) {
    return std::unexpected{transaction_error("sample bank is not a current SBNK object")};
  }
  if (!bank->linked_program_numbers.empty()) {
    return std::unexpected{
        transaction_error("sample bank is referenced by its Program link bitmap")};
  }
  for (const auto &[edge_partition, source, target] : state.known_edges) {
    if (edge_partition == *partition_index && target == bank_id &&
        record_exists(partition, source)) {
      return std::unexpected{
          transaction_error("sample bank is referenced by a Program or sample-bank group")};
    }
  }
  if (auto removed = remove_directory_entry(state, partition, directory_id, bank_id,
                                            operation.sample_bank_name, cancellation);
      !removed) {
    return std::unexpected{removed.error()};
  }
  auto freed = release_record(partition, bank_id);
  if (!freed)
    return std::unexpected{freed.error()};
  std::erase_if(state.known_edges, [&](const auto &edge) {
    const auto &[edge_partition, source, target] = edge;
    return edge_partition == *partition_index && (source == bank_id || target == bank_id);
  });
  OperationReport report;
  report.id = operation.id;
  report.type = operation.type;
  report.partition = *partition_index;
  report.volume_name = operation.volume_name;
  report.object_name = operation.sample_bank_name;
  report.removed_sfs_ids = {bank_id};
  report.freed_clusters = *freed;
  return report;
}

Result<detail::PreparedWaveformMember>
waveform_member(TransactionState &state, MutablePartition &partition, std::string_view volume_name,
                std::string_view waveform_name, const CancellationToken &cancellation) {
  auto located =
      category_object(state, partition, volume_name, "SMPL", waveform_name, "SMPL", cancellation);
  if (!located)
    return std::unexpected{located.error()};
  auto payload = current_payload(state, partition, located->second, cancellation);
  if (!payload)
    return std::unexpected{payload.error()};
  auto decoded = decode_object(*payload);
  if (!decoded)
    return std::unexpected{decoded.error()};
  const auto *sample = std::get_if<CurrentSmpl>(&decoded->payload);
  if (sample == nullptr || sample->link_id.value == 0U) {
    return std::unexpected{transaction_error("waveform has no usable current SMPL link ID")};
  }
  return detail::PreparedWaveformMember{std::string{waveform_name}, sample->link_id.value,
                                        sample->duplicate_sample_rate.value,
                                        sample->wave_length_frames.value};
}

Result<OperationReport> insert_sbnk(TransactionState &state, const OperationView &operation,
                                    const CancellationToken &cancellation) {
  auto partition_index = resolve_partition(state, operation.partition);
  if (!partition_index)
    return std::unexpected{partition_index.error()};
  const auto found = state.partitions.find(partition_index->value);
  if (found == state.partitions.end() || !operation.sample_bank) {
    return std::unexpected{transaction_error("insert-sbnk target is invalid")};
  }
  auto &partition = found->second;
  const auto &spec = *operation.sample_bank;
  auto directory = volume_category(state, partition, operation.volume_name, "SBNK", cancellation);
  if (!directory)
    return std::unexpected{directory.error()};
  auto directory_payload = current_payload(state, partition, *directory, cancellation);
  if (!directory_payload)
    return std::unexpected{directory_payload.error()};
  auto entries = parse_directory(*directory_payload, *directory);
  if (!entries)
    return std::unexpected{entries.error()};
  if (std::ranges::any_of(
          *entries, [&](const ParsedDirectoryEntry &entry) { return entry.name == spec.name; })) {
    return std::unexpected{transaction_error("volume already contains the requested sample bank")};
  }
  if (!spec.waveform_id) {
    return std::unexpected{transaction_error("sample bank requires waveform_name")};
  }
  auto left =
      waveform_member(state, partition, operation.volume_name, *spec.waveform_id, cancellation);
  if (!left)
    return std::unexpected{left.error()};
  std::optional<detail::PreparedWaveformMember> right;
  if (spec.right_waveform_id) {
    auto member = waveform_member(state, partition, operation.volume_name, *spec.right_waveform_id,
                                  cancellation);
    if (!member)
      return std::unexpected{member.error()};
    if (member->sample_rate != left->sample_rate || member->frame_count != left->frame_count) {
      return std::unexpected{
          transaction_error("stereo sample bank requires matching sample rates and frame counts")};
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
  if (auto appended =
          append_directory_entry(state, partition, *directory, bank_id, spec.name, cancellation);
      !appended) {
    return std::unexpected{appended.error()};
  }
  const auto left_object = category_object(state, partition, operation.volume_name, "SMPL",
                                           *spec.waveform_id, "SMPL", cancellation);
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
      return std::unexpected{
          transaction_error("volume already contains a requested waveform name")};
    }
    auto payload = current_payload(state, partition, entry.id, cancellation);
    if (!payload)
      return std::unexpected{
          transaction_error("existing SMPL record is unresolved; link-ID allocation is unsafe")};
    auto decoded = decode_object(*payload);
    if (!decoded)
      return std::unexpected{decoded.error()};
    const auto *sample = std::get_if<CurrentSmpl>(&decoded->payload);
    if (sample == nullptr || sample->link_id.value == 0U) {
      return std::unexpected{transaction_error("existing waveform has no current SMPL link ID")};
    }
    link_ids.insert(sample->link_id.value);
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
  report.audio_import = AudioImportSummary{
      audio->source_path,           audio->source_format,      audio->source_subtype,
      audio->source_channels,       audio->source_sample_rate, audio->output_sample_rate,
      audio->output_frames,         audio->resampled,          audio->quantized,
      audio->source_channels == 2U, audio->clipped_samples};
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
  auto located = category_object(state, partition, operation.volume_name, "SMPL",
                                 operation.waveform_name, "SMPL", cancellation);
  if (!located)
    return std::unexpected{located.error()};
  const auto [directory_id, waveform_id] = *located;
  auto waveform_payload = current_payload(state, partition, waveform_id, cancellation);
  if (!waveform_payload)
    return std::unexpected{waveform_payload.error()};
  auto waveform_object = decode_object(*waveform_payload);
  if (!waveform_object)
    return std::unexpected{waveform_object.error()};
  const auto *waveform = std::get_if<CurrentSmpl>(&waveform_object->payload);
  if (waveform == nullptr || waveform->link_id.value == 0U) {
    return std::unexpected{
        transaction_error("waveform cannot be classified as known_unreferenced")};
  }

  auto bank_directory =
      volume_category(state, partition, operation.volume_name, "SBNK", cancellation);
  if (!bank_directory)
    return std::unexpected{bank_directory.error()};
  auto bank_directory_payload = current_payload(state, partition, *bank_directory, cancellation);
  if (!bank_directory_payload)
    return std::unexpected{bank_directory_payload.error()};
  auto bank_entries = parse_directory(*bank_directory_payload, *bank_directory);
  if (!bank_entries)
    return std::unexpected{bank_entries.error()};
  for (const auto &entry : *bank_entries) {
    if (entry.name == "." || entry.name == "..")
      continue;
    auto payload = current_payload(state, partition, entry.id, cancellation);
    if (!payload) {
      return std::unexpected{
          transaction_error("waveform ownership is ambiguous because an SBNK is unreadable")};
    }
    auto object = decode_object(*payload);
    if (!object)
      return std::unexpected{object.error()};
    const auto *bank = std::get_if<CurrentSbnk>(&object->payload);
    if (bank == nullptr) {
      return std::unexpected{
          transaction_error("waveform ownership is ambiguous because an SBNK entry is unresolved")};
    }
    const auto references = [&](const CurrentSbnkMember &member) {
      return member.smpl_link_id == waveform->link_id.value ||
             member.sample_name == operation.waveform_name;
    };
    if (references(bank->left) || (bank->right && references(*bank->right))) {
      return std::unexpected{transaction_error("waveform is referenced, not known_unreferenced")};
    }
  }
  for (const auto &[edge_partition, source, target] : state.known_edges) {
    static_cast<void>(source);
    if (edge_partition == *partition_index && target == waveform_id) {
      return std::unexpected{
          transaction_error("waveform has a known incoming reference, not known_unreferenced")};
    }
  }
  if (auto removed = remove_directory_entry(state, partition, directory_id, waveform_id,
                                            operation.waveform_name, cancellation);
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
  auto located =
      category_object(state, partition, operation.volume_name, "PROG", name, "PROG", cancellation);
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
  std::set<SfsId> assigned_banks;
  for (const auto &assignment : program->assignments) {
    if (assignment.name.empty() || assignment.kind != 0x10U)
      continue;
    auto bank = category_object(state, partition, operation.volume_name, "SBNK", assignment.name,
                                "SBNK", cancellation);
    if (!bank)
      return std::unexpected{bank.error()};
    assigned_banks.insert(bank->second);
  }
  auto banks = category_objects(state, partition, operation.volume_name, "SBNK", ObjectType::sbnk,
                                cancellation);
  if (!banks)
    return std::unexpected{banks.error()};
  std::set<SfsId> bitmap_banks;
  for (const auto &bank : *banks) {
    auto bit = sbnk_program_bit(bank.payload, *operation.program_number);
    if (!bit)
      return std::unexpected{bit.error()};
    if (*bit)
      bitmap_banks.insert(bank.id);
  }
  if (assigned_banks != bitmap_banks) {
    return std::unexpected{
        transaction_error("Program direct assignments do not match SBNK Program-link bitmaps")};
  }
  for (const auto id : assigned_banks) {
    if (auto updated = set_sbnk_program_bit(state, partition, id, *operation.program_number, false,
                                            cancellation);
        !updated)
      return std::unexpected{updated.error()};
  }
  if (auto removed = remove_directory_entry(state, partition, located->first, located->second, name,
                                            cancellation);
      !removed)
    return std::unexpected{removed.error()};
  auto freed = release_record(partition, located->second);
  if (!freed)
    return std::unexpected{freed.error()};
  std::erase_if(state.known_edges, [&](const auto &edge) {
    const auto &[edge_partition, source, target] = edge;
    return edge_partition == *partition_index &&
           (source == located->second || target == located->second);
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
  const auto &group_assignment = spec.assignments[0];
  const auto &bank_assignment = spec.assignments[1];
  auto group = category_object(state, partition, operation.volume_name, "SBAC",
                               group_assignment.target_name, "SBAC", cancellation);
  if (!group)
    return std::unexpected{group.error()};
  auto bank = category_object(state, partition, operation.volume_name, "SBNK",
                              bank_assignment.target_name, "SBNK", cancellation);
  if (!bank)
    return std::unexpected{bank.error()};
  auto existing_programs = category_objects(state, partition, operation.volume_name, "PROG",
                                            ObjectType::prog, cancellation);
  if (!existing_programs)
    return std::unexpected{existing_programs.error()};
  for (const auto &existing : *existing_programs) {
    const auto *decoded_program = std::get_if<CurrentProg>(&existing.decoded.payload);
    for (const auto &assignment : decoded_program->assignments) {
      if ((assignment.kind == 0x11U && assignment.name == group_assignment.target_name) ||
          (assignment.kind == 0x10U && assignment.name == bank_assignment.target_name)) {
        return std::unexpected{
            transaction_error("Program target is already assigned by another Program")};
      }
    }
  }
  auto bank_payload = current_payload(state, partition, bank->second, cancellation);
  if (!bank_payload)
    return std::unexpected{bank_payload.error()};
  auto bit = sbnk_program_bit(*bank_payload, spec.number);
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
  if (auto appended = append_directory_entry(state, partition, *directory, allocated->first, name,
                                             cancellation);
      !appended)
    return std::unexpected{appended.error()};
  if (auto updated =
          set_sbnk_program_bit(state, partition, bank->second, spec.number, true, cancellation);
      !updated)
    return std::unexpected{updated.error()};
  state.known_edges.emplace_back(*partition_index, allocated->first, group->second);
  state.known_edges.emplace_back(*partition_index, allocated->first, bank->second);
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
  auto located = category_object(state, partition, operation.volume_name, "SBAC",
                                 operation.sample_bank_group_name, "SBAC", cancellation);
  if (!located)
    return std::unexpected{located.error()};
  auto payload = current_payload(state, partition, located->second, cancellation);
  if (!payload)
    return std::unexpected{payload.error()};
  auto decoded = decode_object(*payload);
  if (!decoded)
    return std::unexpected{decoded.error()};
  const auto *group = std::get_if<CurrentSbac>(&decoded->payload);
  if (group == nullptr || group->active_slot_count > group->maximum_slot_count) {
    return std::unexpected{transaction_error("sample-bank group slots are unreadable")};
  }
  auto programs = category_objects(state, partition, operation.volume_name, "PROG",
                                   ObjectType::prog, cancellation);
  if (!programs)
    return std::unexpected{programs.error()};
  for (const auto &program_row : *programs) {
    const auto *program = std::get_if<CurrentProg>(&program_row.decoded.payload);
    if (std::ranges::any_of(program->assignments, [&](const ProgAssignment &assignment) {
          return assignment.kind == 0x11U && assignment.name == operation.sample_bank_group_name;
        })) {
      return std::unexpected{transaction_error("sample-bank group is referenced by a Program")};
    }
  }
  auto groups = category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac,
                                 cancellation);
  if (!groups)
    return std::unexpected{groups.error()};
  std::set<std::string> members;
  for (const auto &slot : group->slots)
    members.insert(slot.name);
  for (const auto &other : *groups) {
    if (other.id == located->second)
      continue;
    const auto *other_group = std::get_if<CurrentSbac>(&other.decoded.payload);
    for (const auto &slot : other_group->slots) {
      if (members.contains(slot.name)) {
        return std::unexpected{transaction_error("another sample-bank group shares a member")};
      }
    }
  }
  for (const auto &slot : group->slots) {
    auto bank = category_object(state, partition, operation.volume_name, "SBNK", slot.name, "SBNK",
                                cancellation);
    if (!bank)
      return std::unexpected{bank.error()};
    auto bank_payload = current_payload(state, partition, bank->second, cancellation);
    if (!bank_payload)
      return std::unexpected{bank_payload.error()};
    if (bank_payload->size() <= 0xd0U ||
        (std::to_integer<std::uint8_t>((*bank_payload)[0xd0U]) & 1U) == 0U) {
      return std::unexpected{transaction_error("member SBNK is missing its grouped flag")};
    }
    if (auto updated = set_sbnk_group_flag(state, partition, bank->second, false, cancellation);
        !updated)
      return std::unexpected{updated.error()};
  }
  if (auto removed = remove_directory_entry(state, partition, located->first, located->second,
                                            operation.sample_bank_group_name, cancellation);
      !removed)
    return std::unexpected{removed.error()};
  auto freed = release_record(partition, located->second);
  if (!freed)
    return std::unexpected{freed.error()};
  std::erase_if(state.known_edges, [&](const auto &edge) {
    const auto &[edge_partition, source, target] = edge;
    return edge_partition == *partition_index &&
           (source == located->second || target == located->second);
  });
  OperationReport report;
  report.id = operation.id;
  report.type = operation.type;
  report.partition = *partition_index;
  report.volume_name = operation.volume_name;
  report.object_name = operation.sample_bank_group_name;
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
  if (found == state.partitions.end() || !operation.sample_bank_group) {
    return std::unexpected{transaction_error("insert-sbac target is invalid")};
  }
  auto &partition = found->second;
  const auto &spec = *operation.sample_bank_group;
  auto directory = volume_category(state, partition, operation.volume_name, "SBAC", cancellation);
  if (!directory)
    return std::unexpected{directory.error()};
  auto existing_groups = category_objects(state, partition, operation.volume_name, "SBAC",
                                          ObjectType::sbac, cancellation);
  if (!existing_groups)
    return std::unexpected{existing_groups.error()};
  std::set<std::string> existing_members;
  for (const auto &existing : *existing_groups) {
    if (existing.name == spec.name) {
      return std::unexpected{transaction_error("sample-bank group already exists")};
    }
    const auto *group = std::get_if<CurrentSbac>(&existing.decoded.payload);
    for (const auto &slot : group->slots)
      existing_members.insert(slot.name);
  }
  std::map<std::string, SampleBankSpec> bank_specs;
  std::vector<SfsId> member_ids;
  for (const auto &name : spec.member_sample_banks) {
    if (existing_members.contains(name)) {
      return std::unexpected{transaction_error("SBNK already belongs to another group")};
    }
    auto bank = category_object(state, partition, operation.volume_name, "SBNK", name, "SBNK",
                                cancellation);
    if (!bank)
      return std::unexpected{bank.error()};
    auto bank_payload = current_payload(state, partition, bank->second, cancellation);
    if (!bank_payload)
      return std::unexpected{bank_payload.error()};
    auto decoded = decode_object(*bank_payload);
    if (!decoded)
      return std::unexpected{decoded.error()};
    const auto *current_bank = std::get_if<CurrentSbnk>(&decoded->payload);
    if (current_bank == nullptr || current_bank->right_slot_present ||
        (std::to_integer<std::uint8_t>((*bank_payload)[0xd0U]) & 1U) != 0U) {
      return std::unexpected{
          transaction_error("SBAC profile requires ungrouped mono SBNK members")};
    }
    SampleBankSpec placeholder;
    placeholder.name = name;
    bank_specs.emplace(name, std::move(placeholder));
    member_ids.push_back(bank->second);
  }
  auto payload = detail::prepare_sbac_payload(spec, bank_specs);
  if (!payload)
    return std::unexpected{payload.error()};
  auto allocated = allocate_record(partition, std::move(*payload), PayloadKind::object);
  if (!allocated)
    return std::unexpected{allocated.error()};
  for (const auto id : member_ids) {
    if (auto updated = set_sbnk_group_flag(state, partition, id, true, cancellation); !updated)
      return std::unexpected{updated.error()};
    state.known_edges.emplace_back(*partition_index, allocated->first, id);
  }
  if (auto appended = append_directory_entry(state, partition, *directory, allocated->first,
                                             spec.name, cancellation);
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
  auto located = category_object(state, partition, operation.volume_name, "SMPL",
                                 operation.waveform_name, "SMPL", cancellation);
  if (!located)
    return std::unexpected{located.error()};
  auto waveform_payload = current_payload(state, partition, located->second, cancellation);
  if (!waveform_payload)
    return std::unexpected{waveform_payload.error()};
  auto waveform_object = decode_object(*waveform_payload);
  if (!waveform_object)
    return std::unexpected{waveform_object.error()};
  const auto *waveform = std::get_if<CurrentSmpl>(&waveform_object->payload);
  if (waveform == nullptr || waveform->link_id.value == 0U)
    return std::unexpected{transaction_error("waveform has no current link identity")};
  auto waveforms = category_objects(state, partition, operation.volume_name, "SMPL",
                                    ObjectType::smpl, cancellation);
  if (!waveforms)
    return std::unexpected{waveforms.error()};
  for (const auto &other : *waveforms) {
    if (other.id == located->second)
      continue;
    const auto *sample = std::get_if<CurrentSmpl>(&other.decoded.payload);
    if (other.name == operation.new_waveform_name ||
        sample->link_id.value == waveform->link_id.value) {
      return std::unexpected{transaction_error("waveform rename identity is not unique")};
    }
  }
  auto banks = category_objects(state, partition, operation.volume_name, "SBNK", ObjectType::sbnk,
                                cancellation);
  if (!banks)
    return std::unexpected{banks.error()};
  std::set<SfsId> updated_banks;
  for (const auto &bank_row : *banks) {
    const auto *bank = std::get_if<CurrentSbnk>(&bank_row.decoded.payload);
    std::vector<std::size_t> offsets;
    const auto inspect = [&](const CurrentSbnkMember &member, std::size_t offset) -> Result<void> {
      const auto name_matches = member.sample_name == operation.waveform_name;
      const auto link_matches = member.smpl_link_id == waveform->link_id.value;
      if (name_matches != link_matches)
        return std::unexpected{transaction_error("SBNK waveform name and link identity disagree")};
      if (name_matches)
        offsets.push_back(offset);
      return {};
    };
    if (auto checked = inspect(bank->left, 0x78U); !checked)
      return std::unexpected{checked.error()};
    if (bank->right) {
      if (auto checked = inspect(*bank->right, 0x88U); !checked)
        return std::unexpected{checked.error()};
    }
    if (!offsets.empty()) {
      auto payload = bank_row.payload;
      for (const auto offset : offsets)
        put_padded_name(payload, offset, operation.new_waveform_name);
      if (auto replaced = replace_fixed_object_payload(state, partition, bank_row.id,
                                                       std::move(payload), cancellation);
          !replaced)
        return std::unexpected{replaced.error()};
      updated_banks.insert(bank_row.id);
    }
  }
  for (const auto &[edge_partition, source, target] : state.known_edges) {
    if (edge_partition == *partition_index && target == located->second &&
        !updated_banks.contains(source))
      return std::unexpected{
          transaction_error("known waveform references exceed exact rename set")};
  }
  if (auto renamed =
          rename_object_payload(state, partition, located->second, operation.waveform_name,
                                operation.new_waveform_name, cancellation);
      !renamed)
    return std::unexpected{renamed.error()};
  if (auto renamed = rename_directory_entry(state, partition, located->first, located->second,
                                            operation.waveform_name, operation.new_waveform_name,
                                            cancellation);
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
  auto located = category_object(state, partition, operation.volume_name, "SBAC",
                                 operation.sample_bank_group_name, "SBAC", cancellation);
  if (!located)
    return std::unexpected{located.error()};
  auto groups = category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac,
                                 cancellation);
  if (!groups)
    return std::unexpected{groups.error()};
  if (std::ranges::any_of(*groups, [&](const auto &group) {
        return group.id != located->second && group.name == operation.new_sample_bank_group_name;
      }))
    return std::unexpected{transaction_error("sample-bank group rename destination exists")};
  auto group_payload = current_payload(state, partition, located->second, cancellation);
  if (!group_payload)
    return std::unexpected{group_payload.error()};
  auto group_object = decode_object(*group_payload);
  if (!group_object)
    return std::unexpected{group_object.error()};
  const auto *group = std::get_if<CurrentSbac>(&group_object->payload);
  if (group == nullptr || group->slots.empty() || group->slots.size() > 3U ||
      group->slots.size() != group->active_slot_count)
    return std::unexpected{transaction_error("SBAC rename requires 1..3 readable slots")};
  std::set<SfsId> member_ids;
  for (const auto &slot : group->slots) {
    if (slot.raw_handle != 0U)
      return std::unexpected{transaction_error("SBAC member has unsupported nonzero handle")};
    auto member = category_object(state, partition, operation.volume_name, "SBNK", slot.name,
                                  "SBNK", cancellation);
    if (!member)
      return std::unexpected{member.error()};
    auto member_payload = current_payload(state, partition, member->second, cancellation);
    if (!member_payload)
      return std::unexpected{member_payload.error()};
    if (member_payload->size() <= 0xd0U ||
        (std::to_integer<std::uint8_t>((*member_payload)[0xd0U]) & 1U) == 0U) {
      return std::unexpected{transaction_error("SBAC member is missing its grouped flag")};
    }
    member_ids.insert(member->second);
  }
  for (const auto &other : *groups) {
    if (other.id == located->second)
      continue;
    const auto *other_group = std::get_if<CurrentSbac>(&other.decoded.payload);
    if (std::ranges::any_of(other_group->slots, [&](const SbacSlot &slot) {
          return std::ranges::any_of(group->slots,
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
  auto programs = category_objects(state, partition, operation.volume_name, "PROG",
                                   ObjectType::prog, cancellation);
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
      if (assignment.name == operation.new_sample_bank_group_name)
        return std::unexpected{transaction_error("Program already assigns rename destination")};
      if (assignment.name != operation.sample_bank_group_name)
        continue;
      if (assignment.raw_handle != 0U)
        return std::unexpected{
            transaction_error("Program assignment has unsupported nonzero handle")};
      put_padded_name(payload, 0x120U + index * 0x38U, operation.new_sample_bank_group_name);
      changed = true;
    }
    if (changed) {
      if (auto replaced = replace_fixed_object_payload(state, partition, program_row.id,
                                                       std::move(payload), cancellation);
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
    return std::unexpected{
        transaction_error("SBAC raw Program references disagree with known edges")};
  }
  if (auto renamed =
          rename_object_payload(state, partition, located->second, operation.sample_bank_group_name,
                                operation.new_sample_bank_group_name, cancellation);
      !renamed)
    return std::unexpected{renamed.error()};
  if (auto renamed = rename_directory_entry(state, partition, located->first, located->second,
                                            operation.sample_bank_group_name,
                                            operation.new_sample_bank_group_name, cancellation);
      !renamed)
    return std::unexpected{renamed.error()};
  OperationReport report;
  report.id = operation.id;
  report.type = operation.type;
  report.partition = *partition_index;
  report.volume_name = operation.volume_name;
  report.object_name = operation.new_sample_bank_group_name;
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
  auto located = category_object(state, partition, operation.volume_name, "SBNK",
                                 operation.sample_bank_name, "SBNK", cancellation);
  if (!located)
    return std::unexpected{located.error()};
  auto banks = category_objects(state, partition, operation.volume_name, "SBNK", ObjectType::sbnk,
                                cancellation);
  if (!banks)
    return std::unexpected{banks.error()};
  if (std::ranges::any_of(*banks, [&](const auto &bank) {
        return bank.id != located->second && bank.name == operation.new_sample_bank_name;
      }))
    return std::unexpected{transaction_error("SBNK rename destination exists")};
  auto bank_payload = current_payload(state, partition, located->second, cancellation);
  if (!bank_payload)
    return std::unexpected{bank_payload.error()};
  auto bank_object = decode_object(*bank_payload);
  if (!bank_object)
    return std::unexpected{bank_object.error()};
  const auto *bank = std::get_if<CurrentSbnk>(&bank_object->payload);
  if (bank == nullptr)
    return std::unexpected{transaction_error("SBNK is unreadable")};
  const auto grouped = (std::to_integer<std::uint8_t>((*bank_payload)[0xd0U]) & 1U) != 0U;

  auto groups = category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac,
                                 cancellation);
  if (!groups)
    return std::unexpected{groups.error()};
  std::set<SfsId> group_references;
  for (const auto &group_row : *groups) {
    const auto *group = std::get_if<CurrentSbac>(&group_row.decoded.payload);
    auto payload = group_row.payload;
    bool changed{};
    for (const auto &slot : group->slots) {
      if (slot.name == operation.new_sample_bank_name)
        return std::unexpected{
            transaction_error("SBAC already references SBNK rename destination")};
      if (slot.name != operation.sample_bank_name)
        continue;
      if (slot.raw_handle != 0U)
        return std::unexpected{transaction_error("SBAC member has unsupported nonzero handle")};
      put_padded_name(payload, slot.offset, operation.new_sample_bank_name);
      changed = true;
    }
    if (changed) {
      if (auto replaced = replace_fixed_object_payload(state, partition, group_row.id,
                                                       std::move(payload), cancellation);
          !replaced)
        return std::unexpected{replaced.error()};
      group_references.insert(group_row.id);
    }
  }
  if (group_references.size() != (grouped ? 1U : 0U))
    return std::unexpected{
        transaction_error("SBNK grouped flag disagrees with exact SBAC membership")};

  auto programs = category_objects(state, partition, operation.volume_name, "PROG",
                                   ObjectType::prog, cancellation);
  if (!programs)
    return std::unexpected{programs.error()};
  std::set<SfsId> program_references;
  std::set<std::uint8_t> direct_numbers;
  for (const auto &program_row : *programs) {
    int number{};
    const auto parsed_number = std::from_chars(
        program_row.name.data(), program_row.name.data() + program_row.name.size(), number);
    if (parsed_number.ec != std::errc{} ||
        parsed_number.ptr != program_row.name.data() + program_row.name.size()) {
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
      if (assignment.name == operation.new_sample_bank_name)
        return std::unexpected{
            transaction_error("Program already assigns SBNK rename destination")};
      if (assignment.name != operation.sample_bank_name)
        continue;
      if (assignment.raw_handle != 0U)
        return std::unexpected{
            transaction_error("Program assignment has unsupported nonzero handle")};
      put_padded_name(payload, 0x120U + index * 0x38U, operation.new_sample_bank_name);
      changed = true;
    }
    if (changed) {
      if (auto replaced = replace_fixed_object_payload(state, partition, program_row.id,
                                                       std::move(payload), cancellation);
          !replaced)
        return std::unexpected{replaced.error()};
      program_references.insert(program_row.id);
      direct_numbers.insert(static_cast<std::uint8_t>(number));
    }
  }
  std::set<std::uint8_t> bitmap_numbers(bank->linked_program_numbers.begin(),
                                        bank->linked_program_numbers.end());
  if (bitmap_numbers != direct_numbers)
    return std::unexpected{
        transaction_error("SBNK Program bitmap disagrees with exact Program assignments")};
  std::set<SfsId> expected = group_references;
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
  if (auto renamed =
          rename_object_payload(state, partition, located->second, operation.sample_bank_name,
                                operation.new_sample_bank_name, cancellation);
      !renamed)
    return std::unexpected{renamed.error()};
  if (auto renamed = rename_directory_entry(state, partition, located->first, located->second,
                                            operation.sample_bank_name,
                                            operation.new_sample_bank_name, cancellation);
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

Result<void> write_bytes(std::fstream &output, std::uint64_t offset,
                         std::span<const std::byte> data) {
  output.seekp(static_cast<std::streamoff>(offset));
  output.write(reinterpret_cast<const char *>(data.data()),
               static_cast<std::streamsize>(data.size()));
  if (!output)
    return std::unexpected{make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                      "could not write alteration output")};
  return {};
}

Result<void> write_continuation_lists(std::fstream &output, const Partition &partition,
                                      const MutablePartition::InsertedRecord &record) {
  constexpr std::size_t extents_per_cluster = (1024U - 12U) / 12U;
  for (std::size_t list_index = 0; list_index < record.continuation_clusters.size(); ++list_index) {
    const auto extent_begin = list_index * extents_per_cluster;
    const auto extent_count = std::min(extents_per_cluster, record.extents.size() - extent_begin);
    std::vector<std::byte> block(1024U);
    put_be32(block, 0U, static_cast<std::uint32_t>(extent_count));
    const auto next = list_index + 1U < record.continuation_clusters.size()
                          ? record.continuation_clusters[list_index + 1U]
                          : 0U;
    put_be32(block, 8U, next);
    for (std::size_t index = 0; index < extent_count; ++index) {
      const auto &extent = record.extents[extent_begin + index];
      const auto offset = 12U + index * 12U;
      put_be32(block, offset, extent.cluster_offset);
      put_be32(block, offset + 4U, extent.cluster_count);
      put_be32(block, offset + 8U, extent.byte_count);
    }
    const auto absolute = (static_cast<std::uint64_t>(partition.start_sector) +
                           static_cast<std::uint64_t>(record.continuation_clusters[list_index]) *
                               partition.sectors_per_cluster) *
                          512U;
    if (auto written = write_bytes(output, absolute, block); !written) {
      return written;
    }
  }
  return {};
}

Result<std::filesystem::path> copy_to_unique_temporary(const std::filesystem::path &source,
                                                       const std::filesystem::path &output) {
  std::random_device random;
  for (std::size_t attempt = 0; attempt < 32U; ++attempt) {
    const auto temporary = output.parent_path() / std::format(".{}.alter.{:08x}.tmp",
                                                              output.filename().string(), random());
    std::error_code error;
    if (std::filesystem::copy_file(source, temporary, error))
      return temporary;
    if (error != std::errc::file_exists)
      return std::unexpected{
          make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                     "could not copy alteration source to its temporary sibling")};
  }
  return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                    "could not reserve a unique alteration temporary sibling")};
}

Result<void> flush_file_to_disk(const std::filesystem::path &path) {
#if defined(_WIN32)
  const auto handle =
      CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "could not open alteration output for durable flush")};
  const auto flushed = FlushFileBuffers(handle) != 0;
  CloseHandle(handle);
#else
  const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0)
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "could not open alteration output for durable flush")};
  const auto flushed = ::fsync(descriptor) == 0;
  ::close(descriptor);
#endif
  if (!flushed)
    return std::unexpected{make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                      "could not flush alteration output to disk")};
  return {};
}

Result<void> publish_temporary(const std::filesystem::path &temporary,
                               const std::filesystem::path &output) {
#if defined(_WIN32)
  if (MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH) == 0)
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "could not atomically publish alteration output")};
#else
  if (::link(temporary.c_str(), output.c_str()) != 0)
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "could not atomically publish alteration output")};
  if (::unlink(temporary.c_str()) != 0)
    return std::unexpected{
        make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                   "alteration output was published but its temporary link could not be removed")};
  const auto parent =
      output.parent_path().empty() ? std::filesystem::path{"."} : output.parent_path();
  const auto descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (descriptor < 0 || ::fsync(descriptor) != 0) {
    if (descriptor >= 0)
      ::close(descriptor);
    return std::unexpected{
        make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                   "alteration output was renamed but its directory could not be synchronized")};
  }
  ::close(descriptor);
#endif
  return {};
}

Result<void> validate_temporary(const std::filesystem::path &temporary,
                                const TransactionState &state,
                                const CancellationToken &cancellation) {
  OpenOptions options;
  options.cancellation = cancellation;
  auto actual = open_image(temporary, options);
  if (!actual)
    return std::unexpected{actual.error()};
  for (const auto &[index, expected] : state.partitions) {
    const auto partition =
        std::ranges::find(actual->partitions(), PartitionIndex{index}, &Partition::index);
    if (partition == actual->partitions().end())
      return std::unexpected{transaction_error("post-write validation lost a planned partition")};
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
      return std::unexpected{
          transaction_error("post-write SFS record set differs from the transaction plan")};

    const auto compare_payload = [&](SfsId id, std::span<const std::byte> payload) -> Result<void> {
      auto written =
          actual->read_record_data(partition->index, id, 64U * 1024U * 1024U, cancellation);
      if (!written)
        return std::unexpected{written.error()};
      if (!std::ranges::equal(*written, payload))
        return std::unexpected{
            transaction_error("post-write object payload differs from the transaction plan")};
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
    const auto bitmap_size = (partition->cluster_count + 7U) / 8U;
    const auto bitmap_offset =
        (static_cast<std::uint64_t>(partition->start_sector) +
         static_cast<std::uint64_t>(partition->bitmap_cluster) * partition->sectors_per_cluster) *
        512U;
    auto bitmap = read_raw(temporary, bitmap_offset, bitmap_size);
    if (!bitmap)
      return std::unexpected{bitmap.error()};
    if (*bitmap != expected.bitmap)
      return std::unexpected{
          transaction_error("post-write allocation bitmap differs from the transaction plan")};
  }
  return {};
}

Result<void> publish(const TransactionState &state, const std::filesystem::path &output_path,
                     const CancellationToken &cancellation) {
  if (std::filesystem::exists(output_path))
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "alteration output already exists")};
  std::error_code error;
  if (!output_path.parent_path().empty())
    std::filesystem::create_directories(output_path.parent_path(), error);
  if (error)
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "could not create alteration output directory")};
  auto temporary_result = copy_to_unique_temporary(state.container.source_path(), output_path);
  if (!temporary_result)
    return std::unexpected{temporary_result.error()};
  const auto temporary = std::move(*temporary_result);
  const auto cleanup = [&]() { std::filesystem::remove(temporary, error); };
  std::fstream output{temporary, std::ios::binary | std::ios::in | std::ios::out};
  if (!output) {
    cleanup();
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "could not open temporary alteration output")};
  }
  for (const auto &[index, item] : state.partitions) {
    static_cast<void>(index);
    if (const auto check = cancellation.check(); !check) {
      output.close();
      cleanup();
      return std::unexpected{check.error()};
    }
    const auto &partition = *item.source;
    for (const auto id : item.deleted) {
      const auto *source_record = record(partition, id);
      if (source_record == nullptr)
        continue;
      std::array<std::byte, 72> zero{};
      if (auto written = write_bytes(output, source_record->record_offset.value, zero); !written) {
        output.close();
        cleanup();
        return written;
      }
    }
    if (item.root_index && item.root_payload) {
      const auto *root = record(partition, SfsId{1});
      if (auto written = write_bytes(output, root->record_offset.value, *item.root_index);
          !written) {
        output.close();
        cleanup();
        return written;
      }
      const auto payload_offset = (static_cast<std::uint64_t>(partition.start_sector) +
                                   static_cast<std::uint64_t>(root->extents[0].cluster_offset) *
                                       partition.sectors_per_cluster) *
                                  512U;
      if (auto written = write_bytes(output, payload_offset, *item.root_payload); !written) {
        output.close();
        cleanup();
        return written;
      }
    }
    const auto index_base = (static_cast<std::uint64_t>(partition.start_sector) +
                             static_cast<std::uint64_t>(partition.directory_index_cluster) *
                                 partition.sectors_per_cluster) *
                            512U;
    for (const auto &[id, changed] : item.changed) {
      const auto *source_record = record(partition, id);
      if (source_record == nullptr) {
        output.close();
        cleanup();
        return std::unexpected{transaction_error("changed record has no source index location")};
      }
      if (auto written = write_bytes(output, source_record->record_offset.value, changed.raw_index);
          !written) {
        output.close();
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
        if (auto written = write_bytes(output, absolute,
                                       std::span{changed.payload}.subspan(payload_offset, count));
            !written) {
          output.close();
          cleanup();
          return written;
        }
        payload_offset += count;
        if (payload_offset == changed.payload.size())
          break;
      }
      if (auto written = write_continuation_lists(output, partition, changed); !written) {
        output.close();
        cleanup();
        return written;
      }
    }
    for (const auto &[id, inserted] : item.inserted) {
      const auto index_offset = index_base + (id.value / 14U) * 1024U + (id.value % 14U) * 72U;
      if (auto written = write_bytes(output, index_offset, inserted.raw_index); !written) {
        output.close();
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
        if (auto written = write_bytes(output, absolute,
                                       std::span{inserted.payload}.subspan(payload_offset, count));
            !written) {
          output.close();
          cleanup();
          return written;
        }
        payload_offset += count;
      }
      if (auto written = write_continuation_lists(output, partition, inserted); !written) {
        output.close();
        cleanup();
        return written;
      }
    }
    const auto bitmap_offset =
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(partition.bitmap_cluster) * partition.sectors_per_cluster) *
        512U;
    if (auto written = write_bytes(output, bitmap_offset, item.bitmap); !written) {
      output.close();
      cleanup();
      return written;
    }
    const auto mirror_offset = static_cast<std::uint64_t>(partition.start_sector) * 512U + 2048U;
    if (auto written = write_bytes(
            output, mirror_offset,
            std::span{item.bitmap}.first(std::min<std::size_t>(512, item.bitmap.size())));
        !written) {
      output.close();
      cleanup();
      return written;
    }
  }
  output.flush();
  output.close();
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
  if (auto published = publish_temporary(temporary, output_path); !published) {
    cleanup();
    return std::unexpected{published.error()};
  }
  return {};
}

} // namespace

std::string_view operation_type_name(const AlterationOperationData &operation) noexcept {
  constexpr std::array names{
      std::string_view{"delete_volume"},   std::string_view{"insert_volume"},
      std::string_view{"delete_sbnk"},     std::string_view{"insert_sbnk"},
      std::string_view{"insert_waveform"}, std::string_view{"delete_waveform"},
      std::string_view{"rename_waveform"}, std::string_view{"rename_sbnk"},
      std::string_view{"delete_sbac"},     std::string_view{"insert_sbac"},
      std::string_view{"rename_sbac"},     std::string_view{"delete_program"},
      std::string_view{"insert_program"},
  };
  return names[operation.index()];
}

Result<AlterationManifest> parse_alteration_manifest(std::string_view json,
                                                     const std::filesystem::path &base_directory) {
  try {
    const auto root = Json::parse(json);
    if (auto valid = exact_fields(root, {"schema_version", "operations"}, "manifest"); !valid)
      return std::unexpected{valid.error()};
    auto version = required_text(root, "schema_version", "manifest");
    if (!version)
      return std::unexpected{version.error()};
    if (*version != "1.0")
      return std::unexpected{transaction_error("manifest schema version must be 1.0")};
    if (!root["operations"].is_array() || root["operations"].empty())
      return std::unexpected{transaction_error("manifest.operations must be a non-empty array")};
    AlterationManifest result{*version, {}};
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
          *type != "rename_sbac") {
        return std::unexpected{transaction_error(
            "operation type is not implemented by the native transaction engine")};
      }
      PartitionSelector selector;
      if (row["partition_index"].is_number_integer()) {
        const auto value = row["partition_index"].get<int>();
        if (value < 0 || value > 7)
          return std::unexpected{transaction_error("partition index must be 0..7")};
        selector = PartitionIndex{static_cast<std::uint8_t>(value)};
      } else if (row["partition_index"].is_object() && row["partition_index"].size() == 1U &&
                 row["partition_index"].contains("operation_ref")) {
        auto reference =
            required_text(row["partition_index"], "operation_ref", context + ".partition_index");
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
        if (auto valid =
                exact_fields(row, {"id", "type", "partition_index", "volume_name"}, context);
            !valid)
          return std::unexpected{valid.error()};
        auto volume = required_text(row, "volume_name", context);
        if (!volume)
          return std::unexpected{volume.error()};
        operation.volume_name = *volume;
      } else if (*type == "insert_volume") {
        if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume"}, context);
            !valid)
          return std::unexpected{valid.error()};
        Json wrapper{
            {"schema_version", "1.0"},
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
        if (auto valid = exact_fields(
                row, {"id", "type", "partition_index", "volume_name", "sample_bank_name"}, context);
            !valid) {
          return std::unexpected{valid.error()};
        }
        auto volume = required_text(row, "volume_name", context);
        auto bank = object_name(row, "sample_bank_name", context);
        if (!volume)
          return std::unexpected{volume.error()};
        if (!bank)
          return std::unexpected{bank.error()};
        operation.volume_name = std::move(*volume);
        operation.sample_bank_name = std::move(*bank);
      } else if (*type == "insert_sbnk") {
        if (auto valid = exact_fields(
                row, {"id", "type", "partition_index", "volume_name", "sample_bank"}, context);
            !valid) {
          return std::unexpected{valid.error()};
        }
        auto volume = required_text(row, "volume_name", context);
        if (!volume)
          return std::unexpected{volume.error()};
        const auto &bank = row["sample_bank"];
        if (!bank.is_object()) {
          return std::unexpected{transaction_error(context + ".sample_bank must be an object")};
        }
        const std::set<std::string> required{"name", "waveform_name", "root_key", "key_low",
                                             "key_high"};
        const std::set<std::string> optional{"right_waveform_name", "level"};
        for (const auto &field : required) {
          if (!bank.contains(field)) {
            return std::unexpected{
                transaction_error(context + ".sample_bank is missing field " + field)};
          }
        }
        for (const auto &[field, unused] : bank.items()) {
          static_cast<void>(unused);
          if (!required.contains(field) && !optional.contains(field)) {
            return std::unexpected{
                transaction_error(context + ".sample_bank has unknown field " + field)};
          }
        }
        const auto bank_context = context + ".sample_bank";
        auto name = object_name(bank, "name", bank_context);
        auto waveform = object_name(bank, "waveform_name", bank_context);
        auto root_key = midi_value(bank, "root_key", bank_context, 0U, true);
        auto key_low = midi_value(bank, "key_low", bank_context, 0U, true);
        auto key_high = midi_value(bank, "key_high", bank_context, 0U, true);
        auto level = midi_value(bank, "level", bank_context, 100U, false);
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
          return std::unexpected{
              transaction_error(bank_context + ".key_high must not be below key_low")};
        }
        SampleBankSpec spec;
        spec.name = std::move(*name);
        spec.waveform_id = std::move(*waveform);
        if (bank.contains("right_waveform_name")) {
          auto right = object_name(bank, "right_waveform_name", bank_context);
          if (!right)
            return std::unexpected{right.error()};
          spec.right_waveform_id = std::move(*right);
        }
        spec.root_key = *root_key;
        spec.key_low = *key_low;
        spec.key_high = *key_high;
        spec.level = *level;
        operation.volume_name = std::move(*volume);
        operation.sample_bank_name = spec.name;
        operation.sample_bank = std::move(spec);
      } else if (*type == "rename_waveform") {
        if (auto valid = exact_fields(row,
                                      {"id", "type", "partition_index", "volume_name",
                                       "waveform_name", "new_waveform_name"},
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
        if (auto valid = exact_fields(row,
                                      {"id", "type", "partition_index", "volume_name",
                                       "sample_bank_name", "new_sample_bank_name"},
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
      } else if (*type == "rename_sbac") {
        if (auto valid = exact_fields(row,
                                      {"id", "type", "partition_index", "volume_name",
                                       "sample_bank_group_name", "new_sample_bank_group_name"},
                                      context);
            !valid)
          return std::unexpected{valid.error()};
        auto volume = required_text(row, "volume_name", context);
        auto old_name = object_name(row, "sample_bank_group_name", context);
        auto new_name = object_name(row, "new_sample_bank_group_name", context);
        if (!volume)
          return std::unexpected{volume.error()};
        if (!old_name)
          return std::unexpected{old_name.error()};
        if (!new_name)
          return std::unexpected{new_name.error()};
        if (*old_name == *new_name)
          return std::unexpected{transaction_error("new_sample_bank_group_name must differ")};
        operation.volume_name = std::move(*volume);
        operation.sample_bank_group_name = std::move(*old_name);
        operation.new_sample_bank_group_name = std::move(*new_name);
      } else if (*type == "delete_program") {
        if (auto valid = exact_fields(
                row, {"id", "type", "partition_index", "volume_name", "program_number"}, context);
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
        if (auto valid = exact_fields(
                row, {"id", "type", "partition_index", "volume_name", "sample_bank_group_name"},
                context);
            !valid)
          return std::unexpected{valid.error()};
        auto volume = required_text(row, "volume_name", context);
        auto group = object_name(row, "sample_bank_group_name", context);
        if (!volume)
          return std::unexpected{volume.error()};
        if (!group)
          return std::unexpected{group.error()};
        operation.volume_name = std::move(*volume);
        operation.sample_bank_group_name = std::move(*group);
      } else if (*type == "insert_sbac") {
        if (auto valid = exact_fields(
                row, {"id", "type", "partition_index", "volume_name", "sample_bank_group"},
                context);
            !valid)
          return std::unexpected{valid.error()};
        auto volume = required_text(row, "volume_name", context);
        if (!volume)
          return std::unexpected{volume.error()};
        const auto &group = row["sample_bank_group"];
        if (auto valid = exact_fields(group, {"name", "member_sample_banks"},
                                      context + ".sample_bank_group");
            !valid)
          return std::unexpected{valid.error()};
        auto name = object_name(group, "name", context + ".sample_bank_group");
        if (!name)
          return std::unexpected{name.error()};
        if (!group["member_sample_banks"].is_array() || group["member_sample_banks"].empty() ||
            group["member_sample_banks"].size() > 3U)
          return std::unexpected{transaction_error("member_sample_banks must contain 1..3 names")};
        SampleBankGroupSpec spec;
        spec.name = *name;
        for (std::size_t member_index = 0; member_index < group["member_sample_banks"].size();
             ++member_index) {
          Json wrapper{{"name", group["member_sample_banks"][member_index]}};
          auto member =
              object_name(wrapper, "name", context + ".sample_bank_group.member_sample_banks");
          if (!member)
            return std::unexpected{member.error()};
          if (std::ranges::contains(spec.member_sample_banks, *member))
            return std::unexpected{transaction_error("member_sample_banks must be distinct")};
          spec.member_sample_banks.push_back(std::move(*member));
        }
        operation.volume_name = std::move(*volume);
        operation.sample_bank_group_name = spec.name;
        operation.sample_bank_group = std::move(spec);
      } else if (*type == "insert_program") {
        if (auto valid = exact_fields(
                row, {"id", "type", "partition_index", "volume_name", "program"}, context);
            !valid)
          return std::unexpected{valid.error()};
        auto volume = required_text(row, "volume_name", context);
        if (!volume)
          return std::unexpected{volume.error()};
        const auto &program = row["program"];
        if (auto valid = exact_fields(program, {"number", "assignments"}, context + ".program");
            !valid)
          return std::unexpected{valid.error()};
        auto number = program_value(program, "number", context + ".program");
        if (!number)
          return std::unexpected{number.error()};
        if (!program["assignments"].is_array() || program["assignments"].size() != 2U)
          return std::unexpected{transaction_error("Program requires exactly two assignments")};
        ProgramSpec spec;
        spec.number = *number;
        constexpr std::array<std::string_view, 2> target_fields{"sample_bank_group", "sample_bank"};
        for (std::size_t assignment_index = 0; assignment_index < 2U; ++assignment_index) {
          const auto &assignment = program["assignments"][assignment_index];
          const auto field = target_fields[assignment_index];
          if (auto valid = exact_fields(assignment, {field, "receive_channel"},
                                        context + ".program.assignments");
              !valid)
            return std::unexpected{valid.error()};
          auto target = object_name(assignment, field, context + ".program.assignments");
          if (!target)
            return std::unexpected{target.error()};
          auto channel =
              midi_value(assignment, "receive_channel", context + ".program.assignments", 0U, true);
          const auto expected_channel = static_cast<std::uint8_t>(assignment_index + 1U);
          if (!channel || *channel != expected_channel)
            return std::unexpected{transaction_error(
                "Program assignments must be SBAC/channel 1 then SBNK/channel 2")};
          spec.assignments.push_back(
              {assignment_index == 0U ? "SBAC" : "SBNK", std::move(*target), *channel});
        }
        operation.volume_name = std::move(*volume);
        operation.program_number = spec.number;
        operation.program = std::move(spec);
      } else if (*type == "delete_waveform") {
        if (auto valid = exact_fields(
                row, {"id", "type", "partition_index", "volume_name", "waveform_name"}, context);
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
        if (auto valid = exact_fields(
                row, {"id", "type", "partition_index", "volume_name", "audio"}, context);
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
            return std::unexpected{
                transaction_error(context + ".audio has unknown field " + field)};
          }
        }
        if (!audio["waveform_names"].is_array() ||
            (audio["waveform_names"].size() != 1U && audio["waveform_names"].size() != 2U)) {
          return std::unexpected{
              transaction_error(context + ".audio.waveform_names must contain one or two names")};
        }
        InsertWaveformSpec spec;
        for (std::size_t name_index = 0; name_index < audio["waveform_names"].size();
             ++name_index) {
          Json wrapper{{"name", audio["waveform_names"][name_index]}};
          auto name =
              object_name(wrapper, "name",
                          context + ".audio.waveform_names[" + std::to_string(name_index) + "]");
          if (!name)
            return std::unexpected{name.error()};
          if (std::ranges::contains(spec.waveform_names, *name)) {
            return std::unexpected{
                transaction_error(context + ".audio.waveform_names must be distinct")};
          }
          spec.waveform_names.push_back(std::move(*name));
        }
        auto path = required_text(audio, "path", context + ".audio");
        auto root_key = midi_value(audio, "root_key", context + ".audio", 0U, true);
        if (!path)
          return std::unexpected{path.error()};
        if (!root_key)
          return std::unexpected{root_key.error()};
        spec.path = std::filesystem::path{*path};
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
    return std::unexpected{
        transaction_error(std::string{"invalid alteration JSON: "} + error.what())};
  }
}

Result<AlterationManifest> load_alteration_manifest(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  if (!input)
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "could not open alteration manifest")};
  std::ostringstream text;
  text << input.rdbuf();
  return parse_alteration_manifest(text.str(), path.parent_path());
}

Result<AlterationResult> alter_hds(const std::filesystem::path &source_path,
                                   const AlterationManifest &manifest,
                                   const std::optional<std::filesystem::path> &output_path,
                                   const CancellationToken &cancellation, ProgressSink *progress) {
  if (output_path && std::filesystem::absolute(source_path).lexically_normal() ==
                         std::filesystem::absolute(*output_path).lexically_normal()) {
    return std::unexpected{transaction_error("alteration output must differ from source")};
  }
  OpenOptions options;
  options.cancellation = cancellation;
  options.progress = progress;
  auto container = open_image(source_path, options);
  if (!container)
    return std::unexpected{container.error()};
  auto catalog = build_object_catalog(*container, 64U * 1024U * 1024U, cancellation);
  if (!catalog)
    return std::unexpected{catalog.error()};
  auto graph = build_relationship_graph(*catalog);
  TransactionState state{std::move(*container), std::move(*catalog), std::move(graph), {}, {}, {}};
  std::map<std::string, std::pair<PartitionIndex, SfsId>> object_ids;
  for (const auto &object : state.catalog.objects) {
    object_ids.emplace(object.key, std::pair{object.partition, object.sfs_id});
  }
  for (const auto &relationship : state.graph.relationships) {
    if (relationship.quality != RelationshipQuality::known || !relationship.target_key) {
      continue;
    }
    const auto source = object_ids.find(relationship.source_key);
    const auto target = object_ids.find(*relationship.target_key);
    if (source != object_ids.end() && target != object_ids.end() &&
        source->second.first == target->second.first) {
      state.known_edges.emplace_back(source->second.first, source->second.second,
                                     target->second.second);
    }
  }
  for (const auto &partition : state.container.partitions()) {
    if (!partition.allocation.reconstructed_not_stored.empty() ||
        partition.allocation.extent_total_mismatch_count != 0U)
      return std::unexpected{
          transaction_error("source allocation cannot safely support alteration")};
    const auto bitmap_size = (partition.cluster_count + 7U) / 8U;
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
  if (progress)
    progress->report(
        {ProgressPhase::writing, 0U, manifest.operations.size(), "planning alteration", {}});
  using OperationHandler = Result<OperationReport> (*)(TransactionState &, const OperationView &,
                                                       const CancellationToken &);
  constexpr std::array<OperationHandler, std::variant_size_v<AlterationOperationData>> handlers{
      delete_volume,   insert_volume,   delete_sbnk,    insert_sbnk, insert_waveform,
      delete_waveform, rename_waveform, rename_sbnk,    delete_sbac, insert_sbac,
      rename_sbac,     delete_program,  insert_program,
  };
  for (std::size_t operation_index = 0; operation_index < manifest.operations.size();
       ++operation_index) {
    const auto &typed_operation = manifest.operations[operation_index];
    const auto operation = operation_view(typed_operation);
    auto report = handlers[typed_operation.data.index()](state, operation, cancellation);
    if (!report)
      return std::unexpected{report.error()};
    state.reports.push_back(std::move(*report));
    if (progress)
      progress->report(
          {ProgressPhase::writing, operation_index + 1U, manifest.operations.size(), operation.type,
           output_path ? std::optional{output_path->string()} : std::optional<std::string>{}});
  }
  if (output_path) {
    auto applied = publish(state, *output_path, cancellation);
    if (!applied)
      return std::unexpected{applied.error()};
  }
  return AlterationResult{source_path, output_path, output_path.has_value(),
                          std::move(state.reports)};
}

Result<TransactionPlan> plan_hds_alteration(const std::filesystem::path &source_path,
                                            const AlterationManifest &manifest,
                                            const CancellationToken &cancellation,
                                            ProgressSink *progress) {
  auto result = alter_hds(source_path, manifest, {}, cancellation, progress);
  if (!result)
    return std::unexpected{result.error()};
  return TransactionPlan{result->source_path, std::move(result->operations)};
}

} // namespace axk
