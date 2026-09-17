#include "axklib/application/session_tx16w_operations.hpp"

#include <charconv>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/tx16w.hpp"
#include "axklib/tx16w_a_series.hpp"

namespace {

using Json = nlohmann::json;

struct ResolvedDisk {
    std::shared_ptr<const axk::RandomAccessReader> reader;
    std::string filename;
    std::optional<axk::app::UploadLease> lease;
};

axk::app::Error operation_error(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

axk::app::Error core_error(const axk::Error &error) {
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "tx16w_inspection_failed",
            error.message};
}

axk::app::Result<ResolvedDisk> resolve_disk(const Json &source, std::string_view owner_id,
                                            const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads) {
    try {
        const auto has_file = source.contains("fileRef");
        const auto has_upload = source.contains("uploadRef");
        if (has_file == has_upload) {
            return std::unexpected(
                operation_error("invalid_request", "source must contain exactly one of fileRef or uploadRef"));
        }
        if (has_file) {
            const auto &reference = source.at("fileRef");
            auto opened = sandbox.open_file(
                {reference.at("rootId").get<std::string>(), reference.at("relativePath").get<std::string>()});
            if (!opened)
                return std::unexpected(opened.error());
            return ResolvedDisk{std::move(opened->reader), std::move(opened->filename), std::nullopt};
        }
        const axk::app::UploadRef reference{source.at("uploadRef").at("uploadId").get<std::string>()};
        auto snapshot = uploads.inspect(reference, owner_id);
        if (!snapshot)
            return std::unexpected(snapshot.error());
        if (snapshot->kind != axk::app::UploadKind::disk_image) {
            return std::unexpected(
                operation_error("upload_kind_mismatch", "upload is not an admitted TX16W disk image"));
        }
        auto lease = uploads.lease(reference, owner_id);
        if (!lease)
            return std::unexpected(lease.error());
        auto reader = axk::FileReader::open(lease->path());
        if (!reader)
            return std::unexpected(operation_error("tx16w_inspection_failed", reader.error().message));
        return ResolvedDisk{std::move(*reader), snapshot->filename, std::move(*lease)};
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "disk image source reference is malformed"));
    }
}

axk::app::Result<std::vector<ResolvedDisk>> resolve_disks(const Json &sources, std::string_view owner_id,
                                                          const axk::app::Sandbox &sandbox,
                                                          axk::app::UploadStore &uploads) {
    if (!sources.is_array() || sources.empty() || sources.size() > 32U)
        return std::unexpected(operation_error("invalid_request", "sources must contain 1..32 disk images"));
    std::vector<ResolvedDisk> result;
    result.reserve(sources.size());
    for (const auto &source : sources) {
        auto resolved = resolve_disk(source, owner_id, sandbox, uploads);
        if (!resolved)
            return std::unexpected{resolved.error()};
        result.push_back(std::move(*resolved));
    }
    return result;
}

axk::app::Result<axk::tx16w::Inspection> inspect_disks(std::span<const ResolvedDisk> disks,
                                                       const axk::CancellationToken &cancellation) {
    std::vector<axk::tx16w::SourceFile> files;
    for (const auto &disk : disks) {
        auto fat = axk::FatImage::open(disk.reader, disk.filename, cancellation);
        if (!fat)
            return std::unexpected(core_error(fat.error()));
        for (const auto &entry : fat->files()) {
            auto bytes = fat->read_file(entry, cancellation);
            if (!bytes)
                return std::unexpected(core_error(bytes.error()));
            files.push_back({entry.path, std::move(*bytes), disk.filename});
        }
    }
    auto inspection = axk::tx16w::inspect_files(files, cancellation);
    if (!inspection)
        return std::unexpected(core_error(inspection.error()));
    return std::move(*inspection);
}

