#include "axklib/audio_export.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <tuple>
#include <unordered_map>

#include "axklib/media.hpp"
#include "axklib/utf8.hpp"

namespace axk {
namespace {

std::string safe_component(std::string value, std::string_view fallback) {
  const auto trim = [](std::string &text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
      text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
      text.pop_back();
    }
  };
  trim(value);
  std::size_t duplicate_count{};
  while (!value.empty() && value.back() == '*') {
    ++duplicate_count;
    value.pop_back();
  }
  if (duplicate_count != 0U)
    trim(value);
  std::string cleaned;
  bool previous_space{};
  bool previous_underscore{};
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (character == '<' || character == '>') {
      cleaned += character == '<' ? "_lt_" : "_gt_";
      previous_space = false;
      previous_underscore = true;
      continue;
    }
    const bool invalid = byte < 0x20U || std::string_view{":\"/\\|?*"}.contains(character);
    if (invalid || character == '_') {
      if (!previous_underscore)
        cleaned += '_';
      previous_space = false;
      previous_underscore = true;
      continue;
    }
    if (std::isspace(byte) != 0) {
      if (!previous_space && !cleaned.empty())
        cleaned += ' ';
      previous_space = true;
      previous_underscore = false;
      continue;
    }
    cleaned += character;
    previous_space = false;
    previous_underscore = false;
  }
  while (!cleaned.empty() &&
         (cleaned.back() == ' ' || cleaned.back() == '.' || cleaned.back() == '_')) {
    cleaned.pop_back();
  }
  while (!cleaned.empty() &&
         (cleaned.front() == ' ' || cleaned.front() == '.' || cleaned.front() == '_')) {
    cleaned.erase(cleaned.begin());
  }
  if (cleaned.empty())
    cleaned = fallback;
  if (duplicate_count != 0U)
    cleaned += std::format(" ({})", duplicate_count + 1U);
  return cleaned;
}

std::string underscore_name(std::string value, std::string_view fallback) {
  value = safe_component(std::move(value), fallback);
  for (auto &character : value) {
    if (std::isspace(static_cast<unsigned char>(character)) != 0)
      character = '_';
  }
  return value;
}

std::string unique_wav_name(std::string stem, std::set<std::string> &used) {
  stem = safe_component(std::move(stem), "waveform");
  auto candidate = stem + ".wav";
  for (std::size_t index = 2; used.contains(candidate); ++index) {
    candidate = std::format("{} ({}).wav", stem, index);
  }
  used.insert(candidate);
  return candidate;
}

std::optional<std::int64_t> numeric(const CurrentSbnk &bank, std::string_view name) {
  const auto *field = bank.find_numeric_field(name);
  return field == nullptr ? std::nullopt : field->value;
}

const ObjectSnapshot *object(const ObjectCatalog &catalog, std::string_view key) {
  const auto found = std::ranges::find(catalog.objects, key, &ObjectSnapshot::key);
  return found == catalog.objects.end() ? nullptr : &*found;
}

