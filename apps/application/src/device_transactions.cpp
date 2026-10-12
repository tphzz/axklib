#include "device_sessions_internal.hpp"

#include <algorithm>
#include <format>
#include <system_error>

#include "alteration_journal_io.hpp"
#include "private_storage.hpp"

namespace axk::app {
namespace {
using namespace device_detail;

Result<std::vector<std::filesystem::path>> operation_directories(const std::filesystem::path &root) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    for (std::filesystem::directory_iterator at{root, error}, end; !error && at != end; at.increment(error)) {
        if (at->is_symlink(error) || !at->is_directory(error) || error)
            return std::unexpected(Error{"device_state_unavailable", "unexpected entry in device operation storage"});
        // Unpublished crash debris must not consume durable status capacity.
        bool published = std::filesystem::exists(at->path() / "operation.json", error);
        if (!published && !error) {
            for (std::filesystem::directory_iterator entry{at->path(), error}, last; !error && entry != last;
                 entry.increment(error))
                published = published || entry->path().extension() == ".axkjournal";
        }
        if (error)
            return std::unexpected(Error{"device_state_unavailable", "device operation storage cannot be inspected"});
        if (!published)
            continue;
        result.push_back(at->path());
        if (result.size() > 4096U)
            return std::unexpected(Error{"device_state_unavailable", "device operation retention limit exceeded"});
    }
    if (error)
        return std::unexpected(Error{"device_state_unavailable", "device operation storage cannot be enumerated"});
    return result;
}

Result<std::filesystem::path> operation_directory(const std::filesystem::path &root, std::string_view owner,
                                                  std::string_view operation) {
    if (owner.empty() || operation.empty() || operation.size() > 128U)
        return std::unexpected(Error{"device_operation_missing", "device operation is unavailable"});
    const auto key = std::format("{}:{}", owner, operation);
    return root / *digest(std::as_bytes(std::span{key.data(), key.size()}));
}

Result<Guards> load_guards(const std::filesystem::path &directory) {
    auto json = load_json(directory / "guards.json");
    if (!json)
        return std::unexpected(json.error());
    try {
        Guards result;
        const auto &entries = json->at("sectors");
        if (!entries.is_array() || entries.size() > maximum_guard_sectors)
            return std::unexpected(Error{"device_state_unavailable", "invalid device guard count"});
        for (const auto &entry : entries) {
            const auto offset = entry.at("offset").get<std::uint64_t>();
            const auto hash = entry.at("sha256").get<std::string>();
            if (hash.size() != 64U || !result.emplace(offset, hash).second)
                return std::unexpected(Error{"device_state_unavailable", "invalid or duplicate device guard"});
        }
        return result;
    } catch (const Json::exception &) {
        return std::unexpected(Error{"device_state_unavailable", "device guard record is malformed"});
    }
}

void discard_completed_journal(const std::filesystem::path &directory) {
    // Durable terminal status is published first; leftover files are harmless.
    std::error_code error;
    for (std::filesystem::directory_iterator at{directory, error}, end; !error && at != end; at.increment(error)) {
        const auto name = at->path().filename().string();
        if (at->is_regular_file(error) && !at->is_symlink(error) &&
            (name.ends_with(".axkjournal") || name.ends_with(".axkjournal.commit")))
            std::filesystem::remove(at->path(), error);
    }
}

} // namespace

Result<std::vector<DeviceOperationStatus>> DeviceSessionManager::operations(std::string_view owner) const {
    if (!enabled() || owner.empty())
        return std::unexpected(Error{"devices_unavailable", "device access is disabled for this server"});
    auto directories = operation_directories(implementation_->directory / "operations");
    if (!directories)
        return std::unexpected(directories.error());
    std::vector<DeviceOperationStatus> result;
    for (const auto &directory : *directories) {
        auto record = load_json(directory / "operation.json");
        if (!record) {
            // No write can start before the durable record and final journal exist.
            std::error_code error;
            bool journal_present{};
            for (std::filesystem::directory_iterator at{directory, error}, end; !error && at != end;
                 at.increment(error))
                journal_present = journal_present || at->path().extension() == ".axkjournal";
            if (!error && !journal_present && !std::filesystem::exists(directory / "operation.json", error) && !error)
                continue;
            return std::unexpected(record.error());
        }
        try {
            if (record->at("owner").get<std::string>() != owner)
                continue;
            result.push_back({record->at("operationId").get<std::string>(), record->at("state").get<std::string>(),
                              record->at("model").get<std::string>(), record->at("serial").get<std::string>(),
                              record->at("targetLabel").get<std::string>(),
                              record->at("targetSize").get<std::uint64_t>(),
                              record->at("identity").get<std::string>()});
        } catch (const Json::exception &) {
            return std::unexpected(Error{"device_state_unavailable", "device operation record is malformed"});
        }
    }
    return result;
}

