#include "axklib/writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <span>

#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/media.hpp"
#include "axklib/utf8.hpp"
#include "axklib/writer_internal.hpp"

namespace axk {
namespace {

constexpr std::uint64_t cluster_size = 1024;

using detail::PreparedRecord;
using detail::RecordKind;

Result<void> validate_hds_image(const std::filesystem::path &path,
                                const std::vector<std::vector<PreparedRecord>> &records,
                                const CancellationToken &cancellation) {
    auto media = open_media(path, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (media->kind() != MediaKind::sfs) {
        return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                          "written HDS reopened as the wrong container type")};
    }
    auto catalog = build_object_catalog(*media, 64U * 1024U * 1024U, cancellation);
    if (!catalog)
        return std::unexpected{catalog.error()};
    std::size_t expected_objects{};
    for (const auto &partition : records) {
        expected_objects += static_cast<std::size_t>(std::ranges::count_if(
            partition, [](const PreparedRecord &record) { return record.kind == RecordKind::object; }));
    }
    if (catalog->objects.size() != expected_objects) {
        return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                          "written HDS object inventory failed reopen validation")};
    }
    return {};
}

Result<std::vector<std::byte>> ascii(std::string_view value, std::size_t size, std::byte pad = std::byte{' '}) {
    if (value.size() > size || !std::ranges::all_of(value, [](unsigned char character) { return character < 0x80U; })) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "writer name does not fit its ASCII field")};
    }
    std::vector<std::byte> result(size, pad);
    std::ranges::transform(value, result.begin(), [](char character) { return static_cast<std::byte>(character); });
    return result;
}

Result<std::array<std::byte, 32>> directory_entry(std::string_view name, std::uint32_t link,
                                                  std::optional<std::size_t> fixed_width,
                                                  std::optional<std::uint32_t> directory_id, std::uint8_t marker) {
    auto name_bytes = ascii(name, fixed_width.value_or(name.size()));
    if (!name_bytes || name_bytes->size() + 1U > 24U)
        return std::unexpected{
            name_bytes ? make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, "directory name is too long")
                       : name_bytes.error()};
    std::array<std::byte, 32> result{};
    ByteWriter writer{result};
    if (auto written = writer.write_be16(0, 0x20); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be16(2, static_cast<std::uint16_t>(name_bytes->size() + 1U)); !written)
        return std::unexpected{written.error()};
    if (auto written = writer.write_be32(4, link); !written)
        return std::unexpected{written.error()};
    std::ranges::copy(*name_bytes, result.begin() + 8);
    if (name == ".") {
        result[10] = std::byte{0x4f};
        result[11] = std::byte{0x58};
        if (auto written = writer.write_be32(16, 0x0017821a); !written)
            return std::unexpected{written.error()};
        std::fill(result.begin() + 24, result.begin() + 28, std::byte{0xff});
        result[31] = static_cast<std::byte>(marker);
    } else if (name == "..") {
        if (!directory_id || *directory_id > 255U)
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "directory ID exceeds the writer profile")};
        result[11] = static_cast<std::byte>(*directory_id);
        if (auto written = writer.write_be32(16, 0x001783c8); !written)
            return std::unexpected{written.error()};
    }
    return result;
}

using Entry = std::tuple<std::string, std::uint32_t, std::optional<std::size_t>>;

struct LoadedWaveform {
    WaveformSpec spec;
    ImportedAudio audio;
    std::uint32_t link_id{};
};

Result<std::vector<std::byte>> directory_payload(std::uint32_t id, std::uint32_t parent,
                                                 const std::vector<Entry> &entries, std::uint8_t marker = 0) {
    std::vector<std::byte> result;
    const auto append = [&](const auto &entry) { result.insert(result.end(), entry.begin(), entry.end()); };
    auto dot = directory_entry(".", id, {}, id, marker);
    auto dotdot = directory_entry("..", parent, {}, id, marker);
    if (!dot)
        return std::unexpected{dot.error()};
    if (!dotdot)
        return std::unexpected{dotdot.error()};
    append(*dot);
    append(*dotdot);
    for (const auto &[name, link, width] : entries) {
        auto item = directory_entry(name, link, width, {}, marker);
        if (!item)
            return std::unexpected{item.error()};
        append(*item);
    }
    return result;
}

} // namespace

