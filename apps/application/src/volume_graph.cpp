#include "axklib/application/volume_graph.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>

#include <nlohmann/json.hpp>

#include "axklib/utf8.hpp"

namespace axk::app {

using OrderedJson = nlohmann::ordered_json;

namespace {

OrderedJson member_json(const CurrentSbnkMember &member) {
    return {{"wave_data_name", member.wave_data_name},
            {"cached_wave_data_reference_value", member.cached_wave_data_reference_value},
            {"root_key", member.root_key},
            {"sample_rate", member.sample_rate},
            {"fine_tune_cents", member.fine_tune_cents},
            {"pitch_base_word", member.pitch_base_word},
            {"wave_start_frame", member.wave_start_frame},
            {"wave_length_frames", member.wave_length_frames},
            {"loop_start_frame", member.loop_start_frame},
            {"loop_length_frames", member.loop_length_frames}};
}

} // namespace

axk::Result<std::string> serialize_volume_graph(const VolumeExport &volume, const RelationshipGraph &graph,
                                                const std::filesystem::path &source_path,
                                                std::string_view container_kind) {
    try {
        OrderedJson smpl = OrderedJson::array();
        for (const auto &waveform : volume.waveforms) {
            const auto &audio = waveform.waveform;
            OrderedJson aliases = OrderedJson::array();
            for (const auto &alias : waveform.user_facing_aliases) {
                aliases.push_back({
                    {"sample_object_key", alias.sample_object_key},
                    {"display_name", alias.display_name},
                    {"relationship_quality", axk::relationship_quality_name(alias.relationship_quality)},
                });
            }
            smpl.push_back({
                {"id", waveform.object_key},
                {"object_key", waveform.object_key},
                {"display_name", waveform.display_name},
                {"wav_path", axk::text::path_to_utf8(waveform.relative_wav_path)},
                {"user_facing_aliases", std::move(aliases)},
                {"audio",
                 {{"channels", audio.format.channels},
                  {"sample_rate", audio.format.sample_rate},
                  {"sample_width_bytes", audio.format.sample_width_bytes},
                  {"frames", audio.frame_count},
                  {"stored_payload_size", audio.stored_payload_size},
                  {"decoded_pcm_size", audio.pcm.size()},
                  {"stored_payload_transform", audio.stored_payload_transform},
                  {"exactness_status", audio.alternating_byte_payload_detected ? "alternating-byte-compatibility-export"
                                                                               : "exact-current-mono"}}},
                {"playback",
                 {{"root_key_midi", audio.root_key},
                  {"fine_tune_cents", audio.fine_tune_cents},
                  {"loop_mode_raw", audio.loop_mode},
                  {"loop_mode_label", audio.loop_mode_label},
                  {"loop_start_frame", audio.loop_start},
                  {"loop_length_frames", audio.loop_length},
                  {"loop_end_frame_a4000_ui",
                   audio.loop_length == 0U
                       ? OrderedJson(nullptr)
                       : OrderedJson(static_cast<std::uint64_t>(audio.loop_start) + audio.loop_length)}}},
                {"origin",
                 {{"source_container", text::path_to_utf8(source_path)},
                  {"container_kind", container_kind},
                  {"partition_index", audio.partition.value},
                  {"quality", audio.alternating_byte_payload_detected ? "Likely" : "Known"},
                  {"basis", audio.alternating_byte_payload_detected
                                ? "direct object header plus alternating-byte payload "
                                  "detection"
                                : "direct object header and stored payload bytes"},
                  {"alternating_byte_payload_detected", audio.alternating_byte_payload_detected}}},
            });
        }
        OrderedJson sbnk = OrderedJson::array();
        for (const auto &sample : volume.samples) {
            OrderedJson members = OrderedJson::array();
            for (const auto &member : sample.members) {
                members.push_back({
                    {"role", member.role},
                    {"smpl_id", member.waveform_key},
                    {"wav_path", axk::text::path_to_utf8(member.relative_wav_path)},
                    {"relationship_quality", axk::relationship_quality_name(member.quality)},
                });
            }
            OrderedJson rendered = nullptr;
            if (sample.rendered_wav_path) {
                rendered = {
                    {"wav_path", axk::text::path_to_utf8(*sample.rendered_wav_path)},
                    {"channels", 2},
                    {"frames", sample.stereo_decision ? sample.stereo_decision->output_frame_count : 0},
                    {"padding",
                     {{"left_frames", sample.stereo_decision ? sample.stereo_decision->left_padding_frames : 0},
                      {"right_frames", sample.stereo_decision ? sample.stereo_decision->right_padding_frames : 0}}},
                };
            }
            OrderedJson parameter_contexts = OrderedJson::array();
            for (const auto &context : sample.parameter_contexts) {
                OrderedJson numeric_fields = OrderedJson::object();
                numeric_fields["sample_parameter_base_0x0a8"] = 0xa8;
                for (const auto &field : context.decoded.numeric_fields) {
                    if (field.value)
                        numeric_fields[field.name] = *field.value;
                }
                OrderedJson controls = OrderedJson::array();
                for (std::size_t index = 0; index < context.decoded.control_records.size(); ++index) {
                    const auto &control = context.decoded.control_records[index];
                    controls.push_back({
                        {"index", index + 1U},
                        {"offset", 0x164U + index * 4U},
                        {"device_u8", control.device},
                        {"function_u8", control.function},
                        {"type_u8", control.type},
                        {"range_s8", control.range},
                    });
                }
                numeric_fields["sample_control_records"] = std::move(controls);
                parameter_contexts.push_back({
                    {"sample_object_key", context.object_key},
                    {"sample_name", context.display_name},
                    {"relationship_type", context.relationship_type},
                    {"member_parameters", std::move(numeric_fields)},
                    {"left_member", member_json(context.decoded.left)},
                    {"right_member",
                     context.decoded.right ? member_json(*context.decoded.right) : OrderedJson(nullptr)},
                    {"selected_member", member_json(context.decoded.left)},
                    {"sample_topology", context.decoded.right ? "two-member" : "single-member"},
                    {"linked_program_numbers", context.decoded.linked_program_numbers},
                });
            }
            const auto &range_sample =
                sample.parameter_contexts.empty() ? sample.decoded : sample.parameter_contexts.front().decoded;
            const auto range_low = range_sample.key_range_low;
            const auto range_high = range_sample.key_range_high;
            OrderedJson resolved_range = {
                {"low_midi", range_low == 255U ? OrderedJson(range_sample.left.root_key) : OrderedJson(range_low)},
                {"high_midi", range_high == 128U ? OrderedJson(range_sample.left.root_key) : OrderedJson(range_high)},
                {"low_raw", range_low},
                {"high_raw", range_high},
                {"low_display", range_low == 255U ? OrderedJson("Orig") : OrderedJson(range_low)},
                {"high_display", range_high == 128U ? OrderedJson("Orig") : OrderedJson(range_high)},
                {"basis", range_low == 255U || range_high == 128U ? "sampler-orig-key-limit" : "decoded-key-range"},
            };
            sbnk.push_back({
                {"id", sample.object_key},
                {"object_key", sample.object_key},
                {"display_name", sample.display_name},
                {"physical_waveforms", std::move(members)},
                {"rendered_audio", std::move(rendered)},
                {"parameters",
                 {{"decoded_current_sbnk_member_parameters", std::move(parameter_contexts)},
                  {"resolved_key_range", std::move(resolved_range)}}},
            });
        }
        OrderedJson sbac = OrderedJson::array();
        for (const auto &sample_bank : volume.sample_banks) {
            OrderedJson members = OrderedJson::array();
            for (const auto &key : sample_bank.member_sample_keys)
                members.push_back({{"sbnk_id", key}});
            sbac.push_back({
                {"id", sample_bank.object_key},
                {"object_key", sample_bank.object_key},
                {"display_name", sample_bank.display_name},
                {"members", std::move(members)},
                {"relationship_sample_keys", sample_bank.relationship_sample_keys},
            });
        }
        OrderedJson prog = OrderedJson::array();
        for (const auto &program : volume.programs) {
            prog.push_back({
                {"id", program.object_key},
                {"object_key", program.object_key},
                {"display_name", program.display_name},
                {"assignment_target_keys", program.assignment_target_keys},
            });
        }
        OrderedJson relationships = OrderedJson::array();
        std::set<std::string> volume_keys;
        std::set<std::pair<std::string, std::string>> volume_edges;
        std::set<std::pair<std::string, std::string>> relationship_edges;
        for (const auto &row : volume.waveforms)
            volume_keys.insert(row.object_key);
        for (const auto &row : volume.samples) {
            volume_keys.insert(row.object_key);
            for (const auto &member : row.members)
                volume_edges.emplace(row.object_key, member.waveform_key);
        }
        for (const auto &row : volume.sample_banks) {
            volume_keys.insert(row.object_key);
            for (const auto &member : row.member_sample_keys)
                volume_edges.emplace(row.object_key, member);
            for (const auto &target : row.relationship_sample_keys)
                relationship_edges.emplace(row.object_key, target);
        }
        for (const auto &row : volume.programs) {
            volume_keys.insert(row.object_key);
            for (const auto &target : row.assignment_target_keys)
                volume_edges.emplace(row.object_key, target);
        }
        for (const auto &row : graph.relationships) {
            if (!volume_keys.contains(row.source_key))
                continue;
            if (!row.target_key)
                continue;
            const auto retained_relationship = relationship_edges.contains({row.source_key, *row.target_key});
            if (!retained_relationship && !volume_edges.contains({row.source_key, *row.target_key}))
                continue;
            if (row.type == "SBNK_PROGRAM_BITMAP_TO_PROG") {
                continue;
            }
            relationships.push_back({
                {"relationship_type", row.type},
                {"source_key", row.source_key},
                {"target_key", row.target_key ? *row.target_key : ""},
                {"quality", axk::relationship_quality_name(row.quality)},
                {"basis", row.basis},
                {"active_assignment_state", axk::assignment_state_name(row.assignment_state)},
            });
        }
        OrderedJson decisions = OrderedJson::array();
        for (const auto &sample : volume.samples) {
            if (!sample.stereo_decision)
                continue;
            decisions.push_back({
                {"sbnk_object_key", sample.object_key},
                {"sample_name", sample.display_name},
                {"decision",
                 sample.stereo_decision->renderable ? "interleaved_stereo_written" : "physical_members_only"},
                {"reason_code", sample.stereo_decision->reason_code},
                {"output_frame_count", sample.stereo_decision->output_frame_count},
                {"left_padding_frames", sample.stereo_decision->left_padding_frames},
                {"right_padding_frames", sample.stereo_decision->right_padding_frames},
            });
        }
        return OrderedJson{
            {"schema", volume_graph_schema_version},
            {"source",
             {{"containers", {text::path_to_utf8(source_path)}},
              {"container_kinds", {container_kind}},
              {"source_scope", ""}}},
            {"volume",
             {{"path", text::path_to_utf8(volume.relative_root)},
              {"partition_index", volume.partition.value},
              {"partition_name", volume.partition_name},
              {"name", volume.volume_name},
              {"placement_quality", "Known"}}},
            {"objects",
             {{"smpl", std::move(smpl)},
              {"sbnk", std::move(sbnk)},
              {"sbac", std::move(sbac)},
              {"prog", std::move(prog)},
              {"sequ", OrderedJson::array()}}},
            {"relationships", std::move(relationships)},
            {"stereo_decisions", std::move(decisions)},
        }
            .dump(2);
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::internal,
                                          std::string{"could not serialize volume graph JSON: "} + error.what())};
    }
}

