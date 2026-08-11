#include "axklib/application/write_operations.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/alteration.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/version.hpp"
#include <nlohmann/json.hpp>

#include "content_digest.hpp"

#include "write_operations_internal.hpp"

namespace axk::app::write_operations_internal {

using axk::app::detail::file_sha256;
using axk::app::detail::reader_sha256;

TemporaryDirectoryCleanup::TemporaryDirectoryCleanup(std::filesystem::path path) : path_{std::move(path)} {}

TemporaryDirectoryCleanup::~TemporaryDirectoryCleanup() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

WritePlanClaim::WritePlanClaim(std::shared_ptr<WriteOperationState> state, std::string token,
                               std::shared_ptr<WritePlanRecord> record)
    : state_(std::move(state)), token_(std::move(token)), record_(std::move(record)) {}

WritePlanClaim::~WritePlanClaim() { release(); }

WritePlanClaim::WritePlanClaim(WritePlanClaim &&other) noexcept
    : state_(std::move(other.state_)), token_(std::move(other.token_)), record_(std::move(other.record_)),
      active_(std::exchange(other.active_, false)) {}

const std::shared_ptr<WritePlanRecord> &WritePlanClaim::record() const noexcept { return record_; }

void WritePlanClaim::consume() {
    if (!active_)
        return;
    std::lock_guard lock{state_->mutex};
    const auto reservation = normalized_path(record_->output_path);
    if (const auto found = state_->destination_reservations.find(reservation);
        found != state_->destination_reservations.end() && found->second == token_) {
        state_->destination_reservations.erase(found);
    }
    state_->plans.erase(token_);
    active_ = false;
}

void WritePlanClaim::release() {
    if (!active_)
        return;
    std::lock_guard lock{state_->mutex};
    if (const auto found = state_->plans.find(token_); found != state_->plans.end())
        found->second->claimed = false;
    active_ = false;
}

axk::app::Error operation_error(std::string code, std::string message, std::optional<std::string> relative_path) {
    axk::app::ErrorContext context;
    context.relative_path = std::move(relative_path);
    return {std::move(code), std::move(message), std::move(context)};
}

axk::app::Error core_error(const axk::Error &error, std::optional<std::string> relative_path) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    context.relative_path = std::move(relative_path);
    const auto code = error.code == axk::ErrorCode::operation_cancelled   ? "operation_cancelled"
                      : error.code == axk::ErrorCode::io_unsupported_size ? "image_build_too_large"
                                                                          : "image_operation_failed";
    return {code, error.message, std::move(context)};
}

std::string write_plan_kind_name(WritePlanKind kind) {
    switch (kind) {
    case WritePlanKind::hds:
        return "HDS";
    case WritePlanKind::floppy:
        return "FLOPPY";
    case WritePlanKind::iso:
        return "ISO";
    }
    return "HDS";
}

WritePlanKind write_plan_kind(axk::BuildManifestKind kind) {
    switch (kind) {
    case axk::BuildManifestKind::hds:
        return WritePlanKind::hds;
    case axk::BuildManifestKind::fat12_floppy:
        return WritePlanKind::floppy;
    case axk::BuildManifestKind::iso9660:
        return WritePlanKind::iso;
    }
    return WritePlanKind::hds;
}

std::string_view hds_creation_profile_wire_id(axk::HdsCreationProfileId id) {
    switch (id) {
    case axk::HdsCreationProfileId::floppy_scale:
        return "FLOPPY_SCALE";
    case axk::HdsCreationProfileId::cd_r_650:
        return "CD_R_650";
    case axk::HdsCreationProfileId::cd_r_700:
        return "CD_R_700";
    case axk::HdsCreationProfileId::hds_1_gib:
        return "HDS_1_GIB";
    case axk::HdsCreationProfileId::hds_2_gib:
        return "HDS_2_GIB";
    }
    return {};
}

std::optional<axk::HdsCreationProfileId> parse_hds_creation_profile_wire_id(std::string_view id) {
    for (const auto &profile : axk::hds_creation_profiles()) {
        if (hds_creation_profile_wire_id(profile.id) == id)
            return profile.id;
    }
    return std::nullopt;
}

std::string normalized_path(const std::filesystem::path &path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return (error ? path.lexically_normal() : canonical).generic_string();
}