Result<DeviceOperationStatus> DeviceSessionManager::resolve(std::string_view operation_id, std::string_view owner) {
    if (!enabled() || owner.empty())
        return std::unexpected(Error{"devices_unavailable", "device access is disabled for this server"});
    const std::scoped_lock admission{implementation_->operation_admission};
    auto directory = operation_directory(implementation_->directory / "operations", owner, operation_id);
    if (!directory)
        return std::unexpected(directory.error());
    std::error_code error;
    if (!std::filesystem::exists(*directory / "operation.json", error) && !error) {
        if (std::filesystem::exists(*directory, error) && !error) {
            for (std::filesystem::directory_iterator at{*directory, error}, end; !error && at != end;
                 at.increment(error))
                if (at->path().extension() == ".axkjournal")
                    return std::unexpected(
                        Error{"device_recovery_required", "orphan journal requires review; request cannot be cleared"});
            if (error)
                return std::unexpected(
                    Error{"device_state_unavailable", "device operation storage cannot be inspected"});
        }
        auto retained = operations(owner);
        if (!retained || retained->size() >= 4096U)
            return std::unexpected(
                Error{"device_operation_capacity", "device operation status retention is unavailable"});
        if (auto prepared = detail::prepare_private_directory(*directory); !prepared)
            return std::unexpected(prepared.error());
        const Json record{{"operationId", operation_id},
                          {"owner", owner},
                          {"state", "no_write"},
                          {"fenced", true},
                          {"model", ""},
                          {"serial", ""},
                          {"targetLabel", "Unstarted request"},
                          {"targetSize", 0U},
                          {"identity", ""}};
        if (auto saved = save_json(*directory / "operation.json", record, true); !saved)
            return std::unexpected(saved.error());
    }
    auto record = load_json(*directory / "operation.json");
    if (!record)
        return std::unexpected(record.error());
    try {
        if (record->at("owner").get<std::string>() != owner ||
            record->at("operationId").get<std::string>() != operation_id)
            return std::unexpected(Error{"device_operation_missing", "device operation is unavailable"});
        if (record->at("state") == "no_write") {
            (*record)["fenced"] = true;
            if (auto saved = save_json(*directory / "operation.json", *record, false); !saved)
                return std::unexpected(saved.error());
        }
        return DeviceOperationStatus{
            record->at("operationId").get<std::string>(), record->at("state").get<std::string>(),
            record->at("model").get<std::string>(),       record->at("serial").get<std::string>(),
            record->at("targetLabel").get<std::string>(), record->at("targetSize").get<std::uint64_t>(),
            record->at("identity").get<std::string>()};
    } catch (const Json::exception &) {
        return std::unexpected(Error{"device_state_unavailable", "device operation record is malformed"});
    }
}

