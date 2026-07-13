#include "axklib/writer.hpp"

#include "axklib/utf8.hpp"

#include <fstream>
#include <limits>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace axk {
namespace {

using Json = nlohmann::json;

Error manifest_error(std::string message) {
  return make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, std::move(message));
}

Result<void> fields(const Json &value, std::string_view context,
                    std::initializer_list<std::string_view> required,
                    std::initializer_list<std::string_view> optional = {}) {
  if (!value.is_object()) {
    return std::unexpected{manifest_error(std::string{context} + " must be a JSON object")};
  }
  std::set<std::string_view> allowed{required};
  allowed.insert(optional);
  for (const auto name : required) {
    if (!value.contains(name)) {
      return std::unexpected{manifest_error(std::string{context} +
                                            " is missing required field: " + std::string{name})};
    }
  }
  for (const auto &[name, ignored] : value.items()) {
    static_cast<void>(ignored);
    if (!allowed.contains(name)) {
      return std::unexpected{manifest_error(std::string{context} + " has unknown field: " + name)};
    }
  }
  return {};
}

Result<std::string> text(const Json &value, std::string_view context) {
  if (!value.is_string() || value.get_ref<const std::string &>().empty()) {
    return std::unexpected{manifest_error(std::string{context} + " must be a non-empty string")};
  }
  return value.get<std::string>();
}

Result<std::uint64_t> integer(const Json &value, std::string_view context, std::uint64_t minimum,
                              std::uint64_t maximum) {
  if (!value.is_number_unsigned() && !value.is_number_integer()) {
    return std::unexpected{manifest_error(std::string{context} + " must be an integer")};
  }
  if (value.is_number_integer() && value.get<std::int64_t>() < 0) {
    return std::unexpected{
        manifest_error(std::string{context} + " is outside its supported range")};
  }
  const auto result = value.get<std::uint64_t>();
  if (result < minimum || result > maximum) {
    return std::unexpected{
        manifest_error(std::string{context} + " is outside its supported range")};
  }
  return result;
}

Result<std::filesystem::path> path(const Json &value, std::string_view context,
                                   const std::filesystem::path &base) {
  auto parsed = text(value, context);
  if (!parsed)
    return std::unexpected{parsed.error()};
  auto result = axk::text::path_from_utf8(*parsed);
  if (!result)
    return std::unexpected{manifest_error(std::string{context} + " must be a valid UTF-8 path")};
  if (result->is_relative())
    *result = base / *result;
  return result->lexically_normal();
}

Result<WaveformSpec> waveform(const Json &value, std::string context,
                              const std::filesystem::path &base) {
  if (auto valid =
          fields(value, context, {"id", "name", "path", "root_key"}, {"target_sample_rate"});
      !valid)
    return std::unexpected{valid.error()};
  auto id = text(value["id"], context + ".id");
  auto name = text(value["name"], context + ".name");
  auto source = path(value["path"], context + ".path", base);
  auto root = integer(value["root_key"], context + ".root_key", 0, 127);
  if (!id)
    return std::unexpected{id.error()};
  if (!name)
    return std::unexpected{name.error()};
  if (!source)
    return std::unexpected{source.error()};
  if (!root)
    return std::unexpected{root.error()};
  WaveformSpec result{*id, *name, *source, static_cast<std::uint8_t>(*root), {}};
  if (value.contains("target_sample_rate")) {
    auto rate = integer(value["target_sample_rate"], context + ".target_sample_rate", 1,
                        std::numeric_limits<std::uint32_t>::max());
    if (!rate)
      return std::unexpected{rate.error()};
    result.target_sample_rate = static_cast<std::uint32_t>(*rate);
  }
  return result;
}