void cleanup_plans(WriteOperationState &state, Clock::time_point now) {
    for (auto current = state.plans.begin(); current != state.plans.end();) {
        if (!current->second->claimed && current->second->expires_at <= now) {
            const auto reservation = normalized_path(current->second->output_path);
            if (const auto found = state.destination_reservations.find(reservation);
                found != state.destination_reservations.end() && found->second == current->first) {
                state.destination_reservations.erase(found);
            }
            current = state.plans.erase(current);
        } else {
            ++current;
        }
    }
}

axk::app::Result<WritePlanClaim> claim_plan(const std::shared_ptr<WriteOperationState> &state, std::string_view token,
                                            std::string_view owner_id, WritePlanKind expected_kind) {
    std::lock_guard lock{state->mutex};
    cleanup_plans(*state, Clock::now());
    const auto found = state->plans.find(std::string{token});
    if (found == state->plans.end() || found->second->owner_id != owner_id) {
        return std::unexpected(operation_error("write_plan_not_found", "write plan is absent or expired"));
    }
    if (found->second->kind != expected_kind) {
        return std::unexpected(operation_error("write_plan_kind_mismatch", "write plan has the wrong operation kind"));
    }
    if (found->second->claimed)
        return std::unexpected(operation_error("write_plan_in_use", "write plan is already being applied"));
    found->second->claimed = true;
    return WritePlanClaim{state, std::string{token}, found->second};
}

axk::app::Result<axk::app::FileRef> parse_file_ref(const Json &input, std::string_view field) {
    try {
        const auto &value = input.at(field);
        return axk::app::FileRef{value.at("rootId").get<std::string>(), value.at("relativePath").get<std::string>()};
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", std::string{field} + " must be a FileRef"));
    }
}

Json file_ref_json(const axk::app::FileRef &reference) {
    return {{"rootId", reference.root_id}, {"relativePath", reference.relative_path}};
}

axk::app::Result<std::string> read_text(const std::filesystem::path &path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 8U * 1024U * 1024U)
        return std::unexpected(operation_error("manifest_size", "manifest is absent or exceeds 8 MiB"));
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return std::unexpected(operation_error("manifest_read_failed", "could not open manifest"));
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

axk::app::Result<void> write_reader(const std::filesystem::path &path, const axk::RandomAccessReader &reader) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(operation_error("input_read_failed", "could not create retained input staging"));
    std::vector<std::byte> buffer(
        static_cast<std::size_t>(std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, reader.size()))));
    for (std::uint64_t offset = 0U; offset < reader.size();) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), reader.size() - offset));
        if (const auto read = reader.read_exact_at(offset, std::span{buffer}.first(count)); !read)
            return std::unexpected(core_error(read.error()));
        output.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(count));
        if (!output)
            return std::unexpected(operation_error("input_read_failed", "could not write retained input staging"));
        offset += count;
    }
    output.flush();
    if (!output)
        return std::unexpected(operation_error("input_read_failed", "could not flush retained input staging"));
    return {};
}

axk::app::Result<ResolvedInput> resolve_input(const Json &input, std::string_view owner_id,
                                              const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads,
                                              bool require_manifest) {
    try {
        if (input.contains("fileRef") && !input.contains("uploadRef")) {
            const auto &value = input.at("fileRef");
            axk::app::FileRef reference{value.at("rootId").get<std::string>(),
                                        value.at("relativePath").get<std::string>()};
            auto file = sandbox.open_file(reference);
            if (!file)
                return std::unexpected(file.error());
            auto staging = sandbox.create_staging_directory("axklib-write-input");
            if (!staging)
                return std::unexpected(staging.error());
            auto cleanup = std::make_shared<TemporaryDirectoryCleanup>(*staging);
            const auto path = *staging / file->filename;
            if (auto written = write_reader(path, *file->reader); !written)
                return std::unexpected(written.error());
            return ResolvedInput{path, std::nullopt, std::move(cleanup)};
        }
        if (input.contains("uploadRef") && !input.contains("fileRef")) {
            const axk::app::UploadRef reference{input.at("uploadRef").at("uploadId").get<std::string>()};
            auto snapshot = uploads.inspect(reference, owner_id);
            if (!snapshot)
                return std::unexpected(snapshot.error());
            if (require_manifest && snapshot->kind != axk::app::UploadKind::manifest) {
                return std::unexpected(operation_error("upload_kind_mismatch", "upload is not a manifest"));
            }
            if (!require_manifest && snapshot->kind == axk::app::UploadKind::manifest) {
                return std::unexpected(operation_error("upload_kind_mismatch", "manifest upload cannot bind input"));
            }
            auto lease = uploads.lease(reference, owner_id);
            if (!lease)
                return std::unexpected(lease.error());
            auto path = lease->path();
            return ResolvedInput{std::move(path), std::optional<axk::app::UploadLease>{std::move(*lease)}, nullptr};
        }
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "input reference is malformed"));
    }
    return std::unexpected(
        operation_error("invalid_request", "input must contain exactly one of fileRef or uploadRef"));
}

