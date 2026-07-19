#include "axklib/application/write_operations.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
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
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "axklib/alteration.hpp"
#include "axklib/application/secure_random.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"
#include "axklib/writer.hpp"

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct ManifestDocument {
    Json json;
    std::vector<axk::app::UploadLease> leases;
    std::vector<std::filesystem::path> observed_paths;
    std::vector<std::filesystem::path> bound_input_paths;
    std::vector<std::filesystem::path> upload_input_paths;
    std::map<std::string, std::string> logical_input_paths;
};

enum class WritePlanKind : std::uint8_t { hds, floppy, iso };

struct FileFingerprint {
    std::filesystem::path path;
    std::string sha256;
    std::uintmax_t size{};
    std::filesystem::file_time_type last_write_time;
};

struct WritePlanRecord {
    std::string token;
    std::string owner_id;
    Clock::time_point expires_at;
    WritePlanKind kind{WritePlanKind::hds};
    axk::app::FileRef output;
    std::filesystem::path output_path;
    bool overwrite{};
    bool output_existed{};
    std::optional<std::string> output_sha256;
    std::vector<FileFingerprint> inputs;
    std::map<std::string, std::string> logical_input_paths;
    std::vector<axk::app::UploadLease> leases;
    std::variant<axk::HdsBuildManifest, axk::MediaBuildManifest> manifest;
    Json summary;
    std::string semantic_version;
    std::string source_identity;
    bool claimed{};
};

struct WriteOperationState {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<WritePlanRecord>> plans;
    std::unordered_map<std::string, std::string> destination_reservations;
    std::chrono::minutes retention{15};
    std::size_t maximum_plans{128U};
};

class TemporaryFileCleanup {
  public:
    explicit TemporaryFileCleanup(std::filesystem::path path) : path_{std::move(path)} {}
    ~TemporaryFileCleanup() {
        if (!active_)
            return;
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    TemporaryFileCleanup(const TemporaryFileCleanup &) = delete;
    TemporaryFileCleanup &operator=(const TemporaryFileCleanup &) = delete;

    void release() noexcept { active_ = false; }

  private:
    std::filesystem::path path_;
    bool active_{true};
};

axk::app::Error operation_error(std::string code, std::string message,
                                std::optional<std::string> relative_path = std::nullopt) {
    axk::app::ErrorContext context;
    context.relative_path = std::move(relative_path);
    return {std::move(code), std::move(message), std::move(context)};
}

axk::app::Error core_error(const axk::Error &error, std::optional<std::string> relative_path = std::nullopt) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    context.relative_path = std::move(relative_path);
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "image_operation_failed",
            error.message, std::move(context)};
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

class WritePlanClaim {
  public:
    WritePlanClaim(std::shared_ptr<WriteOperationState> state, std::string token,
                   std::shared_ptr<WritePlanRecord> record)
        : state_(std::move(state)), token_(std::move(token)), record_(std::move(record)) {}
    ~WritePlanClaim() { release(); }
    WritePlanClaim(const WritePlanClaim &) = delete;
    WritePlanClaim &operator=(const WritePlanClaim &) = delete;
    WritePlanClaim(WritePlanClaim &&other) noexcept
        : state_(std::move(other.state_)), token_(std::move(other.token_)), record_(std::move(other.record_)),
          active_(std::exchange(other.active_, false)) {}
    WritePlanClaim &operator=(WritePlanClaim &&) = delete;

    [[nodiscard]] const std::shared_ptr<WritePlanRecord> &record() const noexcept { return record_; }