std::string sfz_region(const SampleBankExport &bank, const PhysicalWaveformExport &waveform,
                       std::string sample_path, std::optional<int> pan) {
  std::string line{"<region>"};
  const auto key_low = bank.key_low == 255U ? waveform.waveform.root_key : bank.key_low;
  const auto key_high = bank.key_high == 128U ? waveform.waveform.root_key : bank.key_high;
  line += std::format(" lokey={} hikey={}", key_low, key_high);
  line += std::format(" pitch_keycenter={}", waveform.waveform.root_key);
  line += std::format(" transpose={} tune={}", bank.coarse_tune, waveform.waveform.fine_tune_cents);
  if (pan)
    line += std::format(" pan={}", *pan);
  auto loop_label = waveform.waveform.loop_mode_label;
  std::ranges::transform(loop_label, loop_label.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  if (loop_label.contains("one") && loop_label.contains("shot")) {
    line += " loop_mode=one_shot";
  } else if (!loop_label.empty() && waveform.waveform.loop_length != 0U) {
    line += std::format(" loop_mode=loop_continuous loop_start={} loop_end={}",
                        waveform.waveform.loop_start,
                        waveform.waveform.loop_start + waveform.waveform.loop_length - 1U);
  }
  line += " sample=" + std::move(sample_path);
  return line;
}

Result<void> write_text_atomic(const std::filesystem::path &path, std::string_view text,
                               bool overwrite) {
  if (!overwrite && std::filesystem::exists(path)) {
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "refusing to replace an existing SFZ")};
  }
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "could not create SFZ output directory")};
  }
  const auto temporary = text::temporary_sibling(path);
  if (!temporary)
    return std::unexpected{temporary.error()};
  {
    std::ofstream output{*temporary, std::ios::binary | std::ios::trunc};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
      std::filesystem::remove(*temporary, error);
      return std::unexpected{make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                        "could not write temporary SFZ")};
    }
  }
  if (overwrite)
    std::filesystem::remove(path, error);
  std::filesystem::rename(*temporary, path, error);
  if (error) {
    std::filesystem::remove(*temporary, error);
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "could not publish SFZ atomically")};
  }
  return {};
}

} // namespace