void replace_logical_path(Json &value, std::string_view logical, const std::string &physical) {
    if (value.is_string()) {
        if (value.get_ref<const std::string &>() == logical)
            value = physical;
        return;
    }
    if (value.is_array() || value.is_object()) {
        for (auto &item : value)
            replace_logical_path(item, logical, physical);
    }
}

std::optional<axk::app::FileRef> persistent_file_ref(const Json &input) {
    try {
        if (!input.is_object() || !input.contains("fileRef") || input.contains("uploadRef"))
            return std::nullopt;
        const auto &reference = input.at("fileRef");
        return axk::app::FileRef{reference.at("rootId").get<std::string>(),
                                 reference.at("relativePath").get<std::string>()};
    } catch (const Json::exception &) {
        return std::nullopt;
    }
}

axk::app::Result<ManifestDocument> load_manifest(const Json &input, const axk::app::OperationContext &context,
                                                 const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads) {
    ManifestDocument result;
    try {
        const auto &manifest = input.at("manifest");
        if (manifest.contains("inline") && !manifest.contains("fileRef") && !manifest.contains("uploadRef")) {
            result.json = manifest.at("inline");
        } else {
            const auto persistent_reference = persistent_file_ref(manifest);
            if (persistent_reference)
                result.file_inputs.push_back(*persistent_reference);
            auto resolved = resolve_input(manifest, context.owner_id, sandbox, uploads, true);
            if (!resolved)
                return std::unexpected(resolved.error());
            auto text = read_text(resolved->path);
            if (!text)
                return std::unexpected(text.error());
            result.json = Json::parse(*text);
            result.observed_paths.push_back(resolved->path);
            if (persistent_reference) {
                auto digest = file_sha256(resolved->path, context.cancellation);
                if (!digest)
                    return std::unexpected(digest.error());
                result.file_input_sha256.push_back(std::move(*digest));
            }
            if (resolved->lease)
                result.leases.push_back(std::move(*resolved->lease));
            if (resolved->staging)
                result.staging.push_back(std::move(resolved->staging));
        }
        if (!result.json.is_object())
            return std::unexpected(operation_error("invalid_request", "manifest JSON must be an object"));
        if (input.contains("inputBindings")) {
            if (!input.at("inputBindings").is_array() || input.at("inputBindings").size() > 1024U) {
                return std::unexpected(operation_error("invalid_request", "inputBindings must be a bounded array"));
            }
            std::set<std::string> logical_paths;
            for (const auto &binding : input.at("inputBindings")) {
                const auto logical = binding.at("manifestPath").get<std::string>();
                if (logical.empty() || std::filesystem::path{logical}.is_absolute() || logical.contains("..") ||
                    !logical_paths.insert(logical).second) {
                    return std::unexpected(operation_error(
                        "invalid_binding_path", "logical input paths must be unique, relative, and contained"));
                }
                const auto &binding_input = binding.at("input");
                const auto persistent_reference = persistent_file_ref(binding_input);
                if (persistent_reference)
                    result.file_inputs.push_back(*persistent_reference);
                auto resolved = resolve_input(binding_input, context.owner_id, sandbox, uploads, false);
                if (!resolved)
                    return std::unexpected(resolved.error());
                result.logical_input_paths.emplace(normalized_path(resolved->path), logical);
                replace_logical_path(result.json, logical, resolved->path.string());
                result.observed_paths.push_back(resolved->path);
                result.bound_input_paths.push_back(resolved->path);
                if (persistent_reference) {
                    auto digest = file_sha256(resolved->path, context.cancellation);
                    if (!digest)
                        return std::unexpected(digest.error());
                    result.file_input_sha256.push_back(std::move(*digest));
                }
                if (resolved->lease) {
                    result.upload_input_paths.push_back(resolved->path);
                    result.leases.push_back(std::move(*resolved->lease));
                }
                if (resolved->staging)
                    result.staging.push_back(std::move(resolved->staging));
            }
        }
        return result;
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "manifest request is malformed"));
    }
}