axk::Result<std::string> serialize_unresolved_wave_data_graph(const UnresolvedWaveDataExport &scope,
                                                              const std::filesystem::path &source_path,
                                                              std::string_view container_kind) {
    try {
        OrderedJson waveforms = OrderedJson::array();
        for (const auto &waveform : scope.waveforms) {
            OrderedJson candidates = OrderedJson::array();
            for (const auto &candidate : waveform.placement_candidates) {
                candidates.push_back({
                    {"partition_index", candidate.partition.value},
                    {"partition_name", candidate.partition_name},
                    {"volume_directory_sfs_id", candidate.volume_directory.value},
                    {"volume_name", candidate.volume_name},
                    {"category_name", candidate.category_name},
                    {"entry_name", candidate.entry_name},
                    {"container_directory", candidate.container_directory},
                });
            }
            const auto &audio = waveform.waveform;
            OrderedJson aliases = OrderedJson::array();
            for (const auto &alias : waveform.user_facing_aliases) {
                aliases.push_back({
                    {"sample_object_key", alias.sample_object_key},
                    {"display_name", alias.display_name},
                    {"relationship_quality", axk::relationship_quality_name(alias.relationship_quality)},
                });
            }
            waveforms.push_back({
                {"id", waveform.object_key},
                {"object_key", waveform.object_key},
                {"display_name", waveform.display_name},
                {"wav_path", text::path_to_utf8(waveform.relative_wav_path)},
                {"user_facing_aliases", std::move(aliases)},
                {"placement",
                 {{"resolution", placement_resolution_name(waveform.placement_resolution)},
                  {"quality",
                   waveform.placement_resolution == PlacementResolution::exact ? "authoritative" : "unresolved"},
                  {"basis", waveform.placement_resolution == PlacementResolution::ambiguous
                                ? "multiple directory placement candidates"
                                : "no authoritative directory placement"},
                  {"candidates", std::move(candidates)}}},
                {"audio",
                 {{"channels", audio.format.channels},
                  {"sample_rate", audio.format.sample_rate},
                  {"sample_width_bytes", audio.format.sample_width_bytes},
                  {"frames", audio.frame_count},
                  {"stored_payload_size", audio.stored_payload_size},
                  {"decoded_pcm_size", audio.pcm.size()},
                  {"exactness_status", audio.alternating_byte_payload_detected ? "alternating-byte-compatibility-export"
                                                                               : "exact-current-mono"}}},
            });
        }
        return OrderedJson{
            {"schema", unresolved_wave_data_schema_version},
            {"source", {{"container", text::path_to_utf8(source_path)}, {"container_kind", container_kind}}},
            {"scope",
             {{"kind", "unresolved_physical_wave_data"},
              {"path", text::path_to_utf8(scope.relative_root)},
              {"partition_index", scope.partition.value},
              {"partition_name", scope.partition_name}}},
            {"objects", {{"smpl", std::move(waveforms)}}},
        }
            .dump(2);
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::internal,
                       std::string{"could not serialize unresolved Wave Data JSON: "} + error.what())};
    }
}

} // namespace axk::app