Result<ExportPlan> build_export_plan(const Container &container, const ObjectCatalog &catalog,
                                     const RelationshipGraph &graph,
                                     const CancellationToken &cancellation) {
  ExportPlan result;
  result.source_path = container.source_path();
  using Key = std::pair<std::uint8_t, std::uint32_t>;
  std::map<Key, VolumeExport> volumes;
  for (const auto &item : catalog.objects) {
    if (!item.placement)
      continue;
    const Key key{item.partition.value, item.placement->volume_directory.value};
    auto &volume = volumes[key];
    volume.partition = item.partition;
    volume.partition_name = item.placement->partition_name;
    volume.volume_name = item.placement->volume_name;
    volume.relative_root = std::filesystem::path{std::format(
                               "partition_{:02}_{}", item.partition.value,
                               underscore_name(item.placement->partition_name, "partition"))} /
                           safe_component(item.placement->volume_name, "volume");
  }

  std::unordered_map<std::string, PhysicalWaveformExport *> physical_by_key;
  std::map<Key, std::set<std::string>> rendered_names;
  for (auto &[key, volume] : volumes) {
    const auto waveform_count = std::ranges::count_if(catalog.objects, [&](const auto &item) {
      return item.placement && item.object.header.type == ObjectType::smpl &&
             item.partition.value == volume.partition.value &&
             item.placement->volume_directory.value == key.second;
    });
    volume.waveforms.reserve(static_cast<std::size_t>(waveform_count));
    std::set<std::string> used;
    for (const auto &item : catalog.objects) {
      if (!item.placement || item.object.header.type != ObjectType::smpl ||
          item.partition.value != volume.partition.value ||
          item.placement->volume_directory.value != key.second) {
        continue;
      }
      if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
      auto waveform = decode_waveform(container, item, cancellation);
      if (!waveform)
        return std::unexpected{waveform.error()};
      const auto filename = unique_wav_name(item.object.header.name, used);
      volume.waveforms.push_back({item.key, item.object.header.name,
                                  std::filesystem::path{"SMPL"} / filename, std::move(*waveform)});
      physical_by_key[item.key] = &volume.waveforms.back();
    }
  }

  for (const auto &item : catalog.objects) {
    if (!item.placement)
      continue;
    const Key key{item.partition.value, item.placement->volume_directory.value};
    auto &volume = volumes.at(key);
    if (const auto *bank = std::get_if<CurrentSbnk>(&item.object.payload)) {
      SampleBankExport output;
      output.object_key = item.key;
      output.display_name = item.object.header.name;
      output.key_low = bank->key_range_low;
      output.key_high = bank->key_range_high;
      output.coarse_tune =
          static_cast<std::int8_t>(numeric(*bank, "coarse_tune_0x0d5").value_or(0));
      output.decoded = *bank;
      const auto add_member = [&](std::string_view type, std::string role) {
        for (const auto *relation : graph.children(item.key)) {
          if (relation->type != type || relation->quality != RelationshipQuality::known ||
              !relation->target_key) {
            continue;
          }
          const auto physical = physical_by_key.find(*relation->target_key);
          if (physical != physical_by_key.end()) {
            output.members.push_back({std::move(role), *relation->target_key,
                                      physical->second->relative_wav_path, relation->quality});
          }
        }
      };
      add_member("SBNK_LEFT_MEMBER_TO_SMPL", "left");
      add_member("SBNK_RIGHT_MEMBER_TO_SMPL", "right");
      if (output.members.size() == 2U) {
        const auto *left = physical_by_key[output.members[0].waveform_key];
        const auto *right = physical_by_key[output.members[1].waveform_key];
        output.stereo_decision = stereo_render_decision(left->waveform, right->waveform);
        if (output.stereo_decision->renderable) {
          auto &used = rendered_names[key];
          const auto filename = unique_wav_name(output.display_name, used);
          output.rendered_wav_path = std::filesystem::path{"RENDERED"} / filename;
        }
      }
      std::set<std::string> context_keys;
      for (const auto &member : output.members) {
        for (const auto *relation : graph.parents(member.waveform_key)) {
          if (!relation->target_key || *relation->target_key != member.waveform_key ||
              relation->quality != RelationshipQuality::known ||
              (relation->type != "SBNK_LEFT_MEMBER_TO_SMPL" &&
               relation->type != "SBNK_RIGHT_MEMBER_TO_SMPL") ||
              !context_keys.insert(relation->source_key).second) {
            continue;
          }
          const auto *source = object(catalog, relation->source_key);
          if (source == nullptr)
            continue;
          const auto *parameters = std::get_if<CurrentSbnk>(&source->object.payload);
          if (parameters == nullptr)
            continue;
          output.parameter_contexts.push_back(
              {source->key, source->object.header.name, relation->type, *parameters});
        }
      }
      volume.sample_banks.push_back(std::move(output));
    } else if (std::holds_alternative<CurrentSbac>(item.object.payload)) {
      SampleBankGroupExport output{item.key, item.object.header.name, {}};
      for (const auto *relation : graph.children(item.key)) {
        if (relation->type == "SBAC_SLOT_TO_SBNK" && relation->target_key &&
            relation->quality == RelationshipQuality::known) {
          output.member_bank_keys.push_back(*relation->target_key);
        }
      }
      volume.sample_bank_groups.push_back(std::move(output));
    } else if (std::holds_alternative<CurrentProg>(item.object.payload)) {
      ProgramExport output{item.key, item.object.header.name, {}};
      for (const auto *relation : graph.children(item.key)) {
        if (relation->type.starts_with("PROG_ASSIGNMENT_TO_") && relation->target_key &&
            (relation->assignment_state == AssignmentState::active ||
             relation->assignment_state == AssignmentState::source_load)) {
          output.assignment_target_keys.push_back(*relation->target_key);
        }
      }
      volume.programs.push_back(std::move(output));
    }
  }
  for (auto &[key, volume] : volumes) {
    static_cast<void>(key);
    result.volumes.push_back(std::move(volume));
  }
  return result;
}