Result<SampleBankSpec> sample_bank(const Json &value, std::string context,
                                   const std::filesystem::path &base) {
  if (auto valid = fields(value, context, {"name", "root_key", "key_low", "key_high"},
                          {"level", "waveform_id", "right_waveform_id", "interleaved_audio_path",
                           "left_waveform_name", "right_waveform_name", "target_sample_rate"});
      !valid) {
    return std::unexpected{valid.error()};
  }
  const bool direct = value.contains("waveform_id");
  const bool interleaved = value.contains("interleaved_audio_path");
  if (direct == interleaved || (interleaved && value.contains("right_waveform_id")) ||
      (direct && (value.contains("left_waveform_name") || value.contains("right_waveform_name") ||
                  value.contains("target_sample_rate")))) {
    return std::unexpected{
        manifest_error(context + " has an invalid audio source field combination")};
  }
  auto name = text(value["name"], context + ".name");
  auto root = integer(value["root_key"], context + ".root_key", 0, 127);
  auto low = integer(value["key_low"], context + ".key_low", 0, 127);
  auto high = integer(value["key_high"], context + ".key_high", 0, 127);
  if (!name)
    return std::unexpected{name.error()};
  if (!root)
    return std::unexpected{root.error()};
  if (!low)
    return std::unexpected{low.error()};
  if (!high)
    return std::unexpected{high.error()};
  if (*high < *low)
    return std::unexpected{manifest_error(context + ".key_high precedes key_low")};
  SampleBankSpec result;
  result.name = *name;
  result.root_key = static_cast<std::uint8_t>(*root);
  result.key_low = static_cast<std::uint8_t>(*low);
  result.key_high = static_cast<std::uint8_t>(*high);
  if (value.contains("level")) {
    auto level = integer(value["level"], context + ".level", 0, 127);
    if (!level)
      return std::unexpected{level.error()};
    result.level = static_cast<std::uint8_t>(*level);
  }
  const auto optional_text = [&](std::string_view field) -> Result<std::optional<std::string>> {
    if (!value.contains(field))
      return std::optional<std::string>{};
    auto parsed = text(value[field], context + "." + std::string{field});
    if (!parsed)
      return std::unexpected{parsed.error()};
    return std::optional<std::string>{*parsed};
  };
  auto left_id = optional_text("waveform_id");
  auto right_id = optional_text("right_waveform_id");
  auto left_name = optional_text("left_waveform_name");
  auto right_name = optional_text("right_waveform_name");
  if (!left_id)
    return std::unexpected{left_id.error()};
  if (!right_id)
    return std::unexpected{right_id.error()};
  if (!left_name)
    return std::unexpected{left_name.error()};
  if (!right_name)
    return std::unexpected{right_name.error()};
  result.waveform_id = *left_id;
  result.right_waveform_id = *right_id;
  result.left_waveform_name = *left_name;
  result.right_waveform_name = *right_name;
  if (interleaved) {
    auto source = path(value["interleaved_audio_path"], context + ".interleaved_audio_path", base);
    if (!source)
      return std::unexpected{source.error()};
    result.interleaved_audio_path = *source;
  }
  if (value.contains("target_sample_rate")) {
    auto rate = integer(value["target_sample_rate"], context + ".target_sample_rate", 1,
                        std::numeric_limits<std::uint32_t>::max());
    if (!rate)
      return std::unexpected{rate.error()};
    result.target_sample_rate = static_cast<std::uint32_t>(*rate);
  }
  return result;
}