Result<std::vector<PreparedRecord>> detail::prepare_partition_records(const PartitionSpec &partition,
                                                                      const PartitionGeometry &geometry,
                                                                      std::size_t partition_count,
                                                                      const CancellationToken &cancellation) {
    std::vector<PreparedRecord> records{{0, std::vector<std::byte>(32U * cluster_size), RecordKind::hidden},
                                        {2, {}, RecordKind::system}};
    std::vector<Entry> root{{"sfserrlog", 2, {}}, {"sfserram", 0, {}}};
    std::uint32_t next = 3;
    const auto marker = static_cast<std::uint8_t>(partition_count >= 3U ? geometry.index * 0x78U : 0U);
    constexpr std::array<std::string_view, 5> categories{"SMPL", "SBNK", "SBAC", "SEQU", "PROG"};
    for (const auto &volume : partition.volumes) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        if (volume.sample_banks.empty() != volume.programs.empty() ||
            volume.sample_banks.size() != volume.programs.size()) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "current SBAC/PROG profile requires one Sample Bank per Program")};
        }
        if (!volume.programs.empty()) {
            std::map<std::string, const SampleSpec *> sample_specs;
            std::map<std::string, const SampleBankSpec *> sample_bank_specs;
            for (const auto &sample : volume.samples)
                sample_specs.emplace(sample.name, &sample);
            for (const auto &sample_bank : volume.sample_banks)
                sample_bank_specs.emplace(sample_bank.name, &sample_bank);
            std::set<std::string> assigned_sample_banks;
            std::set<std::string> assigned_direct;
            for (const auto &program : volume.programs) {
                if (program.assignments.size() != 2U || program.assignments[0].target_kind != "SBAC" ||
                    program.assignments[0].receive_channel != 1U || program.assignments[1].target_kind != "SBNK" ||
                    program.assignments[1].receive_channel != 2U) {
                    return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                      "Program profile requires SBAC channel 1 "
                                                      "then SBNK channel 2")};
                }
                const auto sample_bank = sample_bank_specs.find(program.assignments[0].target_name);
                const auto direct = sample_specs.find(program.assignments[1].target_name);
                if (sample_bank == sample_bank_specs.end() || direct == sample_specs.end() ||
                    !assigned_sample_banks.insert(sample_bank->first).second ||
                    !assigned_direct.insert(direct->first).second ||
                    std::ranges::contains(sample_bank->second->member_samples, direct->first)) {
                    return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                      "Program targets must be unique, known, and separate")};
                }
                if (direct->second->right_waveform_id || direct->second->interleaved_audio_path) {
                    return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                      "SBAC/PROG writer profile supports mono Samples only")};
                }
                for (const auto &member_name : sample_bank->second->member_samples) {
                    const auto member = sample_specs.find(member_name);
                    if (member == sample_specs.end() || member->second->right_waveform_id ||
                        member->second->interleaved_audio_path) {
                        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                          "SBAC/PROG writer profile supports mono "
                                                          "Samples only")};
                    }
                }
                if (sample_bank->second->member_samples.size() == 1U) {
                    const auto *member = sample_specs.at(sample_bank->second->member_samples[0]);
                    if (member->waveform_id != direct->second->waveform_id ||
                        member->root_key != direct->second->root_key || member->key_low != direct->second->key_low ||
                        member->key_high != direct->second->key_high || member->level != direct->second->level) {
                        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                          "one-member Sample Bank and direct Sample control "
                                                          "parameters must match")};
                    }
                }
            }
            if (assigned_sample_banks.size() != sample_bank_specs.size()) {
                return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                  "every Sample Bank must be assigned once")};
            }
        }
        const auto volume_id = next++;
        std::array<std::uint32_t, 5> category_ids{};
        for (auto &value : category_ids)
            value = next++;
        root.emplace_back(volume.name, volume_id, 16U);
        std::vector<Entry> volume_entries;
        for (std::size_t index = 0; index < categories.size(); ++index)
            volume_entries.emplace_back(std::string{categories[index]}, category_ids[index], std::nullopt);
        auto volume_data = directory_payload(volume_id, 1, volume_entries, marker);
        if (!volume_data)
            return std::unexpected{volume_data.error()};
        records.push_back({volume_id, std::move(*volume_data), RecordKind::directory,
                           static_cast<std::uint16_t>(2U + categories.size())});
        std::vector<Entry> smpl_entries;
        std::vector<Entry> sbnk_entries;
        std::vector<Entry> sbac_entries;
        std::vector<Entry> prog_entries;
        std::vector<PreparedRecord> objects;
        std::map<std::string, LoadedWaveform> loaded;
        for (std::size_t waveform_index = 0; waveform_index < volume.waveforms.size(); ++waveform_index) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            const auto &spec = volume.waveforms[waveform_index];
            AudioImportOptions options;
            options.expected_channels = 1;
            options.target_sample_rate = spec.target_sample_rate;
            auto imported = import_sampler_audio(spec.path, options);
            if (!imported)
                return std::unexpected{imported.error()};
            const auto id = next++;
            const auto link_id = 0x016b1dbcU + static_cast<std::uint32_t>(waveform_index) * 0x100U;
            auto payload = detail::prepare_smpl_payload(spec, *imported, link_id);
            if (!payload)
                return std::unexpected{payload.error()};
            smpl_entries.emplace_back(spec.name, id, 16U);
            objects.push_back({id, std::move(*payload), RecordKind::object});
            loaded.emplace(spec.id, LoadedWaveform{spec, std::move(*imported), link_id});
        }
        std::map<std::string, std::pair<std::string, std::string>> generated_members;
        std::set<std::string> banked_samples;
        for (const auto &sample_bank : volume.sample_banks) {
            banked_samples.insert(sample_bank.member_samples.begin(), sample_bank.member_samples.end());
        }
        std::map<std::string, std::vector<std::uint8_t>> linked_programs;
        for (const auto &program : volume.programs) {
            if (program.assignments.size() != 2U || program.assignments[0].target_kind != "SBAC" ||
                program.assignments[0].receive_channel != 1U || program.assignments[1].target_kind != "SBNK" ||
                program.assignments[1].receive_channel != 2U) {
                return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                  "Program profile requires SBAC channel 1 then SBNK channel "
                                                  "2")};
            }
            linked_programs[program.assignments[1].target_name].push_back(program.number);
        }
        std::map<std::string, SampleSpec> samples;
        for (const auto &sample : volume.samples) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            if (!sample.interleaved_audio_path)
                continue;
            AudioImportOptions options;
            options.expected_channels = 2;
            options.target_sample_rate = sample.target_sample_rate;
            auto imported = import_sampler_audio(*sample.interleaved_audio_path, options);
            if (!imported)
                return std::unexpected{imported.error()};
            const auto base = sample.name.substr(0, std::min<std::size_t>(sample.name.size(), 14U));
            const auto left_name = sample.left_waveform_name.value_or(base + "-L");
            const auto right_name = sample.right_waveform_name.value_or(base + "-R");
            const auto left_key = sample.name + "#left";
            const auto right_key = sample.name + "#right";
            const auto make_channel = [&](std::size_t channel) {
                auto audio = *imported;
                audio.source_channels = 1;
                audio.pcm_channels = {imported->pcm_channels[channel]};
                return audio;
            };
            const auto add_member = [&](std::string key, std::string name, std::size_t channel) -> Result<void> {
                WaveformSpec spec{key, std::move(name), *sample.interleaved_audio_path, sample.root_key,
                                  sample.target_sample_rate};
                const auto link_id = 0x016b1dbcU + static_cast<std::uint32_t>(loaded.size()) * 0x100U;
                auto audio = make_channel(channel);
                auto payload = detail::prepare_smpl_payload(spec, audio, link_id);
                if (!payload)
                    return std::unexpected{payload.error()};
                const auto id = next++;
                smpl_entries.emplace_back(spec.name, id, 16U);
                objects.push_back({id, std::move(*payload), RecordKind::object});
                loaded.emplace(key, LoadedWaveform{std::move(spec), std::move(audio), link_id});
                return {};
            };
            if (auto added = add_member(left_key, left_name, 0); !added)
                return std::unexpected{added.error()};
            if (auto added = add_member(right_key, right_name, 1); !added)
                return std::unexpected{added.error()};
            generated_members.emplace(sample.name, std::pair{left_key, right_key});
        }
        for (const auto &sample : volume.samples) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            const auto generated = generated_members.find(sample.name);
            const auto left_key = sample.waveform_id                     ? *sample.waveform_id
                                  : generated == generated_members.end() ? std::string{}
                                                                         : generated->second.first;
            const auto right_key = sample.right_waveform_id               ? *sample.right_waveform_id
                                   : generated == generated_members.end() ? std::string{}
                                                                          : generated->second.second;
            const auto left = loaded.find(left_key);
            const auto right = right_key.empty() ? loaded.end() : loaded.find(right_key);
            if (left == loaded.end() || (!right_key.empty() && right == loaded.end()))
                return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                                  "SBNK references an unloaded waveform")};
            if (right != loaded.end() &&
                (left->second.audio.output_sample_rate != right->second.audio.output_sample_rate ||
                 left->second.audio.output_frames != right->second.audio.output_frames))
                return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                                  "stereo SBNK members require matching rate and frame "
                                                  "count")};
            const detail::PreparedWaveformMember left_member{
                left->second.spec.name, left->second.link_id, left->second.audio.output_sample_rate,
                static_cast<std::uint32_t>(left->second.audio.output_frames)};
            std::optional<detail::PreparedWaveformMember> right_member;
            if (right != loaded.end()) {
                right_member.emplace(detail::PreparedWaveformMember{
                    right->second.spec.name, right->second.link_id, right->second.audio.output_sample_rate,
                    static_cast<std::uint32_t>(right->second.audio.output_frames)});
            }
            auto payload = detail::prepare_sbnk_payload(
                sample, left_member, right_member, banked_samples.contains(sample.name), linked_programs[sample.name]);
            if (!payload)
                return std::unexpected{payload.error()};
            const auto id = next++;
            sbnk_entries.emplace_back(sample.name, id, 16U);
            objects.push_back({id, std::move(*payload), RecordKind::object});
            samples.emplace(sample.name, sample);
        }
        for (const auto &sample_bank : volume.sample_banks) {
            auto payload = detail::prepare_sbac_payload(sample_bank, samples);
            if (!payload)
                return std::unexpected{payload.error()};
            const auto id = next++;
            sbac_entries.emplace_back(sample_bank.name, id, 16U);
            objects.push_back({id, std::move(*payload), RecordKind::object});
        }
        for (const auto &program : volume.programs) {
            auto payload = detail::prepare_prog_payload(program);
            if (!payload)
                return std::unexpected{payload.error()};
            const auto id = next++;
            const auto name = std::format("{:03}", program.number);
            prog_entries.emplace_back(name, id, 16U);
            objects.push_back({id, std::move(*payload), RecordKind::object});
        }
        for (std::size_t category_index = 0; category_index < category_ids.size(); ++category_index) {
            const auto id = category_ids[category_index];
            auto category_data = directory_payload(id, volume_id,
                                                   category_index == 0U   ? smpl_entries
                                                   : category_index == 1U ? sbnk_entries
                                                   : category_index == 2U ? sbac_entries
                                                   : category_index == 4U ? prog_entries
                                                                          : std::vector<Entry>{},
                                                   marker);
            if (!category_data)
                return std::unexpected{category_data.error()};
            records.push_back({id, std::move(*category_data), RecordKind::directory, 2});
        }
        records.insert(records.end(), std::make_move_iterator(objects.begin()), std::make_move_iterator(objects.end()));
    }
    auto root_data = directory_payload(1, 1, root);
    if (!root_data)
        return std::unexpected{root_data.error()};
    records.push_back({1, std::move(*root_data), RecordKind::directory, static_cast<std::uint16_t>(root.size())});
    std::ranges::sort(records, {}, &PreparedRecord::id);
    if (auto index_size = checked_directory_index_size(records); !index_size)
        return std::unexpected{index_size.error()};
    std::uint64_t cluster = geometry.first_payload_cluster;
    for (auto &record : records) {
        if (record.kind == RecordKind::system)
            continue;
        record.cluster = static_cast<std::uint32_t>(cluster);
        record.clusters = static_cast<std::uint32_t>((record.payload.size() + cluster_size - 1U) / cluster_size);
        if (record.kind == RecordKind::directory || record.kind == RecordKind::object) {
            record.clusters = std::max(record.clusters, 2U);
        }
        cluster += record.clusters;
    }
    if (cluster >= geometry.cluster_count)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "partition does not have enough payload clusters")};
    return records;
}