Result<ExportPlan> build_export_plan(const MediaContainer &container, const ObjectCatalog &catalog,
                                     const RelationshipGraph &graph,
                                     const CancellationToken &cancellation) {
  if (container.kind() == MediaKind::sfs) {
    return build_export_plan(std::get<Container>(container.storage()), catalog, graph,
                             cancellation);
  }
  auto media_objects = container.objects(64U * 1024U * 1024U, cancellation);
  if (!media_objects)
    return std::unexpected{media_objects.error()};
  const auto paths = structured_object_paths(*media_objects);
  std::unordered_map<std::string, const MediaObject *> media_by_key;
  std::unordered_map<std::string, std::filesystem::path> roots_by_key;
  for (std::size_t index = 0; index < media_objects->size(); ++index) {
    const auto &item = (*media_objects)[index];
    media_by_key.emplace(item.key, &item);
    roots_by_key.emplace(item.key, paths[index].relative_path.parent_path().parent_path());
  }

  ExportPlan result;
  result.source_path = container.source_path();
  using Key = std::pair<std::uint8_t, std::uint32_t>;
  std::map<Key, VolumeExport> volumes;
  for (const auto &item : catalog.objects) {
    if (!item.placement)
      continue;
    const Key key{item.partition.value, item.placement->volume_directory.value};
    auto &volume = volumes[key];
    volume.partition = item.partition;
    volume.partition_name = item.placement->partition_name;
    volume.volume_name = item.placement->volume_name;
    volume.relative_root = roots_by_key.at(item.key);
  }

  std::unordered_map<std::string, PhysicalWaveformExport *> physical_by_key;
  std::map<Key, std::set<std::string>> rendered_names;
  for (auto &[key, volume] : volumes) {
    const auto count = std::ranges::count_if(catalog.objects, [&](const auto &item) {
      return item.placement && item.object.header.type == ObjectType::smpl &&
             item.partition.value == volume.partition.value &&
             item.placement->volume_directory.value == key.second;
    });
    volume.waveforms.reserve(static_cast<std::size_t>(count));
    std::set<std::string> used;
    for (const auto &item : catalog.objects) {
      if (!item.placement || item.object.header.type != ObjectType::smpl ||
          item.partition.value != volume.partition.value ||
          item.placement->volume_directory.value != key.second) {
        continue;
      }
      if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
      const auto source = media_by_key.find(item.key);
      if (source == media_by_key.end()) {
        return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                          "media object disappeared while building export plan")};
      }
      auto waveform = decode_waveform(*source->second);
      if (!waveform)
        return std::unexpected{waveform.error()};
      volume.waveforms.push_back(
          {item.key, item.object.header.name,
           std::filesystem::path{"SMPL"} / unique_wav_name(item.object.header.name, used),
           std::move(*waveform)});
      physical_by_key[item.key] = &volume.waveforms.back();
    }
  }

  for (const auto &item : catalog.objects) {
    if (!item.placement)
      continue;
    const Key key{item.partition.value, item.placement->volume_directory.value};
    auto &volume = volumes.at(key);
    if (const auto *bank = std::get_if<CurrentSbnk>(&item.object.payload)) {
      SampleBankExport output;
      output.object_key = item.key;
      output.display_name = item.object.header.name;
      output.key_low = bank->key_range_low;
      output.key_high = bank->key_range_high;
      output.coarse_tune =
          static_cast<std::int8_t>(numeric(*bank, "coarse_tune_0x0d5").value_or(0));
      output.decoded = *bank;
      const auto add_member = [&](std::string_view type, std::string role) {
        for (const auto *relation : graph.children(item.key)) {
          if (relation->type != type || relation->quality != RelationshipQuality::known ||
              !relation->target_key) {
            continue;
          }
          const auto physical = physical_by_key.find(*relation->target_key);
          if (physical != physical_by_key.end()) {
            output.members.push_back({role, *relation->target_key,
                                      physical->second->relative_wav_path, relation->quality});
          }
        }
      };
      add_member("SBNK_LEFT_MEMBER_TO_SMPL", "left");
      add_member("SBNK_RIGHT_MEMBER_TO_SMPL", "right");
      if (output.members.size() == 2U) {
        const auto *left = physical_by_key.at(output.members[0].waveform_key);
        const auto *right = physical_by_key.at(output.members[1].waveform_key);
        output.stereo_decision = stereo_render_decision(left->waveform, right->waveform);
        if (output.stereo_decision->renderable) {
          output.rendered_wav_path = std::filesystem::path{"RENDERED"} /
                                     unique_wav_name(output.display_name, rendered_names[key]);
        }
      }
      std::set<std::string> context_keys;
      for (const auto &member : output.members) {
        for (const auto *relation : graph.parents(member.waveform_key)) {
          if (!relation->target_key || *relation->target_key != member.waveform_key ||
              relation->quality != RelationshipQuality::known ||
              (relation->type != "SBNK_LEFT_MEMBER_TO_SMPL" &&
               relation->type != "SBNK_RIGHT_MEMBER_TO_SMPL") ||
              !context_keys.insert(relation->source_key).second) {
            continue;
          }
          const auto *source = object(catalog, relation->source_key);
          if (source == nullptr)
            continue;
          const auto *parameters = std::get_if<CurrentSbnk>(&source->object.payload);
          if (parameters != nullptr) {
            output.parameter_contexts.push_back(
                {source->key, source->object.header.name, relation->type, *parameters});
          }
        }
      }
      volume.sample_banks.push_back(std::move(output));
    } else if (std::holds_alternative<CurrentSbac>(item.object.payload)) {
      SampleBankGroupExport output{item.key, item.object.header.name, {}};
      for (const auto *relation : graph.children(item.key)) {
        if (relation->type == "SBAC_SLOT_TO_SBNK" && relation->target_key &&
            relation->quality == RelationshipQuality::known) {
          output.member_bank_keys.push_back(*relation->target_key);
        }
      }
      volume.sample_bank_groups.push_back(std::move(output));
    } else if (std::holds_alternative<CurrentProg>(item.object.payload)) {
      ProgramExport output{item.key, item.object.header.name, {}};
      for (const auto *relation : graph.children(item.key)) {
        if (relation->type.starts_with("PROG_ASSIGNMENT_TO_") && relation->target_key &&
            (relation->assignment_state == AssignmentState::active ||
             relation->assignment_state == AssignmentState::source_load)) {
          output.assignment_target_keys.push_back(*relation->target_key);
        }
      }
      volume.programs.push_back(std::move(output));
    }
  }
  for (auto &[key, volume] : volumes) {
    static_cast<void>(key);
    result.volumes.push_back(std::move(volume));
  }
  return result;
}