    void consume() {
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

  private:
    void release() {
        if (!active_)
            return;
        std::lock_guard lock{state_->mutex};
        if (const auto found = state_->plans.find(token_); found != state_->plans.end())
            found->second->claimed = false;
        active_ = false;
    }

    std::shared_ptr<WriteOperationState> state_;
    std::string token_;
    std::shared_ptr<WritePlanRecord> record_;
    bool active_{true};
};

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

axk::app::Result<std::pair<std::filesystem::path, std::optional<axk::app::UploadLease>>>
resolve_input(const Json &input, std::string_view owner_id, const axk::app::Sandbox &sandbox,
              axk::app::UploadStore &uploads, bool require_manifest) {
    try {
        if (input.contains("fileRef") && !input.contains("uploadRef")) {
            const auto &value = input.at("fileRef");
            axk::app::FileRef reference{value.at("rootId").get<std::string>(),
                                        value.at("relativePath").get<std::string>()};
            auto path = sandbox.resolve_file(reference);
            if (!path)
                return std::unexpected(path.error());
            return std::pair{*path, std::optional<axk::app::UploadLease>{}};
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
            return std::pair{std::move(path), std::optional<axk::app::UploadLease>{std::move(*lease)}};
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

axk::app::Result<ManifestDocument> load_manifest(const Json &input, const axk::app::OperationContext &context,
                                                 const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads) {
    ManifestDocument result;
    try {
        const auto &manifest = input.at("manifest");
        if (manifest.contains("inline") && !manifest.contains("fileRef") && !manifest.contains("uploadRef")) {
            result.json = manifest.at("inline");
        } else {
            auto resolved = resolve_input(manifest, context.owner_id, sandbox, uploads, true);
            if (!resolved)
                return std::unexpected(resolved.error());
            auto text = read_text(resolved->first);
            if (!text)
                return std::unexpected(text.error());
            result.json = Json::parse(*text);
            result.observed_paths.push_back(resolved->first);
            if (resolved->second)
                result.leases.push_back(std::move(*resolved->second));
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
                auto resolved = resolve_input(binding.at("input"), context.owner_id, sandbox, uploads, false);
                if (!resolved)
                    return std::unexpected(resolved.error());
                result.logical_input_paths.emplace(normalized_path(resolved->first), logical);
                replace_logical_path(result.json, logical, resolved->first.string());
                result.observed_paths.push_back(resolved->first);
                result.bound_input_paths.push_back(resolved->first);
                if (resolved->second) {
                    result.upload_input_paths.push_back(resolved->first);
                    result.leases.push_back(std::move(*resolved->second));
                }
            }
        }
        return result;
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "manifest request is malformed"));
    }
}

axk::app::Result<std::string> file_sha256(const std::filesystem::path &path,
                                          const axk::CancellationToken &cancellation) {
    auto reader = axk::FileReader::open(path);
    if (!reader)
        return std::unexpected(core_error(reader.error()));
    constexpr std::size_t chunk_size = 1024U * 1024U;
    std::vector<std::byte> buffer(chunk_size);
    const auto destroy_context = [](EVP_MD_CTX *context) { EVP_MD_CTX_free(context); };
    std::unique_ptr<EVP_MD_CTX, decltype(destroy_context)> digest{EVP_MD_CTX_new(), destroy_context};
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        return std::unexpected(operation_error("hash_failed", "could not initialize SHA-256"));
    }
    for (std::uint64_t offset = 0U; offset < (*reader)->size();) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected(core_error(checked.error()));
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), (*reader)->size() - offset));
        const auto chunk = std::span<std::byte>{buffer}.first(count);
        if (const auto read = (*reader)->read_exact_at(offset, chunk); !read)
            return std::unexpected(core_error(read.error()));
        if (EVP_DigestUpdate(digest.get(), chunk.data(), chunk.size()) != 1)
            return std::unexpected(operation_error("hash_failed", "could not update SHA-256"));
        offset += count;
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size{};
    if (EVP_DigestFinal_ex(digest.get(), bytes.data(), &size) != 1 || size != 32U)
        return std::unexpected(operation_error("hash_failed", "could not finish SHA-256"));
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string result;
    result.reserve(static_cast<std::size_t>(size) * 2U);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(alphabet[bytes[index] >> 4U]);
        result.push_back(alphabet[bytes[index] & 0x0fU]);
    }
    return result;
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
    for (const auto &bank : volume.sample_banks) {
        if (bank.interleaved_audio_path)
            paths.push_back(*bank.interleaved_audio_path);
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
                } else if constexpr (std::same_as<Value, axk::InsertSampleBankOperation>) {
                    if (value.sample_bank.interleaved_audio_path)
                        result.push_back(*value.sample_bank.interleaved_audio_path);
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

std::optional<std::string> known_fingerprint(std::span<const FileFingerprint> fingerprints,
                                             const std::filesystem::path &path) {
    const auto identity = normalized_path(path);
    const auto found = std::ranges::find_if(fingerprints, [&](const FileFingerprint &fingerprint) {
        return normalized_path(fingerprint.path) == identity;
    });
    return found == fingerprints.end() ? std::nullopt : std::optional{found->sha256};
}

axk::app::Result<void> verify_plan_state(const WritePlanRecord &record, const axk::CancellationToken &cancellation) {
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

void decorate_build_result(Json &result, const WritePlanRecord &record) {
    result["schemaVersion"] = "1.0";
    result["kind"] = write_plan_kind_name(record.kind);
    result["summary"] = record.summary;
}

axk::app::Result<Json> validate_written_image(const std::filesystem::path &path, const axk::app::FileRef &output,
                                              const axk::app::OperationContext &context) {
    auto media = axk::open_media(path, context.cancellation);
    if (!media)
        return std::unexpected(core_error(media.error(), output.relative_path));
    auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata, 64U * 1024U * 1024U,
                                                context.cancellation);
    if (!inventory)
        return std::unexpected(core_error(inventory.error(), output.relative_path));
    auto digest = file_sha256(path, context.cancellation);
    if (!digest)
        return std::unexpected(digest.error());
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return std::unexpected(operation_error("output_read_failed", "could not inspect created image"));
    const auto issues = media->validation_issues();
    return Json{{"output", file_ref_json(output)},
                {"sizeBytes", size},
                {"sha256", *digest},
                {"objectCount", inventory->catalog.objects.size()},
                {"validation", {{"valid", issues.empty()}, {"issueCount", issues.size()}}}};
}

axk::app::Result<axk::BuildManifestKind> parse_build_kind(std::string_view value) {
    if (value == "HDS" || value == "hds")
        return axk::BuildManifestKind::hds;
    if (value == "FLOPPY" || value == "floppy")
        return axk::BuildManifestKind::fat12_floppy;
    if (value == "ISO" || value == "iso")
        return axk::BuildManifestKind::iso9660;
    return std::unexpected(operation_error("invalid_request", "kind must be HDS, FLOPPY, or ISO"));
}

Json operation_report_json(const axk::OperationReport &operation,
                           const std::map<std::string, std::string> &logical_input_paths) {
    auto wire_type = operation.type;
    std::ranges::transform(wire_type, wire_type.begin(), [](char character) {
        return character >= 'a' && character <= 'z' ? static_cast<char>(character - ('a' - 'A')) : character;
    });
    auto removed = Json::array();
    for (const auto id : operation.removed_sfs_ids)
        removed.push_back(id.value);
    auto inserted = Json::array();
    for (const auto id : operation.inserted_sfs_ids)
        inserted.push_back(id.value);
    Json result{{"id", operation.id},
                {"type", std::move(wire_type)},
                {"partitionIndex", operation.partition.value},
                {"volumeName", operation.volume_name},
                {"objectName", operation.object_name},
                {"removedSfsIds", std::move(removed)},
                {"insertedSfsIds", std::move(inserted)},
                {"freedClusters", operation.freed_clusters},
                {"allocatedClusters", operation.allocated_clusters}};
    if (operation.audio_import) {
        const auto source_path = logical_input_paths.find(normalized_path(operation.audio_import->source_path));
        result["audioImport"] = {{"sourcePath", source_path == logical_input_paths.end()
                                                    ? operation.audio_import->source_path.filename().generic_string()
                                                    : source_path->second},
                                 {"sourceFormat", operation.audio_import->source_format},
                                 {"sourceSubtype", operation.audio_import->source_subtype},
                                 {"sourceChannels", operation.audio_import->source_channels},
                                 {"sourceSampleRate", operation.audio_import->source_sample_rate},
                                 {"outputSampleRate", operation.audio_import->output_sample_rate},
                                 {"sourceSampleWidthBits", operation.audio_import->source_sample_width_bits},
                                 {"outputSampleWidthBits", operation.audio_import->output_sample_width_bits},
                                 {"outputFrames", operation.audio_import->output_frames},
                                 {"resampled", operation.audio_import->resampled},
                                 {"quantized", operation.audio_import->quantized},
                                 {"sampleWidthConverted", operation.audio_import->sample_width_converted},
                                 {"ditherAlgorithm", operation.audio_import->dither_algorithm},
                                 {"splitStereo", operation.audio_import->split_stereo},
                                 {"clippedSamples", operation.audio_import->clipped_samples}};
    } else {
        result["audioImport"] = nullptr;
    }
    return result;
}

Json alteration_summary(std::span<const axk::OperationReport> operations) {
    std::uint64_t freed_clusters{};
    std::uint64_t allocated_clusters{};
    for (const auto &operation : operations) {
        freed_clusters += operation.freed_clusters;
        allocated_clusters += operation.allocated_clusters;
    }
    return {{"operationCount", operations.size()},
            {"freedClusters", freed_clusters},
            {"allocatedClusters", allocated_clusters}};
}

axk::Result<axk::app::PreparedLocalBuildManifest>
prepare_local_manifest_document(const std::filesystem::path &manifest_path,
                                std::span<const std::filesystem::path> paths) {
    std::ifstream input{manifest_path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io, "could not open manifest"));
    }
    Json document;
    try {
        input >> document;
    } catch (const Json::exception &) {
        return std::unexpected(axk::make_error(axk::ErrorCode::manifest_invalid, axk::ErrorCategory::manifest,
                                               "manifest is not valid JSON"));
    }

    std::map<std::string, axk::app::LocalManifestInputBinding> bindings;
    for (std::size_t index = 0; index < paths.size(); ++index) {
        bindings.try_emplace(normalized_path(paths[index]),
                             axk::app::LocalManifestInputBinding{std::format("inputs/{:04}", index), paths[index]});
    }
    std::set<std::string> replaced;
    const auto rewrite = [&](const auto &self, Json &value) -> void {
        if (value.is_string()) {
            auto candidate = std::filesystem::path{value.get_ref<const std::string &>()};
            if (candidate.is_relative())
                candidate = manifest_path.parent_path() / candidate;
            const auto found = bindings.find(normalized_path(candidate));
            if (found != bindings.end()) {
                value = found->second.manifest_path;
                replaced.insert(found->first);
            }
            return;
        }
        if (value.is_array() || value.is_object()) {
            for (auto &item : value)
                self(self, item);
        }
    };
    rewrite(rewrite, document);
    if (replaced.size() != bindings.size()) {
        return std::unexpected(axk::make_error(axk::ErrorCode::manifest_invalid, axk::ErrorCategory::manifest,
                                               "could not bind every manifest input path"));
    }
    axk::app::PreparedLocalBuildManifest result{std::move(document), {}};
    for (auto &[canonical, binding] : bindings) {
        static_cast<void>(canonical);
        result.bindings.push_back(std::move(binding));
    }
    return result;
}

} // namespace