axk::app::Result<std::pair<std::uintmax_t, std::filesystem::file_time_type>>
file_state(const std::filesystem::path &path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return std::unexpected(operation_error("input_read_failed", "could not inspect input file size"));
    const auto last_write_time = std::filesystem::last_write_time(path, error);
    if (error)
        return std::unexpected(operation_error("input_read_failed", "could not inspect input modification time"));
    return std::pair{size, last_write_time};
}

void append_volume_paths(const axk::VolumeSpec &volume, std::vector<std::filesystem::path> &paths) {
    for (const auto &waveform : volume.waveforms)
        paths.push_back(waveform.path);
    for (const auto &sample : volume.samples) {
        if (sample.interleaved_audio_path)
            paths.push_back(*sample.interleaved_audio_path);
    }
}

std::vector<std::filesystem::path> external_paths(const axk::HdsBuildManifest &manifest) {
    std::vector<std::filesystem::path> result;
    for (const auto &partition : manifest.partitions) {
        for (const auto &volume : partition.volumes)
            append_volume_paths(volume, result);
    }
    return result;
}

std::vector<std::filesystem::path> external_paths(const axk::MediaBuildManifest &manifest) {
    std::vector<std::filesystem::path> result;
    if (manifest.transfer)
        result.push_back(manifest.transfer->source_path);
    if (manifest.authored_volume)
        append_volume_paths(*manifest.authored_volume, result);
    return result;
}

std::vector<std::filesystem::path> external_paths(const axk::AlterationManifest &manifest) {
    std::vector<std::filesystem::path> result;
    for (const auto &operation : manifest.operations) {
        std::visit(
            [&result](const auto &value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<Value, axk::InsertVolumeOperation>) {
                    append_volume_paths(value.volume, result);
                } else if constexpr (std::same_as<Value, axk::InsertSampleOperation>) {
                    if (value.sample.interleaved_audio_path)
                        result.push_back(*value.sample.interleaved_audio_path);
                } else if constexpr (std::same_as<Value, axk::InsertWaveformOperation>) {
                    result.push_back(value.waveform.path);
                }
            },
            operation.data);
    }
    return result;
}

axk::app::Result<void> require_bound_inputs(std::span<const std::filesystem::path> required,
                                            std::span<const std::filesystem::path> bound) {
    std::set<std::string> admitted;
    for (const auto &path : bound)
        admitted.insert(normalized_path(path));
    for (const auto &path : required) {
        if (!admitted.contains(normalized_path(path))) {
            return std::unexpected(operation_error(
                "missing_input_binding", "every manifest file path must have an explicit sandbox or upload binding"));
        }
    }
    return {};
}

bool contains_path(std::span<const std::filesystem::path> paths, const std::filesystem::path &candidate) {
    const auto normalized = normalized_path(candidate);
    return std::ranges::any_of(paths, [&normalized](const auto &path) { return normalized_path(path) == normalized; });
}

axk::app::Result<std::vector<FileFingerprint>> fingerprint_files(std::span<const std::filesystem::path> paths,
                                                                 const axk::CancellationToken &cancellation) {
    std::map<std::string, std::filesystem::path> unique;
    for (const auto &path : paths)
        unique.emplace(normalized_path(path), path);
    std::vector<FileFingerprint> result;
    result.reserve(unique.size());
    for (const auto &[normalized, path] : unique) {
        static_cast<void>(normalized);
        auto before = file_state(path);
        if (!before)
            return std::unexpected(before.error());
        auto digest = file_sha256(path, cancellation);
        if (!digest)
            return std::unexpected(digest.error());
        auto after = file_state(path);
        if (!after)
            return std::unexpected(after.error());
        if (*before != *after) {
            return std::unexpected(
                operation_error("input_changed", "an input changed while the write plan was being prepared"));
        }
        result.push_back({path, std::move(*digest), after->first, after->second});
    }
    return result;
}