axk::app::Result<axk::tx16w::a_series::TargetInventory> target_inventory(const axk::app::ImageSessionRead &session,
                                                                         std::uint8_t partition_index,
                                                                         std::string_view volume_name) {
    const auto volume_exists = std::ranges::any_of(session.volume_scopes_by_id, [&](const auto &entry) {
        return entry.second.partition_index == partition_index && entry.second.display_name == volume_name;
    });
    if (!volume_exists)
        return std::unexpected(operation_error("volume_scope_invalid", "target volume does not exist"));

    axk::tx16w::a_series::TargetInventory result;
    for (const auto *object : session.catalog_objects) {
        if (!object->placement || object->placement->partition.value != partition_index ||
            object->placement->volume_name != volume_name) {
            continue;
        }
        switch (object->object.header.type) {
        case axk::ObjectType::smpl:
            result.wave_data_names.push_back(object->placement->entry_name);
            break;
        case axk::ObjectType::sbnk:
            result.sample_names.push_back(object->placement->entry_name);
            break;
        case axk::ObjectType::sbac:
            result.sample_bank_names.push_back(object->placement->entry_name);
            break;
        case axk::ObjectType::prog: {
            std::uint16_t slot{};
            const auto &name = object->placement->entry_name;
            const auto parsed = std::from_chars(name.data(), name.data() + name.size(), slot);
            if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || slot == 0U || slot > 128U) {
                return std::unexpected(
                    operation_error("target_inventory_invalid", "target contains an invalid Program slot"));
            }
            result.occupied_program_slots.push_back(static_cast<std::uint8_t>(slot));
            const auto *program = std::get_if<axk::CurrentProg>(&object->object.payload);
            if (program == nullptr)
                return std::unexpected(operation_error("target_inventory_invalid", "target Program is unreadable"));
            result.program_names.push_back(program->program_name);
            break;
        }
        default:
            break;
        }
    }
    return result;
}

std::string_view profile_name(axk::tx16w::Profile profile) {
    switch (profile) {
    case axk::tx16w::Profile::yamaha_native:
        return "YAMAHA_NATIVE";
    case axk::tx16w::Profile::yamaha_native_with_auxiliary_files:
        return "YAMAHA_NATIVE_WITH_AUXILIARY";
    }
    return "YAMAHA_NATIVE";
}

std::string_view disposition_name(axk::tx16w::a_series::MappingDisposition disposition) {
    switch (disposition) {
    case axk::tx16w::a_series::MappingDisposition::exact:
        return "EXACT";
    case axk::tx16w::a_series::MappingDisposition::approximated:
        return "APPROXIMATED";
    case axk::tx16w::a_series::MappingDisposition::defaulted:
        return "DEFAULTED";
    case axk::tx16w::a_series::MappingDisposition::omitted:
        return "OMITTED";
    case axk::tx16w::a_series::MappingDisposition::blocked:
        return "BLOCKED";
    }
    return "BLOCKED";
}