Result<void> DeviceSessionManager::apply(
    const std::shared_ptr<MutationTarget> &target, std::span<const AlterationJournalPatch> patches,
    const CancellationToken &cancellation, const std::function<Result<void>()> &validate,
    const std::function<void()> &on_rollback_verified,
    const std::function<Result<void>(std::shared_ptr<const RandomAccessReader>)> &admit_frozen) {
    auto mutation = std::dynamic_pointer_cast<Mutation>(target);
    if (!mutation)
        return std::unexpected(Error{"device_mutation_invalid", "mutation does not belong to a device session"});
    if (auto bound = target->verify_bound(); !bound)
        return bound;
    // Patch originals are streamed into the complete durable journal and
    // compared against the target there. Only semantic planning/admission
    // reads consume the bounded metadata guard table.
    mutation->journal_streaming(true);
    struct PlanningMode {
        Mutation &mutation;
        ~PlanningMode() { mutation.journal_streaming(false); }
    } planning_mode{*mutation};
    auto aligned = align_patches(target, patches);
    if (!aligned)
        return std::unexpected(aligned.error());
    // The journal's entry check verifies all guards after alignment and before
    // publishing anything. Its later publication/admission checks remain fresh.
    Json entries = Json::array();
    for (const auto &[offset, hash] : mutation->guards)
        entries.push_back({{"offset", offset}, {"sha256", hash}});
    if (auto saved = save_json(mutation->directory / "guards.json", {{"sectors", std::move(entries)}}, true,
                               &mutation->connection->reads);
        !saved)
        return saved;
    auto encoded = journal_io::encoded_size(target->journal_identity(), target->stable_identity(), *aligned);
    std::error_code error;
    const auto capacity = std::filesystem::space(mutation->directory, error);
    if (!encoded || error || *encoded > capacity.available || capacity.available - *encoded < 16U * 1024U * 1024U)
        return std::unexpected(
            Error{"device_journal_capacity", "insufficient host space for the complete write journal"});
    {
        const std::scoped_lock admission{implementation_->operation_admission};
        auto record = load_json(mutation->directory / "operation.json");
        if (!record)
            return std::unexpected(record.error());
        if (record->value("fenced", false))
            return std::unexpected(
                Error{"device_operation_resolved", "request was closed by status review; no write is permitted"});
        if (auto saved = mutation->set_state("prepared"); !saved)
            return saved;
    }
    AlterationJournalStore journals{
        mutation->directory, default_maximum_alteration_journal_bytes, {}, 1024U * 1024U, true};
    bool rolled_back{};
    bool rollback_state_saved{};
    auto applied = journals.apply(
        target, target->size(), *aligned, cancellation, validate,
        [&] {
            rolled_back = true;
            rollback_state_saved = mutation->set_state("rolled_back").has_value();
            if (on_rollback_verified)
                on_rollback_verified();
        },
        [&](std::shared_ptr<const RandomAccessReader> reader) -> Result<void> {
            if (!admit_frozen)
                return {};
            mutation->journal_streaming(false);
            auto admitted = admit_frozen(std::move(reader));
            mutation->journal_streaming(true);
            return admitted;
        });
    if (!applied) {
        if (!journals.storage_ready() || (rolled_back && !rollback_state_saved)) {
            static_cast<void>(mutation->set_state("recovery_required"));
            mutation->connection->invalidated = true;
            mutation->connection->authorized_target.clear();
            return std::unexpected(
                Error{"device_recovery_required",
                      "device write requires recovery; retain the journal and check this operation identity"});
        }
        if (!rolled_back) {
            if (auto saved = mutation->set_state("no_write"); !saved)
                return saved;
        }
        if (rolled_back && rollback_state_saved)
            discard_completed_journal(mutation->directory);
        return applied;
    }
    if (auto saved = mutation->set_state("committed"); !saved) {
        mutation->connection->invalidated = true;
        mutation->connection->authorized_target.clear();
        return std::unexpected(
            Error{"device_recovery_required",
                  "write committed but durable status could not be saved; check status before retrying"});
    }
    discard_completed_journal(mutation->directory);
    return {};
}