Result<ExportResult> write_export_audio(const ExportPlan &plan,
                                        const std::filesystem::path &output_directory,
                                        bool overwrite, const CancellationToken &cancellation) {
  ExportResult result;
  std::set<std::filesystem::path> targets;
  for (const auto &volume : plan.volumes) {
    for (const auto &waveform : volume.waveforms) {
      targets.insert(output_directory / volume.relative_root / waveform.relative_wav_path);
    }
    for (const auto &bank : volume.sample_banks) {
      if (bank.rendered_wav_path) {
        targets.insert(output_directory / volume.relative_root / *bank.rendered_wav_path);
      }
    }
  }
  if (!overwrite) {
    const auto existing = std::ranges::find_if(
        targets, [](const auto &path) { return std::filesystem::exists(path); });
    if (existing != targets.end()) {
      return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                        "refusing to replace an existing audio export: " +
                                            text::path_to_utf8(*existing))};
    }
  }
  for (const auto &volume : plan.volumes) {
    for (const auto &waveform : volume.waveforms) {
      if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
      const auto path = output_directory / volume.relative_root / waveform.relative_wav_path;
      if (const auto written = write_wav_atomic(path, waveform.waveform, overwrite); !written) {
        return std::unexpected{written.error()};
      }
      result.written_files.push_back(path);
    }
    for (const auto &bank : volume.sample_banks) {
      if (!bank.rendered_wav_path || bank.members.size() != 2U)
        continue;
      const auto left = std::ranges::find(volume.waveforms, bank.members[0].waveform_key,
                                          &PhysicalWaveformExport::object_key);
      const auto right = std::ranges::find(volume.waveforms, bank.members[1].waveform_key,
                                           &PhysicalWaveformExport::object_key);
      if (left == volume.waveforms.end() || right == volume.waveforms.end())
        continue;
      const auto stereo = render_stereo(left->waveform, right->waveform);
      if (!stereo)
        return std::unexpected{stereo.error()};
      const auto path = output_directory / volume.relative_root / *bank.rendered_wav_path;
      if (const auto written = write_wav_atomic(path, *stereo, overwrite); !written) {
        return std::unexpected{written.error()};
      }
      result.written_files.push_back(path);
    }
  }
  return result;
}