Json plan_json(const axk::tx16w::Inspection &inspection, const axk::tx16w::a_series::ImportPlan &plan,
               std::span<const ResolvedDisk> disks, axk::tx16w::ImportMode mode, std::uint8_t partition_index,
               std::string_view volume_name) {
    Json wave_data = Json::array();
    for (const auto &item : plan.wave_data)
        wave_data.push_back({{"name", item.name}, {"targetSampleRate", item.target_sample_rate}});
    Json samples = Json::array();
    for (const auto &item : plan.samples) {
        samples.push_back({{"name", item.name},
                           {"waveDataName", item.waveform_id.value_or("")},
                           {"rootKey", item.parameters.root_key.value_or(60U)},
                           {"keyLow", item.parameters.key_low.value_or(0U)},
                           {"keyHigh", item.parameters.key_high.value_or(127U)}});
    }
    Json sample_banks = Json::array();
    for (const auto &item : plan.sample_banks)
        sample_banks.push_back({{"name", item.name}, {"sampleNames", item.member_samples}});
    Json programs = Json::array();
    for (const auto &item : plan.programs) {
        Json targets = Json::array();
        for (const auto &assignment : item.assignments)
            targets.push_back({{"kind", assignment.target_kind}, {"name", assignment.target_name}});
        programs.push_back({{"slot", item.number}, {"name", item.name}, {"assignments", std::move(targets)}});
    }
    Json notices = Json::array();
    for (const auto &item : plan.notices) {
        notices.push_back({{"disposition", disposition_name(item.disposition)},
                           {"sourceObject", item.source_object},
                           {"sourceParameter", item.source_parameter},
                           {"targetObject", item.target_object},
                           {"targetParameter", item.target_parameter},
                           {"message", item.message}});
    }
    const auto valid = std::ranges::none_of(plan.notices, [](const auto &notice) {
        return notice.disposition == axk::tx16w::a_series::MappingDisposition::blocked;
    });
    Json source_members = Json::array();
    for (const auto &disk : disks)
        source_members.push_back(disk.filename);
    return {{"schemaVersion", "1.0"},
            {"sourceMembers", std::move(source_members)},
            {"importMode", mode == axk::tx16w::ImportMode::hierarchy ? "HIERARCHY" : "WAVE_DATA_ONLY"},
            {"profile", profile_name(inspection.profile)},
            {"target", {{"partitionIndex", partition_index}, {"volumeName", volume_name}}},
            {"valid", valid},
            {"counts",
             {{"waveData", plan.wave_data.size()},
              {"samples", plan.samples.size()},
              {"sampleBanks", plan.sample_banks.size()},
              {"programs", plan.programs.size()}}},
            {"objects",
             {{"waveData", std::move(wave_data)},
              {"samples", std::move(samples)},
              {"sampleBanks", std::move(sample_banks)},
              {"programs", std::move(programs)}}},
            {"notices", std::move(notices)}};
}

} // namespace

axk::app::Result<void> axk::app::bind_session_tx16w_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                               UploadStore &uploads, ImageSessionManager &images) {
    return registry.bind(
        "images.tx16w.inspect",
        [&sandbox, &uploads, &images](const Json &input, const OperationContext &context) -> Result<Json> {
            try {
                const auto image_id = input.at("imageId").get<std::string>();
                const auto revision = input.at("expectedRevision").get<std::uint64_t>();
                const auto &target = input.at("target");
                const auto partition_index = target.at("partitionIndex").get<std::uint8_t>();
                const auto volume_name = target.at("volumeName").get<std::string>();
                auto session = images.begin_read(image_id, context.owner_id, revision);
                if (!session)
                    return std::unexpected(session.error());
                if (session->media == nullptr || session->media->kind() != axk::MediaKind::sfs) {
                    return std::unexpected(operation_error("image_mutation_unsupported",
                                                           "TX16W import requires a writable SFS hard-disk image"));
                }
                auto inventory = target_inventory(*session, partition_index, volume_name);
                if (!inventory)
                    return std::unexpected(inventory.error());
                auto disks = resolve_disks(input.at("sources"), context.owner_id, sandbox, uploads);
                if (!disks)
                    return std::unexpected(disks.error());
                const auto mode_name = input.at("importMode").get<std::string>();
                const auto mode = mode_name == "HIERARCHY" ? axk::tx16w::ImportMode::hierarchy
                                                           : axk::tx16w::ImportMode::wave_data_only;
                auto inspection = inspect_disks(*disks, context.cancellation);
                if (!inspection)
                    return std::unexpected(inspection.error());
                auto plan = axk::tx16w::a_series::plan_import(*inspection, *inventory, mode);
                if (!plan)
                    return std::unexpected(core_error(plan.error()));
                return plan_json(*inspection, *plan, *disks, mode, partition_index, volume_name);
            } catch (const Json::exception &) {
                return std::unexpected(operation_error("invalid_request", "TX16W import inspection is malformed"));
            }
        });
}