axk::app::Result<void> axk::app::bind_manifest_operations(OperationRegistry &registry) {
    if (!registry.is_implemented("create.manifest")) {
        auto bound = registry.bind("create.manifest", [](const Json &input, const OperationContext &) {
            std::string kind_text;
            try {
                kind_text = input.at("kind").get<std::string>();
            } catch (const Json::exception &) {
                return Result<Json>{std::unexpected(operation_error("invalid_request", "kind is required"))};
            }
            auto kind = parse_build_kind(kind_text);
            if (!kind)
                return Result<Json>{std::unexpected(kind.error())};
            auto serialized = axk::serialize_build_manifest_template(*kind);
            if (!serialized)
                return Result<Json>{std::unexpected(core_error(serialized.error()))};
            try {
                return Result<Json>{Json{{"schemaVersion", "1.0"},
                                         {"kind", kind_text},
                                         {"manifest", Json::parse(*serialized)},
                                         {"canonicalJson", *serialized},
                                         {"choices", manifest_choices(*kind)},
                                         {"documentation", "/formats/generated-image-writing/"}}};
            } catch (const Json::exception &) {
                return Result<Json>{
                    std::unexpected(operation_error("manifest_serialization", "starter manifest is invalid JSON"))};
            }
        });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("alter.manifest")) {
        auto bound = registry.bind("alter.manifest", [](const Json &, const OperationContext &) {
            auto serialized = axk::serialize_alteration_manifest_template();
            if (!serialized)
                return Result<Json>{std::unexpected(core_error(serialized.error()))};
            try {
                return Result<Json>{Json{{"schemaVersion", "1.0"},
                                         {"kind", "ALTERATION"},
                                         {"manifest", Json::parse(*serialized)},
                                         {"canonicalJson", *serialized},
                                         {"choices", alteration_manifest_choices()},
                                         {"documentation", "/output-contracts/writer-and-alteration/"}}};
            } catch (const Json::exception &) {
                return Result<Json>{
                    std::unexpected(operation_error("manifest_serialization", "starter manifest is invalid JSON"))};
            }
        });
        if (!bound)
            return bound;
    }
    return {};
}