Result<SfzExportResult> write_sfz(const ExportPlan &plan,
                                  const std::filesystem::path &output_directory, bool overwrite) {
  SfzExportResult result;
  if (!overwrite) {
    for (const auto &volume : plan.volumes) {
      for (const auto &group : volume.sample_bank_groups) {
        const auto path = output_directory / volume.relative_root /
                          (safe_component("B " + group.display_name, "instrument") + ".sfz");
        if (std::filesystem::exists(path)) {
          return std::unexpected{
              make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                         "refusing to replace an existing SFZ: " + text::path_to_utf8(path))};
        }
      }
      for (const auto &bank : volume.sample_banks) {
        const auto path = output_directory / volume.relative_root /
                          (safe_component(bank.display_name, "instrument") + ".sfz");
        if (std::filesystem::exists(path)) {
          return std::unexpected{
              make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                         "refusing to replace an existing SFZ: " + text::path_to_utf8(path))};
        }
      }
    }
  }
  for (const auto &volume : plan.volumes) {
    std::set<std::string> grouped;
    const auto write_instrument =
        [&](std::string name, const std::vector<const SampleBankExport *> &banks) -> Result<void> {
      std::string text =
          std::format("// Generated by axklib\n// Volume: {}\n// Instrument: {}\n\n<group>\n",
                      text::path_to_utf8(volume.relative_root), name);
      std::size_t region_count{};
      for (const auto *bank : banks) {
        if (bank->rendered_wav_path && !bank->members.empty()) {
          const auto waveform =
              std::ranges::find(volume.waveforms, bank->members.front().waveform_key,
                                &PhysicalWaveformExport::object_key);
          if (waveform != volume.waveforms.end()) {
            text += "// " + bank->display_name + '\n';
            text +=
                sfz_region(*bank, *waveform, text::path_to_utf8(*bank->rendered_wav_path), {}) +
                '\n';
            ++region_count;
          }
        } else {
          for (const auto &member : bank->members) {
            const auto waveform = std::ranges::find(volume.waveforms, member.waveform_key,
                                                    &PhysicalWaveformExport::object_key);
            if (waveform == volume.waveforms.end())
              continue;
            const bool physical_pair =
                bank->members.size() > 1U &&
                std::ranges::any_of(bank->members,
                                    [](const auto &item) { return item.role == "left"; }) &&
                std::ranges::any_of(bank->members,
                                    [](const auto &item) { return item.role == "right"; });
            std::optional<int> pan;
            if (physical_pair && member.role == "left")
              pan = -100;
            else if (physical_pair && member.role == "right")
              pan = 100;
            text += "// " + bank->display_name + '\n';
            text +=
                sfz_region(*bank, *waveform, text::path_to_utf8(member.relative_wav_path), pan) +
                '\n';
            ++region_count;
          }
        }
      }
      if (text.empty() || text.back() != '\n')
        text += '\n';
      if (region_count == 0U)
        return {};
      const auto path = output_directory / volume.relative_root /
                        (safe_component(std::move(name), "instrument") + ".sfz");
      if (const auto written = write_text_atomic(path, text, overwrite); !written) {
        return std::unexpected{written.error()};
      }
      result.written_files.push_back(path);
      return {};
    };
    for (const auto &group : volume.sample_bank_groups) {
      std::vector<const SampleBankExport *> banks;
      for (const auto &key : group.member_bank_keys) {
        const auto found =
            std::ranges::find(volume.sample_banks, key, &SampleBankExport::object_key);
        if (found != volume.sample_banks.end()) {
          banks.push_back(&*found);
          grouped.insert(key);
        }
      }
      if (const auto written = write_instrument("B " + group.display_name, banks); !written) {
        return std::unexpected{written.error()};
      }
    }
    for (const auto &bank : volume.sample_banks) {
      if (grouped.contains(bank.object_key))
        continue;
      if (const auto written = write_instrument(bank.display_name, {&bank}); !written) {
        return std::unexpected{written.error()};
      }
    }
  }
  return result;
}

} // namespace axk