Result<VolumeSpec> volume(const Json &value, std::string context,
                          const std::filesystem::path &base) {
  if (auto valid = fields(value, context, {"name", "waveforms", "sample_banks"},
                          {"sample_bank_groups", "programs"});
      !valid) {
    return std::unexpected{valid.error()};
  }
  if (!value["waveforms"].is_array() || !value["sample_banks"].is_array()) {
    return std::unexpected{
        manifest_error(context + " waveform and sample-bank fields must be arrays")};
  }
  auto name = text(value["name"], context + ".name");
  if (!name)
    return std::unexpected{name.error()};
  VolumeSpec result;
  result.name = *name;
  std::set<std::string> waveform_ids;
  for (std::size_t index = 0; index < value["waveforms"].size(); ++index) {
    auto item = waveform(value["waveforms"][index],
                         context + ".waveforms[" + std::to_string(index) + "]", base);
    if (!item)
      return std::unexpected{item.error()};
    if (!waveform_ids.insert(item->id).second)
      return std::unexpected{manifest_error(context + " has duplicate waveform ids")};
    result.waveforms.push_back(std::move(*item));
  }
  std::set<std::string> bank_names;
  for (std::size_t index = 0; index < value["sample_banks"].size(); ++index) {
    auto item = sample_bank(value["sample_banks"][index],
                            context + ".sample_banks[" + std::to_string(index) + "]", base);
    if (!item)
      return std::unexpected{item.error()};
    if (!bank_names.insert(item->name).second)
      return std::unexpected{manifest_error(context + " has duplicate sample-bank names")};
    if (item->waveform_id && !waveform_ids.contains(*item->waveform_id))
      return std::unexpected{manifest_error(context + " references an unknown waveform")};
    if (item->right_waveform_id && (!waveform_ids.contains(*item->right_waveform_id) ||
                                    item->right_waveform_id == item->waveform_id))
      return std::unexpected{manifest_error(context + " has an invalid right waveform reference")};
    result.sample_banks.push_back(std::move(*item));
  }
  const auto &groups =
      value.contains("sample_bank_groups") ? value["sample_bank_groups"] : Json::array();
  if (!groups.is_array()) {
    return std::unexpected{manifest_error(context + ".sample_bank_groups must be an array")};
  }
  std::set<std::string> group_names;
  for (std::size_t index = 0; index < groups.size(); ++index) {
    const auto group_context = context + ".sample_bank_groups[" + std::to_string(index) + "]";
    const auto &row = groups[index];
    if (auto valid =
            fields(row, group_context, {"name"}, {"member_sample_bank", "member_sample_banks"});
        !valid) {
      return std::unexpected{valid.error()};
    }
    const bool singular = row.contains("member_sample_bank");
    const bool plural = row.contains("member_sample_banks");
    if (singular == plural) {
      return std::unexpected{
          manifest_error(group_context + " must contain exactly one member field")};
    }
    auto group_name = text(row["name"], group_context + ".name");
    if (!group_name)
      return std::unexpected{group_name.error()};
    if (!group_names.insert(*group_name).second) {
      return std::unexpected{manifest_error(context + " has duplicate sample-bank group names")};
    }
    SampleBankGroupSpec group{*group_name, {}};
    if (singular) {
      auto member = text(row["member_sample_bank"], group_context + ".member_sample_bank");
      if (!member)
        return std::unexpected{member.error()};
      group.member_sample_banks.push_back(*member);
    } else {
      if (!row["member_sample_banks"].is_array()) {
        return std::unexpected{
            manifest_error(group_context + ".member_sample_banks must be an array")};
      }
      for (std::size_t member_index = 0; member_index < row["member_sample_banks"].size();
           ++member_index) {
        auto member =
            text(row["member_sample_banks"][member_index],
                 group_context + ".member_sample_banks[" + std::to_string(member_index) + "]");
        if (!member)
          return std::unexpected{member.error()};
        group.member_sample_banks.push_back(*member);
      }
    }
    const std::set<std::string> unique_members{group.member_sample_banks.begin(),
                                               group.member_sample_banks.end()};
    if (group.member_sample_banks.empty() || group.member_sample_banks.size() > 3U ||
        unique_members.size() != group.member_sample_banks.size()) {
      return std::unexpected{manifest_error(group_context + " must contain 1..3 distinct members")};
    }
    for (const auto &member : group.member_sample_banks) {
      if (!bank_names.contains(member)) {
        return std::unexpected{
            manifest_error(group_context + " references an unknown sample bank")};
      }
    }
    result.sample_bank_groups.push_back(std::move(group));
  }
  const auto &programs = value.contains("programs") ? value["programs"] : Json::array();
  if (!programs.is_array()) {
    return std::unexpected{manifest_error(context + ".programs must be an array")};
  }
  std::set<std::uint64_t> program_numbers;
  for (std::size_t index = 0; index < programs.size(); ++index) {
    const auto program_context = context + ".programs[" + std::to_string(index) + "]";
    const auto &row = programs[index];
    if (auto valid = fields(row, program_context, {"number", "assignments"}); !valid) {
      return std::unexpected{valid.error()};
    }
    auto number = integer(row["number"], program_context + ".number", 1, 128);
    if (!number)
      return std::unexpected{number.error()};
    if (!program_numbers.insert(*number).second) {
      return std::unexpected{manifest_error(context + " has duplicate Program numbers")};
    }
    if (!row["assignments"].is_array()) {
      return std::unexpected{manifest_error(program_context + ".assignments must be an array")};
    }
    ProgramSpec program{static_cast<std::uint8_t>(*number), {}};
    for (std::size_t assignment_index = 0; assignment_index < row["assignments"].size();
         ++assignment_index) {
      const auto assignment_context =
          program_context + ".assignments[" + std::to_string(assignment_index) + "]";
      const auto &assignment = row["assignments"][assignment_index];
      if (auto valid = fields(assignment, assignment_context, {"receive_channel"},
                              {"sample_bank", "sample_bank_group"});
          !valid) {
        return std::unexpected{valid.error()};
      }
      const bool bank = assignment.contains("sample_bank");
      const bool group = assignment.contains("sample_bank_group");
      if (bank == group)
        return std::unexpected{
            manifest_error(assignment_context + " must contain exactly one target")};
      const auto target_field = bank ? "sample_bank" : "sample_bank_group";
      auto target = text(assignment[target_field], assignment_context + "." + target_field);
      auto channel =
          integer(assignment["receive_channel"], assignment_context + ".receive_channel", 1, 16);
      if (!target)
        return std::unexpected{target.error()};
      if (!channel)
        return std::unexpected{channel.error()};
      if ((bank && !bank_names.contains(*target)) || (group && !group_names.contains(*target))) {
        return std::unexpected{
            manifest_error(assignment_context + " references an unknown target")};
      }
      program.assignments.push_back(
          {bank ? "SBNK" : "SBAC", *target, static_cast<std::uint8_t>(*channel)});
    }
    result.programs.push_back(std::move(program));
  }
  return result;
}