axk::Result<axk::app::PreparedLocalBuildManifest>
axk::app::prepare_local_build_manifest(std::string_view kind, const std::filesystem::path &manifest_path) {
    auto manifest_kind = parse_build_kind(kind);
    if (!manifest_kind) {
        return std::unexpected(axk::make_error(axk::ErrorCode::invalid_argument, axk::ErrorCategory::manifest,
                                               manifest_kind.error().message));
    }
    std::vector<std::filesystem::path> paths;
    if (*manifest_kind == axk::BuildManifestKind::hds) {
        auto manifest = axk::load_hds_build_manifest(manifest_path);
        if (!manifest)
            return std::unexpected(manifest.error());
        paths = external_paths(*manifest);
    } else {
        auto manifest = axk::load_media_build_manifest(manifest_path);
        if (!manifest)
            return std::unexpected(manifest.error());
        const auto expected_format = *manifest_kind == axk::BuildManifestKind::fat12_floppy
                                         ? axk::MediaImageFormat::fat12_floppy
                                         : axk::MediaImageFormat::iso9660;
        if (manifest->format != expected_format) {
            return std::unexpected(axk::make_error(axk::ErrorCode::invalid_argument, axk::ErrorCategory::manifest,
                                                   "manifest format does not match image build"));
        }
        paths = external_paths(*manifest);
    }

    return prepare_local_manifest_document(manifest_path, paths);
}

axk::Result<axk::app::PreparedLocalBuildManifest>
axk::app::prepare_local_alteration_manifest(const std::filesystem::path &manifest_path) {
    auto manifest = axk::load_alteration_manifest(manifest_path);
    if (!manifest)
        return std::unexpected(manifest.error());
    const auto paths = external_paths(*manifest);
    return prepare_local_manifest_document(manifest_path, paths);
}