Result<void> DeviceSessionManager::recover(std::string_view operation_id, std::string_view session_id,
                                           std::string_view target_id, std::string_view owner, bool acknowledged) {
    if (!writes_supported() || !acknowledged)
        return std::unexpected(
            Error{"device_write_authorization_required", "explicit recovery authorization is required"});
    auto connection = implementation_->owned(session_id, owner);
    if (!connection)
        return std::unexpected(connection.error());
    const std::scoped_lock lock{(*connection)->io};
    if ((*connection)->mutation_active)
        return std::unexpected(Error{"device_busy", "a device mutation is still active"});
    const auto *description = find_target(**connection, target_id);
    if (!description || description->read_only)
        return std::unexpected(Error{"device_read_only", "selected target cannot be recovered"});
    auto directory = operation_directory(implementation_->directory / "operations", owner, operation_id);
    if (!directory)
        return std::unexpected(directory.error());
    auto record = load_json(*directory / "operation.json");
    if (!record)
        return std::unexpected(record.error());
    try {
        if (record->at("owner").get<std::string>() != owner ||
            record->at("operationId").get<std::string>() != operation_id ||
            record->at("identity").get<std::string>() != target_identity(**connection, *description))
            return std::unexpected(
                Error{"device_recovery_identity", "selected device target does not match the recovery record"});
        const auto state = record->at("state").get<std::string>();
        if (state != "prepared" && state != "recovery_required")
            return std::unexpected(
                Error{"device_operation_terminal", "this operation is complete; recovery must not replay it"});
    } catch (const Json::exception &) {
        return std::unexpected(Error{"device_state_unavailable", "device recovery record is malformed"});
    }
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    for (std::filesystem::directory_iterator at{*directory, error}, end; !error && at != end; at.increment(error)) {
        if (at->path().extension() == ".axkjournal" && at->is_regular_file(error) && !at->is_symlink(error))
            paths.push_back(at->path());
    }
    if (!error && paths.empty() && record->value("state", "") == "prepared") {
        (*record)["state"] = "no_write";
        return save_json(*directory / "operation.json", *record, false);
    }
    if (error || paths.size() != 1U)
        return std::unexpected(Error{"device_recovery_required", "a unique complete device journal is required"});
    auto guards = load_guards(*directory);
    if (!guards)
        return std::unexpected(guards.error());
    auto journal = journal_io::inspect(paths.front(), default_maximum_alteration_journal_bytes, 1024U * 1024U);
    if (!journal || journal->target.kind != MutationSourceKind::device ||
        journal->target_identity != target_identity(**connection, *description) ||
        journal->image_size_bytes != description->size_bytes ||
        std::ranges::any_of(journal->patches, [&](const auto &patch) {
            return patch.target_offset % (*connection)->device.transport->sector_size() != 0U ||
                   patch.size % (*connection)->device.transport->sector_size() != 0U;
        }))
        return std::unexpected(Error{"device_recovery_identity", "device journal or target identity is invalid"});
    auto frozen = FileReader::open(paths.front());
    if (!frozen)
        return std::unexpected(Error{"device_recovery_required", frozen.error().message});
    std::vector<AlterationJournalPatch> excluded;
    for (const auto &patch : journal->patches)
        excluded.push_back({patch.target_offset,
                            {*frozen, patch.original_file_offset, patch.size},
                            {*frozen, patch.replacement_file_offset, patch.size}});
    if (auto verified = device_detail::verify_bound(**connection); !verified)
        return verified;
    if (auto verified = verify_guards(**connection, *description, *guards, excluded); !verified)
        return verified;
    if (auto authorized = (*connection)->device.transport->authorize_writes(); !authorized)
        return authorized;
    struct InvalidateAfterRecovery {
        Connection &connection;
        ~InvalidateAfterRecovery() {
            connection.authorized_target.clear();
            connection.metadata_guards.clear();
            connection.invalidated = true;
        }
    } invalidate{**connection};
    if (auto verified = verify_guards(**connection, *description, *guards, excluded); !verified)
        return verified;
    Mutation mutation{*connection, *description, std::string{operation_id}, *directory, true};
    auto committed = journal_io::commit_marker_matches(paths.front(), journal->file_checksum);
    if (!committed)
        return std::unexpected(committed.error());
    if (*committed) {
        if (auto compared = journal_io::compare_target(mutation, paths.front(), *journal, true, 1024U * 1024U);
            !compared)
            return compared;
    } else {
        if (auto recognized =
                journal_io::recognize_uncommitted_target(mutation, paths.front(), *journal, 1024U * 1024U);
            !recognized)
            return recognized;
        if (auto restored = journal_io::restore_original_bytes(mutation, paths.front(), *journal, 1024U * 1024U);
            !restored)
            return std::unexpected(Error{"device_recovery_required", restored.error().message});
    }
    if (auto saved = mutation.set_state(*committed ? "committed" : "rolled_back"); !saved)
        return saved;
    frozen->reset();
    excluded.clear();
    discard_completed_journal(*directory);
    ++(*connection)->revision;
    // Reopening is explicit: existing decoded sessions must never resume after recovery.
    return {};
}
} // namespace axk::app
