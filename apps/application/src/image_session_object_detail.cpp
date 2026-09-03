#include "image_sessions_internal.hpp"

#include <iomanip>
#include <sstream>

namespace {

using Json = nlohmann::ordered_json;

std::string hex(std::span<const std::byte> bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : bytes)
        output << std::setw(2) << static_cast<unsigned int>(std::to_integer<std::uint8_t>(byte));
    return output.str();
}

std::string_view verification_name(axk::Verification verification) {
    switch (verification) {
    case axk::Verification::verified:
        return "VERIFIED";
    case axk::Verification::corroborated:
        return "CORROBORATED";
    case axk::Verification::tentative:
        return "TENTATIVE";
    case axk::Verification::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::string_view wire_placement_resolution_name(axk::PlacementResolution resolution) {
    switch (resolution) {
    case axk::PlacementResolution::exact:
        return "EXACT";
    case axk::PlacementResolution::missing:
        return "MISSING";
    case axk::PlacementResolution::ambiguous:
        return "AMBIGUOUS";
    }
    return "MISSING";
}

Json source_json(const axk::FieldSource &source) {
    return {{"offsetBytes", source.offset},
            {"sizeBytes", source.size},
            {"verification", verification_name(source.verification)},
            {"basis", source.basis}};
}

template <typename T> Json field_json(const axk::FieldValue<T> &field) {
    return {{"value", field.value}, {"source", source_json(field.source)}};
}

Json placement_json(const axk::ObjectPlacement &placement) {
    return {{"partitionIndex", placement.partition.value},
            {"partitionName", placement.partition_name},
            {"volumeDirectoryId", placement.volume_directory.value},
            {"volumeName", placement.volume_name},
            {"categoryName", placement.category_name},
            {"entryName", placement.entry_name},
            {"containerDirectory", placement.container_directory}};
}

Json member_json(const axk::CurrentSbnkMember &member) {
    return {{"waveDataName", member.wave_data_name},
            {"cachedWaveDataReferenceValue", member.cached_wave_data_reference_value},
            {"rootKey", member.root_key},
            {"sampleRate", member.sample_rate},
            {"fineTuneCents", member.fine_tune_cents},
            {"pitchBaseWord", member.pitch_base_word},
            {"waveStartFrame", member.wave_start_frame},
            {"waveLengthFrames", member.wave_length_frames},
            {"loopStartFrame", member.loop_start_frame},
            {"loopLengthFrames", member.loop_length_frames}};
}

Json control_json(const axk::SbnkControlRecord &control) {
    return {
        {"device", control.device}, {"function", control.function}, {"type", control.type}, {"range", control.range}};
}

Json decoded_json(const axk::DecodedObject &object, Json &omissions) {
    if (const auto *wave_data = std::get_if<axk::CurrentSmpl>(&object.payload)) {
        omissions.push_back({{"kind", "AUDIO_PCM"},
                             {"sizeBytes", wave_data->stored_pcm_bytes},
                             {"reason", "Audio content is excluded from object metadata"}});
        return {{"kind", "SMPL"},
                {"sampleRate", field_json(wave_data->sample_rate)},
                {"storedSampleWidthBytes", field_json(wave_data->stored_sample_width_bytes)},
                {"embeddedContainerName", field_json(wave_data->embedded_container_name)},
                {"transientNameHashNextHandle", field_json(wave_data->transient_name_hash_next_handle)},
                {"pcmTransferControl", field_json(wave_data->pcm_transfer_control)},
                {"pcmTransferFormatSelector", wave_data->pcm_transfer_format_selector},
                {"transient512ByteBlockCounter", field_json(wave_data->transient_512_byte_block_counter)},
                {"waveDataReferenceValue", field_json(wave_data->wave_data_reference_value)},
                {"duplicateSampleRate", field_json(wave_data->duplicate_sample_rate)},
                {"rootKey", field_json(wave_data->root_key)},
                {"fineTuneCents", field_json(wave_data->fine_tune_cents)},
                {"loopMode", field_json(wave_data->loop_mode)},
                {"loopModeLabel", wave_data->loop_mode_label},
                {"waveStartFrame", field_json(wave_data->wave_start_frame)},
                {"waveLengthFrames", field_json(wave_data->wave_length_frames)},
                {"waveEndFrameExclusive", wave_data->wave_end_frame_exclusive},
                {"loopStartFrame", field_json(wave_data->loop_start_frame)},
                {"loopLengthFrames", field_json(wave_data->loop_length_frames)},
                {"loopEndFrameInclusive", wave_data->loop_end_frame_inclusive},
                {"loopEndFrameExclusive", wave_data->loop_end_frame_exclusive},
                {"storedPcmOffsetBytes", wave_data->stored_pcm_offset},
                {"storedPcmBytes", wave_data->stored_pcm_bytes},
                {"storedSegmentOffsetBytes", wave_data->stored_segment_offset},
                {"storedSegmentBytes", wave_data->stored_segment_bytes},
                {"storageState", wave_data->stored_segment_offset == 0U &&
                                         wave_data->stored_segment_bytes == wave_data->stored_pcm_bytes
                                     ? "COMPLETE"
                                     : "INCOMPLETE"},
                {"compactRecordHex", hex(wave_data->compact_record)}};
    }
    if (const auto *sample = std::get_if<axk::CurrentSbnk>(&object.payload)) {
        Json controls = Json::array();
        for (const auto &control : sample->control_records)
            controls.push_back(control_json(control));
        Json fields = Json::object();
        for (const auto &field : sample->numeric_fields) {
            fields[field.name] = {{"value", field.value ? Json(*field.value) : Json(nullptr)},
                                  {"source", source_json(field.source)}};
        }
        return {{"kind", "SBNK"},
                {"sampleName", sample->sample_name},
                {"instrumentName", sample->instrument_name},
                {"rightSlotPresent", sample->right_slot_present},
                {"rightLinkRole", sample->right_link_role},
                {"left", member_json(sample->left)},
                {"right", sample->right ? member_json(*sample->right) : Json(nullptr)},
                {"inactiveRight", member_json(sample->inactive_right)},
                {"linkedProgramBitmapWords", sample->linked_program_bitmap_words},
                {"linkedProgramNumbers", sample->linked_program_numbers},
                {"sampleFlags", sample->sample_flags},
                {"mapoutFlags", sample->mapout_flags},
                {"monoMode", sample->mono_mode},
                {"keyRangeHigh", sample->key_range_high},
                {"keyRangeLow", sample->key_range_low},
                {"sampleLevel", sample->sample_level},
                {"pan", sample->pan},
                {"velocityRangeHigh", sample->velocity_range_high},
                {"velocityRangeLow", sample->velocity_range_low},
                {"loopMode", sample->loop_mode},
                {"loopModeLabel", sample->loop_mode_label},
                {"controlRecordStorageOffset", sample->control_record_storage_offset},
                {"controlRecordTailCopyPresent", sample->control_record_tail_copy_present},
                {"controlRecordCopiesMatch",
                 sample->control_record_copies_match ? Json(*sample->control_record_copies_match) : Json(nullptr)},
                {"controlRecords", std::move(controls)},
                {"numericFields", std::move(fields)},
                {"rawParameterWindowHex", hex(sample->raw_parameter_window)}};
    }
    if (const auto *sample_bank = std::get_if<axk::CurrentSbac>(&object.payload)) {
        Json slots = Json::array();
        for (const auto &slot : sample_bank->slots)
            slots.push_back({{"name", slot.name}, {"rawHandle", slot.raw_handle}, {"offsetBytes", slot.offset}});
        return {{"kind", "SBAC"},
                {"rawSampleParameterBlockHex", hex(sample_bank->raw_sample_parameter_block)},
                {"valueEnableWords", sample_bank->value_enable_words},
                {"enabledParameterNumbers", sample_bank->enabled_parameter_numbers},
                {"enabledNumbersOutsideTable", sample_bank->enabled_numbers_outside_table},
                {"bulkAssignedSampleCount", sample_bank->bulk_assigned_sample_count},
                {"activeSlotCount", sample_bank->active_slot_count},
                {"maximumSlotCount", sample_bank->maximum_slot_count},
                {"slots", std::move(slots)}};
    }
    if (const auto *program = std::get_if<axk::CurrentProg>(&object.payload)) {
        Json controls = Json::array();
        for (const auto &control : program->control_records)
            controls.push_back(control_json(control));
        Json effects = Json::array();
        for (const auto &effect : program->effect_blocks)
            effects.push_back(hex(effect));
        Json assignments = Json::array();
        for (const auto &assignment : program->assignments) {
            assignments.push_back({{"name", assignment.name},
                                   {"rawHandle", assignment.raw_handle},
                                   {"kind", assignment.kind},
                                   {"flags", assignment.flags},
                                   {"levelOffset", assignment.level_offset},
                                   {"velocitySensitivity", assignment.velocity_sensitivity},
                                   {"panOffset", assignment.pan_offset},
                                   {"keyLimitHigh", assignment.key_limit_high},
                                   {"keyLimitLow", assignment.key_limit_low},
                                   {"velocityLimitHigh", assignment.velocity_limit_high},
                                   {"velocityLimitLow", assignment.velocity_limit_low},
                                   {"rawRowHex", hex(assignment.raw_row)}});
        }
        return {{"kind", "PROG"},
                {"programName", program->program_name},
                {"controlRecords", std::move(controls)},
                {"rawControlBlockHex", hex(program->raw_control_block)},
                {"rawControlTailCopyHex", hex(program->raw_control_tail_copy)},
                {"effectBlocksHex", std::move(effects)},
                {"assignments", std::move(assignments)}};
    }
    if (const auto *sequence = std::get_if<axk::CurrentSequence>(&object.payload)) {
        Json tempo_events = Json::array();
        for (const auto &event : sequence->tempo_events) {
            tempo_events.push_back(
                {{"tick", event.tick}, {"microsecondsPerQuarterNote", event.microseconds_per_quarter_note}});
        }
        omissions.push_back({{"kind", "SEQUENCE_PAYLOAD"},
                             {"sizeBytes", sequence->raw_payload.size()},
                             {"reason", "Sequence event content is excluded from object metadata"}});
        return {{"kind", "SEQU"},
                {"formatVersion", sequence->format_version},
                {"ticksPerQuarterNote", sequence->ticks_per_quarter_note},
                {"firstTick", sequence->first_tick},
                {"endTick", sequence->end_tick},
                {"eventCount", sequence->event_count},
                {"headerTempoBpm", sequence->header_tempo_bpm},
                {"effectiveInitialTempoMicrosecondsPerQuarterNote",
                 sequence->effective_initial_tempo_microseconds_per_quarter_note},
                {"tempoEvents", std::move(tempo_events)}};
    }
    const auto payload_size = std::visit(
        [](const auto &payload) -> std::size_t {
            if constexpr (requires { payload.raw_payload; })
                return payload.raw_payload.size();
            return 0U;
        },
        object.payload);
    omissions.push_back({{"kind", "OPAQUE_PAYLOAD"},
                         {"sizeBytes", payload_size},
                         {"reason", "Opaque object content is excluded from object metadata"}});
    return {{"kind", std::holds_alternative<axk::CurrentProfile>(object.payload) ? "PRF3" : "GENERIC"}};
}

Json header_json(const axk::ObjectHeader &header) {
    return {{"rawType", header.raw_type},
            {"name", header.name},
            {"headerSizeBytes", header.header_size},
            {"layoutSelector0x14", header.unknown_0x14},
            {"recordSizeOrHeaderUsed0x18", header.record_size_or_header_used},
            {"payloadBytes0x1c", header.payload_bytes_0x1c},
            {"payloadBytes0x20", header.payload_bytes_0x20},
            {"payloadOffset0x24", header.payload_offset_0x24},
            {"rawPrefixHex", hex(header.raw_prefix)}};
}

} // namespace

axk::app::Result<nlohmann::ordered_json> axk::app::ImageSessionManager::object_detail(std::string_view image_id,
                                                                                      std::string_view owner_id,
                                                                                      std::string_view object_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    const auto snapshot = (*session)->snapshots_by_id.find(std::string{object_id});
    const auto descriptor = (*session)->descriptors_by_id.find(std::string{object_id});
    if (snapshot == (*session)->snapshots_by_id.end() || descriptor == (*session)->descriptors_by_id.end())
        return std::unexpected(session_error("object_not_found", "image object does not exist"));

    const auto object_reference = [&](std::string_view id) -> Json {
        const auto referenced = (*session)->snapshots_by_id.find(std::string{id});
        if (referenced == (*session)->snapshots_by_id.end())
            return nullptr;
        return {{"id", id},
                {"key", referenced->second.key},
                {"type", referenced->second.object.header.raw_type},
                {"name", referenced->second.object.header.name}};
    };

    Json relationships = Json::array();
    for (const auto &relationship : (*session)->relationships) {
        Json roles = Json::array();
        if (relationship.source_object_id == object_id)
            roles.push_back("SOURCE");
        if (relationship.target_object_id && *relationship.target_object_id == object_id)
            roles.push_back("TARGET");
        if (std::ranges::contains(relationship.candidate_object_ids, object_id))
            roles.push_back("CANDIDATE");
        if (roles.empty())
            continue;
        Json candidates = Json::array();
        for (const auto &candidate : relationship.candidate_object_ids)
            candidates.push_back(object_reference(candidate));
        relationships.push_back(
            {{"id", relationship.id},
             {"selectedObjectRoles", std::move(roles)},
             {"sourceObject", object_reference(relationship.source_object_id)},
             {"targetObject",
              relationship.target_object_id ? object_reference(*relationship.target_object_id) : Json(nullptr)},
             {"candidateObjects", std::move(candidates)},
             {"type", relationship.type},
             {"quality", relationship.quality},
             {"basis", relationship.basis},
             {"notes", relationship.notes},
             {"assignmentIndex", relationship.assignment_index},
             {"assignmentName", relationship.assignment_name},
             {"assignmentState", relationship.assignment_state},
             {"receiveChannelDisplay", relationship.receive_channel_display}});
    }

    Json omissions = Json::array();
    Json candidates = Json::array();
    for (const auto &candidate : snapshot->second.placement_candidates)
        candidates.push_back(placement_json(candidate));
    const auto &media_descriptor = descriptor->second;
    Json object = {
        {"id", object_id},
        {"key", snapshot->second.key},
        {"type", snapshot->second.object.header.raw_type},
        {"name", snapshot->second.object.header.name},
        {"format", object_format_name(snapshot->second.object.format)},
        {"scopeKey", snapshot->second.scope_key},
        {"partitionIndex", snapshot->second.partition.value},
        {"sfsId", snapshot->second.sfs_id.value},
        {"storedSizeBytes", media_descriptor.size},
        {"placementResolution", wire_placement_resolution_name(snapshot->second.placement_resolution)},
        {"placement", snapshot->second.placement ? placement_json(*snapshot->second.placement) : Json(nullptr)},
        {"placementCandidates", std::move(candidates)},
        {"descriptor",
         {{"logicalPath", media_descriptor.logical_path},
          {"scopeKey", media_descriptor.scope_key},
          {"rawGroup", media_descriptor.raw_group},
          {"rawVolume", media_descriptor.raw_volume},
          {"groupLabel", media_descriptor.group_label.value},
          {"groupLabelStatus", media_descriptor.group_label.status},
          {"groupLabelBasis", media_descriptor.group_label.basis},
          {"volumeLabel", media_descriptor.volume_label.value},
          {"volumeLabelStatus", media_descriptor.volume_label.status},
          {"volumeLabelBasis", media_descriptor.volume_label.basis},
          {"dataOffsetBytes", media_descriptor.data_offset}}},
        {"header", header_json(snapshot->second.object.header)},
        {"decoded", decoded_json(snapshot->second.object, omissions)},
        {"omissions", std::move(omissions)}};
    return Json{{"schemaVersion", 1U},
                {"image", {{"imageId", image_id}, {"revision", (*session)->revision}, {"format", (*session)->format}}},
                {"object", std::move(object)},
                {"relationships", std::move(relationships)}};
}