Result<HdsBuildManifest> parse(const Json &root, const std::filesystem::path &base) {
  if (auto valid = fields(root, "manifest", {"schema_version", "size_bytes", "partitions"}); !valid)
    return std::unexpected{valid.error()};
  auto version = text(root["schema_version"], "manifest.schema_version");
  if (!version)
    return std::unexpected{version.error()};
  if (*version != "1.0")
    return std::unexpected{manifest_error("manifest.schema_version must be '1.0'")};
  auto size =
      integer(root["size_bytes"], "manifest.size_bytes", minimum_hds_size, maximum_hds_size);
  if (!size)
    return std::unexpected{size.error()};
  if (*size % 512U != 0U)
    return std::unexpected{manifest_error("manifest.size_bytes must be a multiple of 512")};
  if (!root["partitions"].is_array() || root["partitions"].empty() ||
      root["partitions"].size() > 8U)
    return std::unexpected{manifest_error("manifest.partitions must contain 1..8 partitions")};
  HdsBuildManifest result{*version, *size, {}};
  for (std::size_t index = 0; index < root["partitions"].size(); ++index) {
    const auto &row = root["partitions"][index];
    const auto context = "manifest.partitions[" + std::to_string(index) + "]";
    if (auto valid = fields(row, context, {"name", "volumes"}); !valid)
      return std::unexpected{valid.error()};
    auto name = text(row["name"], context + ".name");
    if (!name)
      return std::unexpected{name.error()};
    if (!row["volumes"].is_array() || row["volumes"].empty())
      return std::unexpected{manifest_error(context + ".volumes must be a non-empty array")};
    PartitionSpec partition{*name, {}};
    for (std::size_t volume_index = 0; volume_index < row["volumes"].size(); ++volume_index) {
      auto parsed = volume(row["volumes"][volume_index],
                           context + ".volumes[" + std::to_string(volume_index) + "]", base);
      if (!parsed)
        return std::unexpected{parsed.error()};
      partition.volumes.push_back(std::move(*parsed));
    }
    result.partitions.push_back(std::move(partition));
  }
  return result;
}