Result<std::size_t> detail::checked_directory_index_size(std::span<const PreparedRecord> records) {
    if (records.empty()) {
        return std::unexpected{
            make_error(ErrorCode::internal_invariant, ErrorCategory::internal, "SFS directory index has no records")};
    }
    std::uint32_t maximum_id{};
    std::set<std::uint32_t> ids;
    for (const auto &record : records) {
        if (record.id >= sfs_directory_index_capacity) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "SFS directory index exceeds the 5012-entry writer profile")};
        }
        if (!ids.insert(record.id).second) {
            return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                              "SFS directory index contains a duplicate record ID")};
        }
        maximum_id = std::max(maximum_id, record.id);
    }
    const auto pages = static_cast<std::size_t>(maximum_id) / sfs_directory_index_records_per_page + 1U;
    if (pages > sfs_directory_index_page_capacity ||
        pages > std::numeric_limits<std::size_t>::max() / sfs_directory_index_page_bytes) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "SFS directory index exceeds its fixed page capacity")};
    }
    return pages * sfs_directory_index_page_bytes;
}

namespace {

class TemporaryFileCleanup {
  public:
    explicit TemporaryFileCleanup(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFileCleanup() {
        if (active_)
            detail::discard_temporary_file(path_);
    }
    TemporaryFileCleanup(const TemporaryFileCleanup &) = delete;
    TemporaryFileCleanup &operator=(const TemporaryFileCleanup &) = delete;
    void release() noexcept { active_ = false; }

  private:
    std::filesystem::path path_;
    bool active_{true};
};

} // namespace

Result<WrittenImageLayout> write_hds_image(const HdsBuildManifest &manifest, const std::filesystem::path &output_path,
                                           bool overwrite, const CancellationToken &cancellation) {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    auto geometries = plan_hds_geometry(manifest);
    if (!geometries)
        return std::unexpected{geometries.error()};
    if (!overwrite && std::filesystem::exists(output_path))
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "fresh HDS output already exists")};
    std::vector<std::vector<PreparedRecord>> all_records;
    for (std::size_t index = 0; index < geometries->size(); ++index) {
        auto records = detail::prepare_partition_records(manifest.partitions[index], (*geometries)[index],
                                                         geometries->size(), cancellation);
        if (!records)
            return std::unexpected{records.error()};
        all_records.push_back(std::move(*records));
    }
    std::error_code filesystem_error;
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    }
    if (filesystem_error)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create HDS output directory")};
    const auto temporary = detail::reserve_temporary_file(output_path);
    if (!temporary)
        return std::unexpected{temporary.error()};
    TemporaryFileCleanup cleanup{*temporary};
    if (auto resized = detail::resize_temporary_file(*temporary, manifest.size_bytes); !resized)
        return std::unexpected{resized.error()};
    std::vector<std::byte> superblock(512);
    const std::string_view magic{"YAMAHA_dev3"};
    std::ranges::transform(magic, superblock.begin(), [](char value) { return static_cast<std::byte>(value); });
    constexpr std::array<std::uint32_t, 7> residue{0xa1e00152, 0xa22c0000, 0x17,      0x09100000,
                                                   0x17,       0x09100000, 0x01000152};
    ByteWriter superblock_writer{superblock};
    for (std::size_t index = 0; index < residue.size(); ++index) {
        if (auto written = superblock_writer.write_be32(0x80 + index * 4U, residue[index]); !written)
            return std::unexpected{written.error()};
    }
    if (auto written = superblock_writer.write_be32(0x9c, 512); !written)
        return std::unexpected{written.error()};
    if (auto written = superblock_writer.write_be32(0xa0, static_cast<std::uint32_t>(manifest.size_bytes / 512U));
        !written)
        return std::unexpected{written.error()};
    for (const auto &geometry : *geometries) {
        if (auto written = superblock_writer.write_be32(0xa8 + geometry.index * 8U,
                                                        static_cast<std::uint32_t>(geometry.start_sector));
            !written)
            return std::unexpected{written.error()};
        if (auto written = superblock_writer.write_be32(0xac + geometry.index * 8U,
                                                        static_cast<std::uint32_t>(geometry.filesystem_sector_count));
            !written)
            return std::unexpected{written.error()};
    }
    if (!detail::write_temporary_file_at(*temporary, 0, superblock) ||
        !detail::write_temporary_file_at(*temporary, 512, superblock)) {
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write HDS superblocks")};
    }
    WrittenImageLayout layout{output_path, manifest.size_bytes, {}, 0};
    for (std::size_t partition_index = 0; partition_index < geometries->size(); ++partition_index) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto &geometry = (*geometries)[partition_index];
        const auto &records = all_records[partition_index];
        std::vector<std::byte> token(512);
        const auto token_text = std::format("{:08x}", 0xab432100U | geometry.index);
        std::ranges::transform(token_text, token.begin(), [](char value) { return static_cast<std::byte>(value); });
        const auto token_offset = geometry.index == 0 ? 1024U : (geometry.start_sector - 1U) * 512U;
        if (!detail::write_temporary_file_at(*temporary, token_offset, token))
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write HDS transfer token")};
        std::vector<std::byte> header(1024);
        std::ranges::transform(magic, header.begin(), [](char value) { return static_cast<std::byte>(value); });
        auto name =
            ascii(manifest.partitions[partition_index].name, geometry.index > 0 && geometries->size() > 1 ? 15U : 16U);
        if (!name)
            return std::unexpected{name.error()};
        std::ranges::copy(*name, header.begin() + 0x40);
        if (geometry.index > 0 && geometries->size() > 1)
            header[0x4f] = static_cast<std::byte>('0' + geometry.index);
        ByteWriter header_writer{header};
        if (auto written = header_writer.write_be32(0x80, 2); !written)
            return std::unexpected{written.error()};
        if (auto written = header_writer.write_be32(0x84, 200); !written)
            return std::unexpected{written.error()};
        std::fill(header.begin() + 0x88, header.begin() + 0x90, std::byte{0xff});
        for (const auto &[offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 7>{
                 {{0x90, static_cast<std::uint32_t>(geometry.cluster_count)},
                  {0x94, 2},
                  {0x98, 2},
                  {0x9c, static_cast<std::uint32_t>(geometry.bitmap_cluster)},
                  {0xa0, detail::sfs_directory_index_capacity},
                  {0xa4, static_cast<std::uint32_t>(geometry.directory_index_cluster)},
                  {0xa8, detail::sfs_directory_index_page_capacity}}}) {
            if (auto written = header_writer.write_be32(offset, value); !written)
                return std::unexpected{written.error()};
        }
        const auto dynamic = 0x0152a3fcU + geometry.index * 0x11U;
        const auto total_sectors = manifest.size_bytes / 512U;
        if (geometries->size() == 2U)
            header[0xaf] = static_cast<std::byte>(geometry.index);
        for (const auto &[offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 17>{
                 {{0x104, static_cast<std::uint32_t>(geometry.start_sector)},
                  {0x114, geometry.index * 0x20U},
                  {0x118, static_cast<std::uint32_t>(geometry.start_sector)},
                  {0x11c, static_cast<std::uint32_t>(geometry.filesystem_sector_count)},
                  {0x134, dynamic},
                  {0x144, geometry.index == 0 ? 0x016f5bf0U : 0x015d6cc0U},
                  {0x14c, static_cast<std::uint32_t>(
                              geometries->size() >= 3U ? total_sectors - (geometries->size() - 2U) : total_sectors)},
                  {0x154, geometry.index},
                  {0x158, dynamic},
                  {0x15c, geometry.index * 0x20U},
                  {0x160, static_cast<std::uint32_t>(geometry.index * geometry.filesystem_sector_count)},
                  {0x164, dynamic},
                  {0x178, static_cast<std::uint32_t>(geometry.start_sector)},
                  {0x17c, static_cast<std::uint32_t>(geometry.filesystem_sector_count)},
                  {0x184, dynamic},
                  {0x194, geometries->size() >= 3U ? 0U : static_cast<std::uint32_t>(geometries->size())},
                  {0x1a8, geometry.index}}}) {
            if (auto written = header_writer.write_be32(offset, value); !written)
                return std::unexpected{written.error()};
        }
        const auto start = geometry.start_sector * 512U;
        if (!detail::write_temporary_file_at(*temporary, start, header) ||
            !detail::write_temporary_file_at(*temporary, start + 1024U, header))
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write partition headers")};
        std::vector<std::byte> bitmap(geometry.bitmap_cluster_count * cluster_size);
        auto index_size = detail::checked_directory_index_size(records);
        if (!index_size)
            return std::unexpected{index_size.error()};
        std::vector<std::byte> index(*index_size);
        std::uint64_t allocated{};
        for (const auto &record : records) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            allocated += record.clusters;
            const auto record_end = static_cast<std::uint64_t>(record.cluster) + record.clusters;
            if (record_end > geometry.cluster_count) {
                return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                                  "prepared SFS record exceeds the partition cluster range")};
            }
            for (std::uint32_t cluster = record.cluster; cluster < record_end; ++cluster) {
                const auto byte = static_cast<std::size_t>(cluster / 8U);
                if (byte >= bitmap.size()) {
                    return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                                      "prepared SFS allocation exceeds the bitmap")};
                }
                bitmap[byte] |= static_cast<std::byte>(0x80U >> (cluster % 8U));
            }
            const auto offset =
                (record.id / detail::sfs_directory_index_records_per_page) * detail::sfs_directory_index_page_bytes +
                (record.id % detail::sfs_directory_index_records_per_page) * detail::sfs_directory_index_record_bytes;
            auto bytes = detail::encode_sfs_index_record(record);
            if (!bytes)
                return std::unexpected{bytes.error()};
            if (offset > index.size() || bytes->size() > index.size() - offset) {
                return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                                  "prepared SFS directory record exceeds the index")};
            }
            std::ranges::copy(*bytes, std::span{index}.subspan(offset, bytes->size()).begin());
            if (!record.payload.empty() &&
                !detail::write_temporary_file_at(*temporary, (geometry.start_sector + record.cluster * 2U) * 512U,
                                                 record.payload))
                return std::unexpected{
                    make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write partition payload")};
        }
        if (!detail::write_temporary_file_at(*temporary, start + 2048U, std::span{bitmap}.first(512)) ||
            !detail::write_temporary_file_at(*temporary, (geometry.start_sector + geometry.bitmap_cluster * 2U) * 512U,
                                             bitmap) ||
            !detail::write_temporary_file_at(
                *temporary, (geometry.start_sector + geometry.directory_index_cluster * 2U) * 512U, index))
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write partition allocation data")};
        const auto free_clusters = geometry.cluster_count - geometry.first_payload_cluster - allocated;
        layout.partitions.push_back({geometry, manifest.partitions[partition_index].name, allocated, free_clusters,
                                     free_clusters * 1024U, (free_clusters * 1024U) / 1024U});
    }
    if (const auto check = cancellation.check(); !check) {
        return std::unexpected{check.error()};
    }
    if (auto flushed = detail::flush_file_to_disk(*temporary); !flushed)
        return std::unexpected{flushed.error()};
    if (auto validated = validate_hds_image(*temporary, all_records, cancellation); !validated)
        return std::unexpected{validated.error()};
    if (auto published = detail::publish_temporary_file(*temporary, output_path, overwrite); !published)
        return std::unexpected{published.error()};
    cleanup.release();
    const auto &last = geometries->back();
    layout.unused_tail_sectors = manifest.size_bytes / 512U - (last.start_sector + last.filesystem_sector_count);
    return layout;
}

Result<HdsBuildPlanSummary> plan_hds_build(const HdsBuildManifest &manifest, const CancellationToken &cancellation) {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    auto geometries = plan_hds_geometry(manifest);
    if (!geometries)
        return std::unexpected{geometries.error()};
    std::size_t object_count{};
    for (std::size_t index = 0; index < geometries->size(); ++index) {
        auto records = detail::prepare_partition_records(manifest.partitions[index], (*geometries)[index],
                                                         geometries->size(), cancellation);
        if (!records)
            return std::unexpected{records.error()};
        object_count += static_cast<std::size_t>(std::ranges::count_if(
            *records, [](const PreparedRecord &record) { return record.kind == RecordKind::object; }));
    }
    return HdsBuildPlanSummary{manifest.size_bytes, geometries->size(), object_count, std::move(*geometries)};
}

} // namespace axk