axk::app::Result<void> axk::app::bind_write_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                       UploadStore &uploads) {
    if (auto bound = bind_manifest_operations(registry); !bound)
        return bound;

    auto state = std::make_shared<WriteOperationState>();

    OperationRegistry::Handler create_plan_handler = [state, &sandbox, &uploads](const Json &input,
                                                                                 const OperationContext &context) {
        std::string kind_text;
        try {
            kind_text = input.at("kind").get<std::string>();
        } catch (const Json::exception &) {
            return Result<Json>{std::unexpected(operation_error("invalid_request", "kind is required"))};
        }
        auto manifest_kind = parse_build_kind(kind_text);
        if (!manifest_kind)
            return Result<Json>{std::unexpected(manifest_kind.error())};
        auto output = parse_file_ref(input, "output");
        if (!output)
            return Result<Json>{std::unexpected(output.error())};
        const auto overwrite = input.value("overwrite", false);
        auto output_path = sandbox.resolve_output_file(*output, overwrite);
        if (!output_path)
            return Result<Json>{std::unexpected(output_path.error())};
        auto document = load_manifest(input, context, sandbox, uploads);
        if (!document)
            return Result<Json>{std::unexpected(document.error())};

        const auto serialized = document->json.dump();
        Json summary;
        std::variant<axk::HdsBuildManifest, axk::MediaBuildManifest> manifest;
        std::vector<std::filesystem::path> required_paths;
        if (*manifest_kind == axk::BuildManifestKind::hds) {
            auto parsed = axk::parse_hds_build_manifest(serialized);
            if (!parsed)
                return Result<Json>{std::unexpected(core_error(parsed.error()))};
            required_paths = external_paths(*parsed);
            if (auto admitted = require_bound_inputs(required_paths, document->bound_input_paths); !admitted)
                return Result<Json>{std::unexpected(admitted.error())};
            auto planned = axk::plan_hds_build(*parsed, context.cancellation);
            if (!planned)
                return Result<Json>{std::unexpected(core_error(planned.error()))};
            summary = {{"format", "HDS"},
                       {"sizeBytes", planned->size_bytes},
                       {"partitionCount", planned->partition_count},
                       {"objectCount", planned->object_count}};
            manifest = std::move(*parsed);
        } else {
            auto parsed = axk::parse_media_build_manifest(serialized);
            if (!parsed)
                return Result<Json>{std::unexpected(core_error(parsed.error()))};
            const auto expected_format = *manifest_kind == axk::BuildManifestKind::fat12_floppy
                                             ? axk::MediaImageFormat::fat12_floppy
                                             : axk::MediaImageFormat::iso9660;
            if (parsed->format != expected_format) {
                return Result<Json>{std::unexpected(
                    operation_error("manifest_kind_mismatch", "manifest format does not match image build"))};
            }
            if (parsed->transfer && contains_path(document->upload_input_paths, parsed->transfer->source_path)) {
                return Result<Json>{std::unexpected(operation_error(
                    "whole_source_requires_file_ref", "whole-source transfer input must be a persistent FileRef"))};
            }
            required_paths = external_paths(*parsed);
            if (auto admitted = require_bound_inputs(required_paths, document->bound_input_paths); !admitted)
                return Result<Json>{std::unexpected(admitted.error())};
            auto planned = axk::plan_media_build(*parsed, context.cancellation);
            if (!planned)
                return Result<Json>{std::unexpected(core_error(planned.error()))};
            summary = {{"format", write_plan_kind_name(write_plan_kind(*manifest_kind))},
                       {"objectCount", planned->object_count}};
            manifest = std::move(*parsed);
        }

        auto fingerprint_paths = document->observed_paths;
        fingerprint_paths.insert(fingerprint_paths.end(), required_paths.begin(), required_paths.end());
        auto fingerprints = fingerprint_files(fingerprint_paths, context.cancellation);
        if (!fingerprints)
            return Result<Json>{std::unexpected(fingerprints.error())};
        std::error_code filesystem_error;
        const auto output_existed = std::filesystem::exists(*output_path, filesystem_error);
        if (filesystem_error) {
            return Result<Json>{
                std::unexpected(operation_error("output_read_failed", "could not inspect build destination"))};
        }
        std::optional<std::string> output_digest;
        if (output_existed) {
            output_digest = known_fingerprint(*fingerprints, *output_path);
            if (!output_digest) {
                auto digest = file_sha256(*output_path, context.cancellation);
                if (!digest)
                    return Result<Json>{std::unexpected(digest.error())};
                output_digest = std::move(*digest);
            }
        }
        const auto build = axk::current_build_info();
        const auto now = Clock::now();
        auto token = axk::app::secure_random_hex(24U);
        if (!token)
            return Result<Json>{std::unexpected(token.error())};
        auto record = std::make_shared<WritePlanRecord>(WritePlanRecord{
            std::move(*token), context.owner_id, now + state->retention, write_plan_kind(*manifest_kind), *output,
            *output_path, overwrite, output_existed, std::move(output_digest), std::move(*fingerprints),
            std::move(document->logical_input_paths), std::move(document->leases), std::move(manifest),
            std::move(summary), std::string{axk::version()}, build.source_identity, false});
        if (auto registered = register_plan(state, record); !registered)
            return Result<Json>{std::unexpected(registered.error())};
        return Result<Json>{write_plan_json(*record, static_cast<std::uint64_t>(state->retention.count() * 60))};
    };

    if (!registry.is_implemented("create.plan")) {
        auto bound = registry.bind("create.plan", create_plan_handler);
        if (!bound)
            return bound;
    }

    if (!registry.is_implemented("create.hds.profiles")) {
        auto bound = registry.bind("create.hds.profiles", [](const Json &, const OperationContext &) {
            auto profiles = Json::array();
            for (const auto &profile : axk::hds_creation_profiles()) {
                auto options = Json::array();
                for (const auto &option : profile.partition_options) {
                    options.push_back({{"partitionCount", option.partition_count},
                                       {"partitionSizeBytes", option.partitions.front().filesystem_sector_count * 512U},
                                       {"unusedTailBytes", option.unused_tail_sectors * 512U}});
                }
                profiles.push_back({{"profileId", hds_creation_profile_wire_id(profile.id)},
                                    {"sizeBytes", profile.size_bytes},
                                    {"defaultPartitionCount", profile.default_partition_count},
                                    {"partitionOptions", std::move(options)}});
            }
            return Result<Json>{Json{{"schemaVersion", "1.0"}, {"profiles", std::move(profiles)}}};
        });
        if (!bound)
            return bound;
    }

    if (!registry.is_implemented("create.hds.plan")) {
        auto bound = registry.bind("create.hds.plan", [create_plan_handler](const Json &input,
                                                                            const OperationContext &context) {
            std::string profile_text;
            std::uint8_t partition_count{};
            try {
                profile_text = input.at("profileId").get<std::string>();
                partition_count = input.at("partitionCount").get<std::uint8_t>();
            } catch (const Json::exception &) {
                return Result<Json>{
                    std::unexpected(operation_error("invalid_request", "profileId and partitionCount are required"))};
            }
            const auto profile = parse_hds_creation_profile_wire_id(profile_text);
            if (!profile) {
                return Result<Json>{
                    std::unexpected(operation_error("invalid_request", "profileId is not a supported HDS profile"))};
            }
            auto planned = axk::plan_hds_creation({*profile, partition_count}, context.cancellation);
            if (!planned)
                return Result<Json>{std::unexpected(core_error(planned.error()))};
            if (!input.contains("output")) {
                return Result<Json>{std::unexpected(operation_error("invalid_request", "output is required"))};
            }

            auto partitions = Json::array();
            for (const auto &partition : planned->manifest.partitions) {
                partitions.push_back({{"name", partition.name}, {"volumes", Json::array()}});
            }
            Json generic_request{{"kind", "HDS"},
                                 {"manifest",
                                  {{"inline",
                                    {{"schema_version", "1.0"},
                                     {"size_bytes", planned->manifest.size_bytes},
                                     {"partitions", std::move(partitions)}}}}},
                                 {"output", input.at("output")},
                                 {"overwrite", input.value("overwrite", false)}};
            return create_plan_handler(generic_request, context);
        });
        if (!bound)
            return bound;
    }

    const auto bind_create = [&](std::string_view operation_id, axk::BuildManifestKind expected_kind) -> Result<void> {
        if (registry.is_implemented(operation_id))
            return {};
        return registry.bind(operation_id, [state, expected_kind, &sandbox](const Json &input,
                                                                            const OperationContext &context) {
            std::string token;
            try {
                token = input.at("planToken").get<std::string>();
            } catch (const Json::exception &) {
                return Result<Json>{std::unexpected(operation_error("invalid_request", "planToken is required"))};
            }
            auto claim = claim_plan(state, token, context.owner_id, write_plan_kind(expected_kind));
            if (!claim)
                return Result<Json>{std::unexpected(claim.error())};
            const auto &record = *claim->record();
            if (auto verified = verify_plan_state(record, context.cancellation); !verified) {
                claim->consume();
                return Result<Json>{std::unexpected(verified.error())};
            }
            auto output_path = sandbox.resolve_output_file(record.output, record.overwrite);
            if (!output_path)
                return Result<Json>{std::unexpected(output_path.error())};
            if (normalized_path(*output_path) != normalized_path(record.output_path)) {
                return Result<Json>{std::unexpected(
                    operation_error("write_plan_stale", "destination identity changed after planning"))};
            }
            if (expected_kind == axk::BuildManifestKind::hds) {
                const auto &manifest = std::get<axk::HdsBuildManifest>(record.manifest);
                auto written = axk::write_hds_image(manifest, *output_path, record.overwrite, context.cancellation);
                if (!written)
                    return Result<Json>{std::unexpected(core_error(written.error(), record.output.relative_path))};
                auto partitions = Json::array();
                for (const auto &partition : written->partitions) {
                    partitions.push_back({{"index", partition.geometry.index},
                                          {"name", partition.name},
                                          {"startSector", partition.geometry.start_sector},
                                          {"sectorCount", partition.geometry.filesystem_sector_count},
                                          {"clusterCount", partition.geometry.cluster_count},
                                          {"freeKiB", partition.sampler_visible_free_kib}});
                }
                auto result = validate_written_image(*output_path, record.output, context);
                if (!result)
                    return Result<Json>{std::unexpected(result.error())};
                decorate_build_result(*result, record);
                (*result)["partitions"] = std::move(partitions);
                (*result)["unusedTailSectors"] = written->unused_tail_sectors;
                claim->consume();
                return result;
            } else {
                const auto &manifest = std::get<axk::MediaBuildManifest>(record.manifest);
                auto written = axk::write_media_image(manifest, *output_path, record.overwrite, context.cancellation);
                if (!written)
                    return Result<Json>{std::unexpected(core_error(written.error(), record.output.relative_path))};
            }
            auto result = validate_written_image(*output_path, record.output, context);
            if (!result)
                return Result<Json>{std::unexpected(result.error())};
            decorate_build_result(*result, record);
            claim->consume();
            return result;
        });
    };
    if (auto bound = bind_create("create.hds", axk::BuildManifestKind::hds); !bound)
        return bound;
    if (auto bound = bind_create("create.floppy", axk::BuildManifestKind::fat12_floppy); !bound)
        return bound;
    if (auto bound = bind_create("create.iso", axk::BuildManifestKind::iso9660); !bound)
        return bound;

    if (!registry.is_implemented("alter.inspect")) {
        auto bound =
            registry.bind("alter.inspect", [&sandbox, &uploads](const Json &input, const OperationContext &context) {
                auto source = parse_file_ref(input, "source");
                if (!source)
                    return Result<Json>{std::unexpected(source.error())};
                auto source_path = sandbox.resolve_file(*source);
                if (!source_path)
                    return Result<Json>{std::unexpected(source_path.error())};
                auto document = load_manifest(input, context, sandbox, uploads);
                if (!document)
                    return Result<Json>{std::unexpected(document.error())};
                auto manifest = axk::parse_alteration_manifest(document->json.dump());
                if (!manifest)
                    return Result<Json>{std::unexpected(core_error(manifest.error()))};
                const auto required_paths = external_paths(*manifest);
                if (auto admitted = require_bound_inputs(required_paths, document->bound_input_paths); !admitted)
                    return Result<Json>{std::unexpected(admitted.error())};
                auto inspection =
                    axk::inspect_hds_alteration(*source_path, *manifest, context.cancellation, context.progress);
                if (!inspection)
                    return Result<Json>{std::unexpected(core_error(inspection.error(), source->relative_path))};

                auto operations = Json::array();
                for (const auto &operation : inspection->operations)
                    operations.push_back(operation_report_json(operation, document->logical_input_paths));
                const auto build = axk::current_build_info();
                Json result = {{"schemaVersion", "1.0"},
                               {"kind", "ALTERATION"},
                               {"semanticVersion", axk::version()},
                               {"sourceIdentity", build.source_identity},
                               {"summary", alteration_summary(inspection->operations)},
                               {"valid", true},
                               {"operations", std::move(operations)},
                               {"warnings", Json::array()},
                               {"validation", {{"valid", true}, {"issueCount", 0U}}}};
                return Result<Json>{std::move(result)};
            });
        if (!bound)
            return bound;
    }

    if (!registry.is_implemented("alter.hds")) {
        auto bound = registry.bind("alter.hds", [&sandbox, &uploads](const Json &input,
                                                                     const OperationContext &context) {
            auto source = parse_file_ref(input, "source");
            if (!source)
                return Result<Json>{std::unexpected(source.error())};
            auto source_path = sandbox.resolve_file(*source);
            if (!source_path)
                return Result<Json>{std::unexpected(source_path.error())};
            const auto replace_source = input.value("replaceSource", false);
            auto output = parse_file_ref(input, "output");
            if (!output)
                return Result<Json>{std::unexpected(output.error())};
            if (replace_source && input.value("overwrite", false)) {
                return Result<Json>{std::unexpected(
                    operation_error("invalid_request", "overwrite must be omitted when replaceSource is true"))};
            }
            if (replace_source) {
                auto output_identity = sandbox.resolve_file(*output);
                if (!output_identity)
                    return Result<Json>{std::unexpected(output_identity.error())};
                if (normalized_path(*source_path) != normalized_path(*output_identity)) {
                    return Result<Json>{std::unexpected(
                        operation_error("invalid_request", "output must match source when replaceSource is true"))};
                }
            } else if (const auto distinct = sandbox.require_distinct(*source, *output); !distinct) {
                return Result<Json>{std::unexpected(distinct.error())};
            }
            const auto overwrite = replace_source ? true : input.value("overwrite", false);
            auto output_path = sandbox.resolve_output_file(*output, overwrite);
            if (!output_path)
                return Result<Json>{std::unexpected(output_path.error())};
            auto document = load_manifest(input, context, sandbox, uploads);
            if (!document)
                return Result<Json>{std::unexpected(document.error())};
            auto manifest = axk::parse_alteration_manifest(document->json.dump());
            if (!manifest)
                return Result<Json>{std::unexpected(core_error(manifest.error()))};
            const auto required_paths = external_paths(*manifest);
            if (auto admitted = require_bound_inputs(required_paths, document->bound_input_paths); !admitted)
                return Result<Json>{std::unexpected(admitted.error())};
            auto fingerprint_paths = document->observed_paths;
            fingerprint_paths.push_back(*source_path);
            fingerprint_paths.insert(fingerprint_paths.end(), required_paths.begin(), required_paths.end());
            auto fingerprints = fingerprint_files(fingerprint_paths, context.cancellation);
            if (!fingerprints)
                return Result<Json>{std::unexpected(fingerprints.error())};
            std::error_code filesystem_error;
            const auto output_existed = std::filesystem::exists(*output_path, filesystem_error);
            if (filesystem_error) {
                return Result<Json>{
                    std::unexpected(operation_error("output_read_failed", "could not inspect alteration destination"))};
            }
            std::optional<std::string> output_digest;
            if (output_existed) {
                output_digest = known_fingerprint(*fingerprints, *output_path);
                if (!output_digest) {
                    auto digest = file_sha256(*output_path, context.cancellation);
                    if (!digest)
                        return Result<Json>{std::unexpected(digest.error())};
                    output_digest = std::move(*digest);
                }
            }
            auto staging = axk::text::temporary_sibling(*output_path);
            if (!staging) {
                return Result<Json>{std::unexpected(
                    operation_error("alteration_write_failed", "could not name alteration staging output"))};
            }
            TemporaryFileCleanup cleanup{*staging};
            auto altered =
                axk::alter_hds(*source_path, *manifest, *staging, context.cancellation, context.progress, false);
            if (!altered)
                return Result<Json>{std::unexpected(core_error(altered.error(), output->relative_path))};
            if (context.progress) {
                context.progress->report(
                    {axk::ProgressPhase::validating, 0U, 1U, "validating alteration image", std::nullopt});
            }
            if (const auto checked = context.cancellation.check(); !checked)
                return Result<Json>{std::unexpected(core_error(checked.error(), output->relative_path))};
            auto validation = validate_written_image(*staging, *output, context);
            if (!validation)
                return Result<Json>{std::unexpected(validation.error())};
            if (context.progress) {
                context.progress->report(
                    {axk::ProgressPhase::validating, 1U, 1U, "validated alteration image", std::nullopt});
            }
            if (auto verified = verify_alteration_state(*fingerprints, *output_path, output_existed, output_digest,
                                                        context.cancellation);
                !verified) {
                return Result<Json>{std::unexpected(verified.error())};
            }
            if (context.progress) {
                context.progress->report(
                    {axk::ProgressPhase::publishing, 0U, 1U, "publishing alteration image", std::nullopt});
            }
            if (const auto checked = context.cancellation.check(); !checked)
                return Result<Json>{std::unexpected(core_error(checked.error(), output->relative_path))};
            if (auto published = axk::detail::publish_temporary_file(*staging, *output_path, overwrite); !published) {
                return Result<Json>{std::unexpected(core_error(published.error(), output->relative_path))};
            }
            cleanup.release();
            if (context.progress) {
                context.progress->report(
                    {axk::ProgressPhase::publishing, 1U, 1U, "published alteration image", std::nullopt});
            }
            auto operations = Json::array();
            for (const auto &operation : altered->operations)
                operations.push_back(operation_report_json(operation, document->logical_input_paths));
            (*validation)["operations"] = std::move(operations);
            (*validation)["applied"] = altered->applied;
            (*validation)["schemaVersion"] = "1.0";
            (*validation)["kind"] = "ALTERATION";
            (*validation)["summary"] = alteration_summary(altered->operations);
            (*validation)["warnings"] = Json::array();
            return validation;
        });
        if (!bound)
            return bound;
    }
    return {};
}