Result<MediaBuildManifest> parse_media(const Json &root, const std::filesystem::path &base) {
  if (auto valid = fields(root, "manifest", {"schema_version", "format"},
                          {"transfer", "authored_volume", "iso"});
      !valid) {
    return std::unexpected{valid.error()};
  }
  auto version = text(root["schema_version"], "manifest.schema_version");
  auto format = text(root["format"], "manifest.format");
  if (!version)
    return std::unexpected{version.error()};
  if (!format)
    return std::unexpected{format.error()};
  if (*version != "1.0")
    return std::unexpected{manifest_error("manifest.schema_version must be '1.0'")};
  const bool transfer = root.contains("transfer");
  const bool authored = root.contains("authored_volume");
  if (transfer == authored) {
    return std::unexpected{
        manifest_error("manifest must contain exactly one of transfer or authored_volume")};
  }
  MediaBuildManifest result;
  result.schema_version = *version;
  if (*format == "fat12_floppy") {
    result.format = MediaImageFormat::fat12_floppy;
  } else if (*format == "iso9660") {
    result.format = MediaImageFormat::iso9660;
  } else {
    return std::unexpected{manifest_error("manifest.format must be 'fat12_floppy' or 'iso9660'")};
  }
  if (transfer) {
    const auto &row = root["transfer"];
    if (auto valid =
            fields(row, "manifest.transfer", {"source_path"}, {"selection", "root_object_keys"});
        !valid) {
      return std::unexpected{valid.error()};
    }
    auto source = path(row["source_path"], "manifest.transfer.source_path", base);
    if (!source)
      return std::unexpected{source.error()};
    SavedObjectSelection selection = SavedObjectSelection::roots;
    if (row.contains("selection")) {
      auto value = text(row["selection"], "manifest.transfer.selection");
      if (!value)
        return std::unexpected{value.error()};
      if (*value == "all") {
        selection = SavedObjectSelection::all;
      } else if (*value != "roots") {
        return std::unexpected{
            manifest_error("manifest.transfer.selection must be 'roots' or 'all'")};
      }
    }
    const bool has_roots = row.contains("root_object_keys");
    if (selection == SavedObjectSelection::all && has_roots) {
      return std::unexpected{manifest_error(
          "manifest.transfer.root_object_keys must be omitted when selection is 'all'")};
    }
    if (selection == SavedObjectSelection::roots &&
        (!has_roots || !row["root_object_keys"].is_array() || row["root_object_keys"].empty())) {
      return std::unexpected{manifest_error(
          "manifest.transfer.root_object_keys must be a non-empty array for root selection")};
    }
    SavedObjectTransferSpec spec{*source, {}, selection};
    if (selection == SavedObjectSelection::roots) {
      std::set<std::string> keys;
      for (std::size_t index = 0; index < row["root_object_keys"].size(); ++index) {
        auto key = text(row["root_object_keys"][index],
                        "manifest.transfer.root_object_keys[" + std::to_string(index) + "]");
        if (!key)
          return std::unexpected{key.error()};
        if (!keys.insert(*key).second)
          return std::unexpected{
              manifest_error("manifest.transfer.root_object_keys contains a duplicate")};
        spec.root_object_keys.push_back(std::move(*key));
      }
    }
    result.transfer = std::move(spec);
  } else {
    auto parsed = volume(root["authored_volume"], "manifest.authored_volume", base);
    if (!parsed)
      return std::unexpected{parsed.error()};
    result.authored_volume = std::move(*parsed);
  }
  if (root.contains("iso")) {
    const auto &iso = root["iso"];
    if (auto valid = fields(iso, "manifest.iso",
                            {"volume_id", "raw_group", "group_name", "raw_volume", "volume_name"});
        !valid) {
      return std::unexpected{valid.error()};
    }
    auto volume_id = text(iso["volume_id"], "manifest.iso.volume_id");
    auto raw_group = text(iso["raw_group"], "manifest.iso.raw_group");
    auto group_name = text(iso["group_name"], "manifest.iso.group_name");
    auto raw_volume = text(iso["raw_volume"], "manifest.iso.raw_volume");
    auto volume_name = text(iso["volume_name"], "manifest.iso.volume_name");
    if (!volume_id)
      return std::unexpected{volume_id.error()};
    if (!raw_group)
      return std::unexpected{raw_group.error()};
    if (!group_name)
      return std::unexpected{group_name.error()};
    if (!raw_volume)
      return std::unexpected{raw_volume.error()};
    if (!volume_name)
      return std::unexpected{volume_name.error()};
    result.iso_volume_id = std::move(*volume_id);
    result.raw_group = std::move(*raw_group);
    result.group_name = std::move(*group_name);
    result.raw_volume = std::move(*raw_volume);
    result.volume_name = std::move(*volume_name);
  } else if (result.format == MediaImageFormat::iso9660) {
    return std::unexpected{manifest_error("manifest.iso is required for ISO9660 output")};
  }
  return result;
}

} // namespace