axk::app::Result<void> verify_fingerprints(std::span<const FileFingerprint> fingerprints,
                                           const axk::CancellationToken &cancellation) {
    for (const auto &input : fingerprints) {
        auto state = file_state(input.path);
        if (!state || state->first != input.size || state->second != input.last_write_time)
            return std::unexpected(operation_error("input_changed", "an alteration input changed during execution"));
        auto digest = file_sha256(input.path, cancellation);
        if (!digest || *digest != input.sha256)
            return std::unexpected(operation_error("input_changed", "an alteration input changed during execution"));
    }
    return {};
}

std::optional<std::string> known_fingerprint(std::span<const FileFingerprint> fingerprints,
                                             const std::filesystem::path &path) {
    const auto identity = normalized_path(path);
    const auto found = std::ranges::find_if(fingerprints, [&](const FileFingerprint &fingerprint) {
        return normalized_path(fingerprint.path) == identity;
    });
    return found == fingerprints.end() ? std::nullopt : std::optional{found->sha256};
}

axk::app::Result<void> verify_plan_state(const WritePlanRecord &record, const axk::app::Sandbox &sandbox,
                                         const axk::CancellationToken &cancellation) {
    const auto build = axk::current_build_info();
    if (record.semantic_version != axk::version() || record.source_identity != build.source_identity) {
        return std::unexpected(operation_error("write_plan_stale", "server build identity changed after planning"));
    }
    for (const auto &input : record.inputs) {
        auto state = file_state(input.path);
        if (!state || state->first != input.size || state->second != input.last_write_time) {
            return std::unexpected(operation_error("write_plan_stale", "an input changed after planning"));
        }
        auto digest = file_sha256(input.path, cancellation);
        if (!digest || *digest != input.sha256)
            return std::unexpected(operation_error("write_plan_stale", "an input changed after planning"));
    }
    if (record.input_refs.size() != record.input_ref_sha256.size())
        return std::unexpected(operation_error("write_plan_stale", "write plan input identity is incomplete"));
    for (std::size_t index = 0U; index < record.input_refs.size(); ++index) {
        auto input = sandbox.open_file(record.input_refs[index]);
        if (!input)
            return std::unexpected(operation_error("write_plan_stale", "an input changed after planning"));
        auto digest = reader_sha256(*input->reader, cancellation);
        if (!digest || *digest != record.input_ref_sha256[index])
            return std::unexpected(operation_error("write_plan_stale", "an input changed after planning"));
    }
    std::error_code error;
    const auto output_exists = std::filesystem::exists(record.output_path, error);
    if (error || output_exists != record.output_existed)
        return std::unexpected(operation_error("write_plan_stale", "destination state changed after planning"));
    if (record.output_sha256) {
        const auto known = std::ranges::find_if(record.inputs, [&](const FileFingerprint &fingerprint) {
            return normalized_path(fingerprint.path) == normalized_path(record.output_path);
        });
        if (known == record.inputs.end()) {
            auto digest = file_sha256(record.output_path, cancellation);
            if (!digest || *digest != *record.output_sha256) {
                return std::unexpected(operation_error("write_plan_stale", "destination changed after planning"));
            }
        }
    }
    return {};
}

axk::app::Result<void> verify_sandbox_files(std::span<const axk::app::FileRef> references,
                                            std::span<const std::string> expected_sha256,
                                            const axk::app::Sandbox &sandbox,
                                            const axk::CancellationToken &cancellation) {
    if (references.size() != expected_sha256.size())
        return std::unexpected(operation_error("input_changed", "input identity is incomplete"));
    for (std::size_t index = 0U; index < references.size(); ++index) {
        auto input = sandbox.open_file(references[index]);
        if (!input)
            return std::unexpected(operation_error("input_changed", "an input changed during execution"));
        auto digest = reader_sha256(*input->reader, cancellation);
        if (!digest || *digest != expected_sha256[index])
            return std::unexpected(operation_error("input_changed", "an input changed during execution"));
    }
    return {};
}

