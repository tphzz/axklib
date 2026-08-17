#include "image_sessions_internal.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <string_view>
#include <variant>

#include "axklib/system_file.hpp"

namespace axk::app {
namespace {

constexpr std::array system_file_kinds{axk::SystemFileKind::a3000_system, axk::SystemFileKind::a4000_a5000_system2};

SystemProgramContextFile context_file(axk::SystemFileKind kind) {
    return kind == axk::SystemFileKind::a3000_system ? SystemProgramContextFile::system
                                                     : SystemProgramContextFile::system2;
}

std::string_view file_name(axk::SystemFileKind kind) {
    return kind == axk::SystemFileKind::a3000_system ? "SYSTEM" : "SYSTEM2";
}

ImageSystemProgramContext unavailable(axk::SystemFileKind kind, SystemProgramContextAvailability availability,
                                      std::string message) {
    ImageSystemProgramContext result;
    result.file_kind = context_file(kind);
    result.availability = availability;
    result.message = std::move(message);
    return result;
}

std::string_view port_name(axk::MidiPort port) { return port == axk::MidiPort::a ? "A" : "B"; }

ImageSystemMidiAddress midi_address(const axk::SystemMidiAddress &address, bool show_port) {
    const auto port = port_name(address.port);
    return {.port = std::string{port},
            .channel = address.channel,
            .display =
                show_port ? std::format("{}{:02}", port, address.channel) : std::format("{:02}", address.channel)};
}

std::string part_label(const axk::SystemProgramPart &part, bool show_port) {
    return midi_address(part.midi, show_port).display;
}

ImageSystemProgramContext available(const axk::DecodedSystemFile &decoded) {
    ImageSystemProgramContext result;
    result.file_kind = context_file(decoded.kind);
    result.availability = SystemProgramContextAvailability::available;
    const auto show_port = decoded.model == axk::ASeriesModel::a5000;
    result.model = decoded.model == axk::ASeriesModel::a3000   ? "A3000"
                   : decoded.model == axk::ASeriesModel::a4000 ? "A4000"
                                                               : "A5000";

    if (const auto *context = std::get_if<axk::A3000SystemContext>(&decoded.context)) {
        result.basic_receive = midi_address(context->basic_receive, false);
        result.omni = context->omni;
        result.program_change_enabled = context->program_change_enabled;
        return result;
    }

    const auto &context = std::get<axk::A4000A5000SystemContext>(decoded.context);
    result.saved_program_mode = context.saved_program_mode == axk::ProgramMode::single ? "SINGLE" : "MULTI";
    result.basic_receive = midi_address(context.basic_receive, show_port);
    result.omni = context.omni;
    result.program_change_enabled = context.program_change_enabled;
    result.parts.reserve(context.parts.size());
    for (const auto &part : context.parts) {
        result.parts.push_back({.part_number = part.part_number,
                                .part_label = part_label(part, show_port),
                                .midi = midi_address(part.midi, show_port),
                                .program_number = part.program_number,
                                .master = part.master});
    }
    return result;
}

} // namespace

Result<ImageSystemProgramContexts> ImageSessionManager::system_program_contexts(std::string_view image_id,
                                                                                std::string_view owner_id,
                                                                                std::uint8_t partition_index) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    if (!(*session)->media) {
        return std::unexpected(
            image_sessions_internal::session_error("image_media_unavailable", "image session media is unavailable"));
    }
    const auto *container = std::get_if<axk::Container>(&(*session)->media->storage());
    if (container == nullptr) {
        return ImageSystemProgramContexts{.partition_index = partition_index,
                                          .files = {},
                                          .message = "System File decoding is not supported for this media format."};
    }

    const auto partition =
        std::ranges::find(container->partitions(), axk::PartitionIndex{partition_index}, &axk::Partition::index);
    if (partition == container->partitions().end()) {
        return std::unexpected(image_sessions_internal::session_error(
            "partition_not_found", std::format("partition {} does not exist", partition_index)));
    }

    ImageSystemProgramContexts result{.partition_index = partition_index, .files = {}, .message = {}};
    result.files.reserve(system_file_kinds.size());
    for (const auto kind : system_file_kinds) {
        const auto record_id = axk::locate_system_file_record(*partition, kind);
        if (!record_id) {
            result.files.push_back(unavailable(
                kind, SystemProgramContextAvailability::invalid,
                std::format("The partition's saved {} file is invalid and was not used.", file_name(kind))));
            continue;
        }
        if (!*record_id) {
            auto message = std::format("No saved {} file exists for partition {}.", file_name(kind), partition_index);
            if (kind == axk::SystemFileKind::a4000_a5000_system2)
                message += " Multi assignments cannot be derived from this partition.";
            result.files.push_back(
                unavailable(kind, SystemProgramContextAvailability::not_present, std::move(message)));
            continue;
        }

        const auto record = std::ranges::find(partition->records, **record_id, &axk::IndexRecord::sfs_id);
        const auto file_size = axk::system_file_record_size(kind);
        if (record == partition->records.end() || record->data_size != file_size) {
            result.files.push_back(unavailable(
                kind, SystemProgramContextAvailability::invalid,
                std::format("The partition's saved {} file is invalid and was not used.", file_name(kind))));
            continue;
        }
        const auto bytes = container->read_record_data(partition->index, **record_id, file_size);
        if (!bytes) {
            auto context = image_sessions_internal::core_error(bytes.error(), (*session)->source).context;
            return std::unexpected{Error{"system_file_read_failed",
                                         std::format("could not read the partition's saved {} file", file_name(kind)),
                                         std::move(context), true}};
        }
        const auto decoded = axk::decode_system_file(kind, *bytes);
        if (!decoded) {
            result.files.push_back(unavailable(
                kind, SystemProgramContextAvailability::invalid,
                std::format("The partition's saved {} file is invalid and was not used.", file_name(kind))));
            continue;
        }
        result.files.push_back(available(*decoded));
    }
    return result;
}

} // namespace axk::app