Result<HdsBuildManifest> parse_hds_build_manifest(std::string_view json,
                                                  const std::filesystem::path &base_directory) {
  try {
    return parse(Json::parse(json), base_directory);
  } catch (const Json::exception &error) {
    return std::unexpected{
        manifest_error(std::string{"invalid HDS manifest JSON: "} + error.what())};
  }
}

Result<HdsBuildManifest> load_hds_build_manifest(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  if (!input)
    return std::unexpected{
        make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not open HDS manifest")};
  std::ostringstream text;
  text << input.rdbuf();
  if (!input && !input.eof())
    return std::unexpected{
        make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not read HDS manifest")};
  return parse_hds_build_manifest(text.str(), path.parent_path());
}

Result<MediaBuildManifest> parse_media_build_manifest(std::string_view json,
                                                      const std::filesystem::path &base_directory) {
  try {
    return parse_media(Json::parse(json), base_directory);
  } catch (const Json::exception &error) {
    return std::unexpected{
        manifest_error(std::string{"invalid media manifest JSON: "} + error.what())};
  }
}

Result<MediaBuildManifest> load_media_build_manifest(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  if (!input)
    return std::unexpected{
        make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not open media manifest")};
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input && !input.eof())
    return std::unexpected{
        make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not read media manifest")};
  return parse_media_build_manifest(contents.str(), path.parent_path());
}

Result<std::vector<PartitionGeometry>> plan_hds_geometry(const HdsBuildManifest &manifest) {
  if (manifest.partitions.empty() || manifest.partitions.size() > 8U ||
      manifest.size_bytes < minimum_hds_size || manifest.size_bytes > maximum_hds_size ||
      manifest.size_bytes % 512U != 0U) {
    return std::unexpected{manifest_error("HDS manifest geometry is outside the writer profile")};
  }
  constexpr std::uint64_t start = 3;
  constexpr std::uint64_t maximum_slot = 1'073'741'824 / 512U - 1U;
  const auto slot = std::min(
      (manifest.size_bytes / 512U - (start - 1U)) / manifest.partitions.size(), maximum_slot);
  const auto sectors = slot - 1U;
  if (sectors < 2045U)
    return std::unexpected{manifest_error("partition slots are too small for SFS")};
  const auto clusters = sectors / 2U;
  const auto bitmap_count = ((clusters + 7U) / 8U + 1023U) / 1024U;
  std::vector<PartitionGeometry> result;
  for (std::size_t index = 0; index < manifest.partitions.size(); ++index) {
    const auto bitmap = 2U + bitmap_count;
    const auto directory = bitmap + bitmap_count;
    result.push_back({static_cast<std::uint8_t>(index), start + index * slot, slot, sectors,
                      clusters, bitmap, bitmap_count, directory, directory + 358U});
  }
  return result;
}

} // namespace axk