axk::app::Result<void> verify_alteration_state(std::span<const FileFingerprint> inputs,
                                               const std::filesystem::path &output_path, bool output_existed,
                                               const std::optional<std::string> &output_sha256,
                                               const axk::CancellationToken &cancellation) {
    for (const auto &input : inputs) {
        auto state = file_state(input.path);
        if (!state || state->first != input.size || state->second != input.last_write_time)
            return std::unexpected(operation_error("input_changed", "an alteration input changed during execution"));
        auto digest = file_sha256(input.path, cancellation);
        if (!digest || *digest != input.sha256)
            return std::unexpected(operation_error("input_changed", "an alteration input changed during execution"));
    }
    std::error_code error;
    const auto exists = std::filesystem::exists(output_path, error);
    if (error || exists != output_existed)
        return std::unexpected(
            operation_error("destination_changed", "alteration destination changed during execution"));
    if (output_sha256) {
        const auto known = std::ranges::find_if(inputs, [&](const FileFingerprint &fingerprint) {
            return normalized_path(fingerprint.path) == normalized_path(output_path);
        });
        if (known == inputs.end()) {
            auto digest = file_sha256(output_path, cancellation);
            if (!digest || *digest != *output_sha256)
                return std::unexpected(
                    operation_error("destination_changed", "alteration destination changed during execution"));
        }
    }
    return {};
}

axk::app::Result<void> register_plan(const std::shared_ptr<WriteOperationState> &state,
                                     const std::shared_ptr<WritePlanRecord> &record) {
    std::lock_guard lock{state->mutex};
    cleanup_plans(*state, Clock::now());
    if (state->plans.size() >= state->maximum_plans)
        return std::unexpected(operation_error("write_plan_capacity", "too many write plans are active"));
    if (state->plans.contains(record->token))
        return std::unexpected(operation_error("secure_random_failed", "write plan token collision"));
    const auto reservation = normalized_path(record->output_path);
    if (state->destination_reservations.contains(reservation)) {
        return std::unexpected(
            operation_error("destination_reserved", "destination is reserved by another active plan"));
    }
    state->destination_reservations.emplace(reservation, record->token);
    state->plans.emplace(record->token, record);
    return {};
}

Json write_plan_json(const WritePlanRecord &record, std::uint64_t expires_in_seconds) {
    return {{"schemaVersion", "1.0"},
            {"planToken", record.token},
            {"expiresInSeconds", expires_in_seconds},
            {"kind", write_plan_kind_name(record.kind)},
            {"output", file_ref_json(record.output)},
            {"overwrite", record.overwrite},
            {"semanticVersion", record.semantic_version},
            {"sourceIdentity", record.source_identity},
            {"summary", record.summary},
            {"valid", true}};
}

Json manifest_choices(axk::BuildManifestKind kind) {
    Json modes = Json::array({"AUTHORED"});
    Json whole_source_inputs = Json::array();
    std::string profile;
    switch (kind) {
    case axk::BuildManifestKind::hds:
        profile = "YAMAHA_SFS_HDS";
        break;
    case axk::BuildManifestKind::fat12_floppy:
        profile = "YAMAHA_FAT12";
        break;
    case axk::BuildManifestKind::iso9660:
        profile = "YAMAHA_ISO9660";
        modes.push_back("WHOLE_SOURCE");
        whole_source_inputs.push_back("FILE_REF");
        break;
    }
    return {{"manifestSources", Json::array({"INLINE", "FILE_REF", "UPLOAD_REF"})},
            {"inputBindingSources", Json::array({"FILE_REF", "UPLOAD_REF"})},
            {"wholeSourceInputSources", std::move(whole_source_inputs)},
            {"modes", std::move(modes)},
            {"profiles", Json::array({std::move(profile)})}};
}

Json alteration_manifest_choices() {
    return {{"manifestSources", Json::array({"INLINE", "FILE_REF", "UPLOAD_REF"})},
            {"inputBindingSources", Json::array({"FILE_REF", "UPLOAD_REF"})},
            {"wholeSourceInputSources", Json::array()},
            {"modes", Json::array({"ALTERATION"})},
            {"profiles", Json::array({"YAMAHA_SFS_HDS"})}};
}

} // namespace axk::app::write_operations_internal
