#include "axklib/application/package_operations.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/secure_random.hpp"
#include "axklib/media.hpp"
#include "axklib/package.hpp"
#include "axklib/utf8.hpp"

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct PackageInput {
    std::variant<axk::app::FileRef, axk::app::UploadRef> reference;
};

struct ResolvedPackage {
    std::shared_ptr<const axk::RandomAccessReader> reader;
    std::string filename;
    std::optional<axk::app::UploadLease> lease;
};

struct PackagePlanRecord {
    std::string token;
    std::string owner_id;
    Clock::time_point expires_at;
    axk::app::FileRef target;
    axk::app::FileRef output;
    std::filesystem::path output_path;
    bool overwrite{};
    std::vector<PackageInput> inputs;
    std::vector<ResolvedPackage> resolved;
    std::vector<axk::PortablePackage> packages;
    axk::PackageImportPlan plan;
    bool claimed{};
};

struct PackageOperationState {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<PackagePlanRecord>> plans;
    std::unordered_map<std::string, std::string> destination_reservations;
    std::chrono::minutes retention{15};
    std::size_t maximum_plans{128U};
};

class TemporaryDirectoryCleanup {
  public:
    explicit TemporaryDirectoryCleanup(std::filesystem::path path) : path_{std::move(path)} {}
    ~TemporaryDirectoryCleanup() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

  private:
    std::filesystem::path path_;
};

std::string normalized_path(const std::filesystem::path &path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return (error ? path.lexically_normal() : canonical).generic_string();
}

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
    std::string code = "package_operation_failed";
    if (error.code == axk::ErrorCode::operation_cancelled) {
        code = "operation_cancelled";
    } else if (error.code == axk::ErrorCode::transaction_rejected &&
               (error.message == "target image changed while its import plan was built" ||
                error.message == "package import plan is stale for this target" ||
                error.message == "package import target changed before transaction preparation")) {
        code = "package_plan_stale";
    }
    return {std::move(code), error.message, std::move(context)};
}

axk::app::Result<void> write_reader(const std::filesystem::path &path, const axk::RandomAccessReader &reader) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(operation_error("package_read_failed", "could not create retained target staging"));
    std::vector<std::byte> buffer(
        static_cast<std::size_t>(std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, reader.size()))));
    for (std::uint64_t offset = 0U; offset < reader.size();) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), reader.size() - offset));
        if (const auto read = reader.read_exact_at(offset, std::span{buffer}.first(count)); !read)
            return std::unexpected(core_error(read.error()));
        output.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(count));
        if (!output)
            return std::unexpected(operation_error("package_read_failed", "could not write retained target staging"));
        offset += count;
    }
    output.flush();
    if (!output)
        return std::unexpected(operation_error("package_read_failed", "could not flush retained target staging"));
    return {};
}

axk::app::Result<axk::app::FileRef> parse_file_ref(const Json &input, std::string_view field) {
    try {
        const auto &value = input.at(field);
        return axk::app::FileRef{value.at("rootId").get<std::string>(), value.at("relativePath").get<std::string>()};
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", std::format("{} must be a FileRef", field)));
    }
}

Json file_ref_json(const axk::app::FileRef &reference) {
    return {{"rootId", reference.root_id}, {"relativePath", reference.relative_path}};
}

axk::app::Result<PackageInput> parse_package_input(const Json &input) {
    try {
        if (!input.is_object())
            return std::unexpected(operation_error("invalid_request", "package input must be an object"));
        if (input.contains("fileRef") && !input.contains("uploadRef")) {
            const auto &value = input.at("fileRef");
            return PackageInput{
                axk::app::FileRef{value.at("rootId").get<std::string>(), value.at("relativePath").get<std::string>()}};
        }
        if (input.contains("uploadRef") && !input.contains("fileRef")) {
            const auto &value = input.at("uploadRef");
            return PackageInput{axk::app::UploadRef{value.at("uploadId").get<std::string>()}};
        }
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "package input reference is malformed"));
    }
    return std::unexpected(
        operation_error("invalid_request", "package input must contain exactly one of fileRef or uploadRef"));
}

axk::app::Result<ResolvedPackage> resolve_package(const PackageInput &input, std::string_view owner_id,
                                                  const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads) {
    if (const auto *file = std::get_if<axk::app::FileRef>(&input.reference)) {
        auto opened = sandbox.open_file(*file);
        if (!opened)
            return std::unexpected(opened.error());
        return ResolvedPackage{std::move(opened->reader), std::move(opened->filename), std::nullopt};
    }
    const auto &upload = std::get<axk::app::UploadRef>(input.reference);
    auto snapshot = uploads.inspect(upload, owner_id);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    if (snapshot->kind != axk::app::UploadKind::package) {
        return std::unexpected(operation_error("upload_kind_mismatch", "upload is not a portable package"));
    }
    auto lease = uploads.lease(upload, owner_id);
    if (!lease)
        return std::unexpected(lease.error());
    auto reader = axk::FileReader::open(lease->path());
    if (!reader)
        return std::unexpected(operation_error("package_read_failed", reader.error().message));
    return ResolvedPackage{std::move(*reader), snapshot->filename, std::move(*lease)};
}

axk::app::Result<axk::PortablePackage> read_package(const ResolvedPackage &resolved, bool verify,
                                                    const axk::app::OperationContext &context) {
    auto package = verify ? axk::open_portable_package(*resolved.reader, resolved.filename, context.cancellation)
                          : axk::inspect_portable_package(*resolved.reader, resolved.filename, context.cancellation);
    if (!package)
        return std::unexpected(core_error(package.error()));
    if (verify) {
        auto checked = axk::verify_portable_package(*package);
        if (!checked)
            return std::unexpected(core_error(checked.error()));
    }
    return std::move(*package);
}

Json package_json(const axk::PortablePackage &package) {
    auto roots = Json::array();
    for (const auto &root : package.roots) {
        roots.push_back({{"kind", std::string{axk::package_root_kind_name(root.kind)}},
                         {"displayName", root.display_name},
                         {"nodeIds", root.node_ids}});
    }
    auto objects = Json::array();
    for (const auto &node : package.nodes) {
        objects.push_back({{"nodeId", node.node_id},
                           {"objectType", node.object_type},
                           {"name", node.name},
                           {"payloadSha256", node.payload_sha256},
                           {"normalizedSha256", node.normalized_sha256},
                           {"semanticSha256", node.semantic_sha256 ? Json(*node.semantic_sha256) : Json{}},
                           {"audioSha256", node.audio_sha256 ? Json(*node.audio_sha256) : Json{}}});
    }
    auto issues = Json::array();
    for (const auto &issue : package.issues)
        issues.push_back({{"code", issue.code}, {"message", issue.message}, {"fatal", issue.fatal}});
    return {{"schemaVersion", "1.0"},
            {"packageId", package.package_id},
            {"packageKind", std::string{axk::package_kind_name(package.kind)}},
            {"requiredExtension", std::string{axk::required_package_extension(package.kind)}},
            {"sourceMediaKind", package.source_media_kind},
            {"valid", std::ranges::none_of(package.issues, &axk::PackageIssue::fatal)},
            {"payloadsVerified", package.payloads_verified},
            {"roots", std::move(roots)},
            {"objects", std::move(objects)},
            {"relationshipCount", package.relationships.size()},
            {"issues", std::move(issues)}};
}

axk::app::Result<axk::PackageRootKind> parse_root_kind(std::string_view value) {
    if (value == "volume")
        return axk::PackageRootKind::volume;
    if (value == "program" || value == "prog")
        return axk::PackageRootKind::prog;
    if (value == "sbac" || value == "sample-bank" || value == "bank-group")
        return axk::PackageRootKind::sbac;
    if (value == "sbnk" || value == "sample")
        return axk::PackageRootKind::sbnk;
    if (value == "smpl" || value == "wave-data")
        return axk::PackageRootKind::smpl;
    return std::unexpected(operation_error("unsupported_package_root",
                                           "package root kind must be volume, program, sample-bank, sample, "
                                           "wave-data, sbac, sbnk, or smpl"));
}

axk::app::Result<std::vector<axk::PackageRootSelector>> parse_roots(const Json &input) {
    try {
        const auto &values = input.at("roots");
        if (!values.is_array() || values.empty() || values.size() > 1024U)
            return std::unexpected(operation_error("invalid_request", "roots must contain 1 to 1024 selectors"));
        std::vector<axk::PackageRootSelector> result;
        result.reserve(values.size());
        for (const auto &value : values) {
            auto kind = parse_root_kind(value.at("kind").get<std::string>());
            if (!kind)
                return std::unexpected(kind.error());
            axk::PackageRootSelector root;
            root.kind = *kind;
            if (value.contains("partitionIndex") && !value.at("partitionIndex").is_null()) {
                const auto partition = value.at("partitionIndex").get<std::uint32_t>();
                if (partition > std::numeric_limits<std::uint8_t>::max())
                    return std::unexpected(operation_error("invalid_request", "partition index is out of range"));
                root.partition_index = static_cast<std::uint8_t>(partition);
            }
            root.group_name = value.value("groupName", std::string{});
            root.volume_name = value.value("volumeName", std::string{});
            root.object_name = value.value("objectName", std::string{});
            if (root.kind == axk::PackageRootKind::volume && !root.object_name.empty())
                return std::unexpected(operation_error("invalid_request", "volume roots do not take objectName"));
            if (root.kind != axk::PackageRootKind::volume && root.object_name.empty()) {
                return std::unexpected(operation_error("invalid_request", "object package roots require objectName"));
            }
            result.push_back(std::move(root));
        }
        return result;
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "package root selectors are malformed"));
    }
}

axk::app::Result<axk::PackageImportRequest> parse_import_request(const Json &input) {
    axk::PackageImportRequest result;
    try {
        if (!input.contains("destinations") || !input.at("destinations").is_array())
            return std::unexpected(operation_error("invalid_request", "destinations must be an array"));
        for (const auto &value : input.at("destinations")) {
            axk::PackageRootDestination destination;
            destination.package_index = value.at("packageIndex").get<std::size_t>();
            destination.root_index = value.at("rootIndex").get<std::size_t>();
            if (value.contains("partitionIndex") && !value.at("partitionIndex").is_null()) {
                const auto partition = value.at("partitionIndex").get<std::uint32_t>();
                if (partition > std::numeric_limits<std::uint8_t>::max())
                    return std::unexpected(operation_error("invalid_request", "partition index is out of range"));
                destination.partition_index = static_cast<std::uint8_t>(partition);
            }
            destination.group_name = value.value("groupName", std::string{});
            destination.volume_name = value.value("volumeName", std::string{});
            destination.raw_group = value.value("rawGroup", std::string{});
            destination.raw_volume = value.value("rawVolume", std::string{});
            destination.create_destination = value.value("create", false);
            result.root_destinations.push_back(std::move(destination));
        }
        if (input.contains("renames")) {
            if (!input.at("renames").is_array())
                return std::unexpected(operation_error("invalid_request", "renames must be an array"));
            for (const auto &value : input.at("renames")) {
                result.policy.renames.push_back({value.at("packageIndex").get<std::size_t>(),
                                                 value.at("nodeId").get<std::string>(),
                                                 value.at("destinationName").get<std::string>()});
            }
        }
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "package import mappings are malformed"));
    }
    return result;
}

std::string target_kind_name(axk::MediaKind kind) {
    switch (kind) {
    case axk::MediaKind::sfs:
        return "sfs";
    case axk::MediaKind::fat12_floppy:
        return "fat12-floppy";
    case axk::MediaKind::iso9660:
        return "iso9660";
    case axk::MediaKind::standalone_object:
        return "standalone-object";
    }
    return "unknown";
}

Json plan_json(const axk::PackageImportPlan &plan, std::string_view token, std::uint64_t expires_in_seconds) {
    auto warnings = Json::array();
    for (const auto &warning : plan.warnings)
        warnings.push_back({{"code", warning.code}, {"message", warning.message}, {"fatal", warning.fatal}});
    auto conflicts = Json::array();
    for (const auto &conflict : plan.conflicts) {
        conflicts.push_back({{"code", conflict.code},
                             {"message", conflict.message},
                             {"packageIndex", conflict.package_index ? Json(*conflict.package_index) : Json{}},
                             {"rootIndex", conflict.root_index ? Json(*conflict.root_index) : Json{}},
                             {"packageId", conflict.package_id},
                             {"nodeId", conflict.node_id},
                             {"partitionIndex", conflict.partition_index ? Json(*conflict.partition_index) : Json{}},
                             {"groupName", conflict.group_name},
                             {"volumeName", conflict.volume_name},
                             {"rawGroup", conflict.raw_group},
                             {"rawVolume", conflict.raw_volume}});
    }
    auto actions = Json::array();
    for (const auto &object : plan.objects) {
        auto names = Json::array();
        for (const auto action : object.actions)
            names.push_back(std::string{axk::package_import_action_name(action)});
        actions.push_back(
            {{"actionId", object.action_id},
             {"packageIndex", object.package_index},
             {"rootIndex", object.root_index},
             {"packageId", object.package_id},
             {"nodeId", object.node_id},
             {"objectType", object.object_type},
             {"sourceName", object.source_name},
             {"destinationName", object.destination_name},
             {"partitionIndex", object.partition_index},
             {"groupName", object.group_name},
             {"volumeName", object.volume_name},
             {"rawGroup", object.raw_group},
             {"rawVolume", object.raw_volume},
             {"actions", std::move(names)},
             {"canonicalActionId", object.canonical_action_id ? Json(*object.canonical_action_id) : Json{}},
             {"targetSfsId", object.target_sfs_id ? Json(*object.target_sfs_id) : Json{}},
             {"targetLinkId", object.target_link_id ? Json(*object.target_link_id) : Json{}}});
    }
    auto allocation = Json::array();
    for (const auto &item : plan.allocation) {
        allocation.push_back({{"partitionIndex", item.partition_index},
                              {"groupName", item.group_name},
                              {"volumeName", item.volume_name},
                              {"rawGroup", item.raw_group},
                              {"rawVolume", item.raw_volume},
                              {"insertedObjectCount", item.inserted_object_count},
                              {"reusedObjectCount", item.reused_object_count},
                              {"payloadClusters", item.payload_clusters},
                              {"payloadSectors", item.payload_sectors},
                              {"continuationClusters", item.continuation_clusters},
                              {"directoryGrowthBytes", item.directory_growth_bytes},
                              {"remainingObjectIds", item.remaining_object_ids},
                              {"remainingClusters", item.remaining_clusters},
                              {"projectedImageSectors", item.projected_image_sectors},
                              {"projectedImageSizeBytes", item.projected_image_size_bytes}});
    }
    return {{"schemaVersion", "1.0"},
            {"planToken", token},
            {"expiresInSeconds", expires_in_seconds},
            {"planId", plan.plan_id},
            {"targetKind", target_kind_name(plan.target_kind)},
            {"targetSnapshotId", plan.target_snapshot_id},
            {"valid", plan.valid()},
            {"warnings", std::move(warnings)},
            {"conflicts", std::move(conflicts)},
            {"actions", std::move(actions)},
            {"allocation", std::move(allocation)}};
}

void cleanup_plans(PackageOperationState &state, Clock::time_point now) {
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

class PackagePlanClaim {
  public:
    PackagePlanClaim(std::shared_ptr<PackageOperationState> state, std::shared_ptr<PackagePlanRecord> record)
        : state_(std::move(state)), record_(std::move(record)) {}
    ~PackagePlanClaim() { release(); }
    PackagePlanClaim(const PackagePlanClaim &) = delete;
    PackagePlanClaim &operator=(const PackagePlanClaim &) = delete;
    PackagePlanClaim(PackagePlanClaim &&other) noexcept
        : state_(std::move(other.state_)), record_(std::move(other.record_)),
          active_(std::exchange(other.active_, false)) {}
    PackagePlanClaim &operator=(PackagePlanClaim &&) = delete;

    [[nodiscard]] const std::shared_ptr<PackagePlanRecord> &record() const noexcept { return record_; }

    void consume() {
        if (!active_)
            return;
        std::lock_guard lock{state_->mutex};
        const auto reservation = normalized_path(record_->output_path);
        if (const auto found = state_->destination_reservations.find(reservation);
            found != state_->destination_reservations.end() && found->second == record_->token) {
            state_->destination_reservations.erase(found);
        }
        state_->plans.erase(record_->token);
        active_ = false;
    }

  private:
    void release() {
        if (!active_)
            return;
        std::lock_guard lock{state_->mutex};
        if (const auto found = state_->plans.find(record_->token); found != state_->plans.end())
            found->second->claimed = false;
        active_ = false;
    }

    std::shared_ptr<PackageOperationState> state_;
    std::shared_ptr<PackagePlanRecord> record_;
    bool active_{true};
};

axk::app::Result<PackagePlanClaim> claim_plan(const std::shared_ptr<PackageOperationState> &state,
                                              std::string_view token, std::string_view owner_id) {
    std::lock_guard lock{state->mutex};
    cleanup_plans(*state, Clock::now());
    const auto found = state->plans.find(std::string{token});
    if (found == state->plans.end() || found->second->owner_id != owner_id) {
        return std::unexpected(operation_error("package_plan_not_found", "package import plan is absent or expired"));
    }
    if (found->second->claimed)
        return std::unexpected(operation_error("package_plan_in_use", "package import plan is already being applied"));
    found->second->claimed = true;
    return PackagePlanClaim{state, found->second};
}

axk::app::Result<Json> read_operation(const Json &input, const axk::app::OperationContext &context,
                                      const axk::app::Sandbox &sandbox, axk::app::UploadStore &uploads, bool verify) {
    auto source = parse_package_input(input.at("package"));
    if (!source)
        return std::unexpected(source.error());
    auto resolved = resolve_package(*source, context.owner_id, sandbox, uploads);
    if (!resolved)
        return std::unexpected(resolved.error());
    auto package = read_package(*resolved, verify, context);
    if (!package)
        return std::unexpected(package.error());
    return package_json(*package);
}

} // namespace

axk::app::Result<void> axk::app::bind_package_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                         UploadStore &uploads) {
    auto state = std::make_shared<PackageOperationState>();

    if (!registry.is_implemented("package.inspect")) {
        auto bound =
            registry.bind("package.inspect", [&sandbox, &uploads](const Json &input, const OperationContext &context) {
                try {
                    return read_operation(input, context, sandbox, uploads, false);
                } catch (const Json::exception &) {
                    return Result<Json>{std::unexpected(operation_error("invalid_request", "package is required"))};
                }
            });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("package.verify")) {
        auto bound =
            registry.bind("package.verify", [&sandbox, &uploads](const Json &input, const OperationContext &context) {
                try {
                    return read_operation(input, context, sandbox, uploads, true);
                } catch (const Json::exception &) {
                    return Result<Json>{std::unexpected(operation_error("invalid_request", "package is required"))};
                }
            });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("package.export")) {
        auto bound = registry.bind("package.export", [&sandbox](const Json &input, const OperationContext &context) {
            auto source = parse_file_ref(input, "source");
            auto output = parse_file_ref(input, "output");
            auto roots = parse_roots(input);
            if (!source)
                return Result<Json>{std::unexpected(source.error())};
            if (!output)
                return Result<Json>{std::unexpected(output.error())};
            if (!roots)
                return Result<Json>{std::unexpected(roots.error())};
            const auto overwrite = input.value("overwrite", false);
            if (const auto distinct = sandbox.require_distinct(*source, *output); !distinct)
                return Result<Json>{std::unexpected(distinct.error())};
            auto source_file = sandbox.open_file(*source);
            if (!source_file)
                return Result<Json>{std::unexpected(source_file.error())};
            auto media = axk::open_media(source_file->reader, std::filesystem::path{source_file->filename},
                                         context.cancellation);
            if (!media)
                return Result<Json>{std::unexpected(core_error(media.error(), source->relative_path))};
            auto build = axk::build_portable_package(*media, *roots, context.cancellation);
            if (!build)
                return Result<Json>{std::unexpected(core_error(build.error(), source->relative_path))};
            auto effective_output = *output;
            const auto required = std::string{build->required_extension};
            if (!effective_output.relative_path.ends_with(required)) {
                const auto extension = std::filesystem::path{effective_output.relative_path}.extension().string();
                if (!extension.empty()) {
                    return Result<Json>{std::unexpected(
                        operation_error("package_extension_mismatch",
                                        std::format("package output must use {}", required), output->relative_path))};
                }
                effective_output.relative_path += required;
            }
            if (context.progress)
                context.progress->report(
                    {axk::ProgressPhase::exporting, 0U, 1U, "Publishing portable package", std::nullopt});
            const auto size_bytes = build->archive.size();
            const axk::MemoryReader archive{std::move(build->archive)};
            if (auto publication = sandbox.publish_file(effective_output, overwrite, archive); !publication)
                return Result<Json>{std::unexpected(publication.error())};
            if (context.progress)
                context.progress->report(
                    {axk::ProgressPhase::exporting, 1U, 1U, "Portable package published", std::nullopt});
            auto result = package_json(build->package);
            result["output"] = file_ref_json(effective_output);
            result["sizeBytes"] = size_bytes;
            return Result<Json>{std::move(result)};
        });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("package.plan_import")) {
        auto bound = registry.bind(
            "package.plan_import", [state, &sandbox, &uploads](const Json &input, const OperationContext &context) {
                auto target = parse_file_ref(input, "target");
                auto output = parse_file_ref(input, "output");
                if (!target)
                    return Result<Json>{std::unexpected(target.error())};
                if (!output)
                    return Result<Json>{std::unexpected(output.error())};
                if (const auto distinct = sandbox.require_distinct(*target, *output); !distinct)
                    return Result<Json>{std::unexpected(distinct.error())};
                auto target_file = sandbox.open_file(*target);
                if (!target_file)
                    return Result<Json>{std::unexpected(target_file.error())};
                auto target_staging = sandbox.create_staging_directory("axklib-package-plan");
                if (!target_staging)
                    return Result<Json>{std::unexpected(target_staging.error())};
                TemporaryDirectoryCleanup target_cleanup{*target_staging};
                const auto target_path = *target_staging / "target.img";
                if (auto staged = write_reader(target_path, *target_file->reader); !staged)
                    return Result<Json>{std::unexpected(staged.error())};
                const auto overwrite = input.value("overwrite", false);
                auto output_path = sandbox.resolve_output_file(*output, overwrite);
                if (!output_path)
                    return Result<Json>{std::unexpected(output_path.error())};

                std::vector<PackageInput> inputs;
                try {
                    if (!input.contains("packages") || !input.at("packages").is_array() ||
                        input.at("packages").empty() || input.at("packages").size() > 256U) {
                        return Result<Json>{std::unexpected(
                            operation_error("invalid_request", "packages must contain 1 to 256 references"))};
                    }
                    for (const auto &value : input.at("packages")) {
                        auto parsed = parse_package_input(value);
                        if (!parsed)
                            return Result<Json>{std::unexpected(parsed.error())};
                        inputs.push_back(std::move(*parsed));
                    }
                } catch (const Json::exception &) {
                    return Result<Json>{std::unexpected(operation_error("invalid_request", "packages are malformed"))};
                }
                auto import_request = parse_import_request(input);
                if (!import_request)
                    return Result<Json>{std::unexpected(import_request.error())};

                std::vector<ResolvedPackage> resolved;
                std::vector<axk::PortablePackage> packages;
                resolved.reserve(inputs.size());
                packages.reserve(inputs.size());
                for (const auto &source : inputs) {
                    auto item = resolve_package(source, context.owner_id, sandbox, uploads);
                    if (!item)
                        return Result<Json>{std::unexpected(item.error())};
                    auto package = read_package(*item, true, context);
                    if (!package)
                        return Result<Json>{std::unexpected(package.error())};
                    resolved.push_back(std::move(*item));
                    packages.push_back(std::move(*package));
                }
                auto plan = axk::plan_package_import(target_path, packages, *import_request, context.cancellation);
                if (!plan)
                    return Result<Json>{std::unexpected(core_error(plan.error(), target->relative_path))};

                const auto now = Clock::now();
                auto token = axk::app::secure_random_hex(24U);
                if (!token)
                    return Result<Json>{std::unexpected(token.error())};
                auto record = std::make_shared<PackagePlanRecord>(PackagePlanRecord{
                    *token, context.owner_id, now + state->retention, *target, *output, *output_path, overwrite,
                    std::move(inputs), std::move(resolved), std::move(packages), std::move(*plan), false});
                {
                    std::lock_guard lock{state->mutex};
                    cleanup_plans(*state, now);
                    if (state->plans.size() >= state->maximum_plans) {
                        return Result<Json>{std::unexpected(
                            operation_error("package_plan_capacity", "too many package import plans are active"))};
                    }
                    const auto reservation = normalized_path(*output_path);
                    if (state->destination_reservations.contains(reservation)) {
                        return Result<Json>{std::unexpected(
                            operation_error("destination_reserved", "destination is reserved by another active plan"))};
                    }
                    if (state->plans.contains(*token)) {
                        return Result<Json>{
                            std::unexpected(operation_error("secure_random_failed", "package plan token collision"))};
                    }
                    state->destination_reservations.emplace(reservation, *token);
                    state->plans.emplace(*token, record);
                }
                return Result<Json>{
                    plan_json(record->plan, *token, static_cast<std::uint64_t>(state->retention.count() * 60))};
            });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("package.import")) {
        auto bound = registry.bind("package.import", [state, &sandbox, &uploads](const Json &input,
                                                                                 const OperationContext &context) {
            std::string token;
            try {
                token = input.at("planToken").get<std::string>();
            } catch (const Json::exception &) {
                return Result<Json>{std::unexpected(operation_error("invalid_request", "planToken is required"))};
            }
            auto claim = claim_plan(state, token, context.owner_id);
            if (!claim)
                return Result<Json>{std::unexpected(claim.error())};
            const auto record = claim->record();
            if (!record->plan.valid()) {
                claim->consume();
                return Result<Json>{std::unexpected(
                    operation_error("package_plan_conflicts", "package import plan contains unresolved conflicts"))};
            }

            std::vector<axk::PortablePackage> packages;
            packages.reserve(record->inputs.size());
            std::vector<ResolvedPackage> current_inputs;
            current_inputs.reserve(record->inputs.size());
            for (std::size_t index = 0; index < record->inputs.size(); ++index) {
                auto resolved = resolve_package(record->inputs[index], context.owner_id, sandbox, uploads);
                if (!resolved)
                    return Result<Json>{std::unexpected(resolved.error())};
                auto package = read_package(*resolved, true, context);
                if (!package)
                    return Result<Json>{std::unexpected(package.error())};
                if (package->package_id != record->plan.package_ids[index]) {
                    claim->consume();
                    return Result<Json>{std::unexpected(
                        operation_error("package_plan_stale", "a package changed after import planning"))};
                }
                current_inputs.push_back(std::move(*resolved));
                packages.push_back(std::move(*package));
            }
            auto target_file = sandbox.open_file(record->target);
            if (!target_file)
                return Result<Json>{std::unexpected(target_file.error())};
            auto output_path = sandbox.resolve_output_file(record->output, record->overwrite);
            if (!output_path)
                return Result<Json>{std::unexpected(output_path.error())};
            if (normalized_path(*output_path) != normalized_path(record->output_path)) {
                claim->consume();
                return Result<Json>{std::unexpected(
                    operation_error("package_plan_stale", "package import destination changed after planning"))};
            }
            auto staging_directory = sandbox.create_staging_directory("axklib-package-import");
            if (!staging_directory)
                return Result<Json>{std::unexpected(staging_directory.error())};
            TemporaryDirectoryCleanup staging_cleanup{*staging_directory};
            const auto target_path = *staging_directory / "target.img";
            if (auto staged = write_reader(target_path, *target_file->reader); !staged)
                return Result<Json>{std::unexpected(staged.error())};
            const auto staged_output = *staging_directory / output_path->filename();
            auto report = axk::apply_package_import(target_path, packages, record->plan, staged_output, false,
                                                    context.cancellation, context.progress);
            if (!report)
                return Result<Json>{std::unexpected(core_error(report.error(), record->output.relative_path))};
            auto staged_reader = axk::FileReader::open(staged_output);
            if (!staged_reader)
                return Result<Json>{std::unexpected(core_error(staged_reader.error(), record->output.relative_path))};
            if (auto published = sandbox.publish_file(record->output, record->overwrite, **staged_reader); !published)
                return Result<Json>{std::unexpected(published.error())};
            claim->consume();
            return Result<Json>{{{"schemaVersion", "1.0"},
                                 {"planId", report->plan_id},
                                 {"output", file_ref_json(record->output)},
                                 {"sourceSnapshotId", report->source_snapshot_id},
                                 {"outputSnapshotId", report->output_snapshot_id},
                                 {"applied", report->applied}}};
        });
        if (!bound)
            return bound;
    }
    auto accesses_bound = registry.bind_path_accesses(
        "package.import",
        [state](const Json &input, const OperationContext &context) -> Result<std::vector<PathAccess>> {
            std::string token;
            try {
                token = input.at("planToken").get<std::string>();
            } catch (const Json::exception &) {
                return std::unexpected(operation_error("invalid_request", "planToken is required"));
            }
            std::lock_guard lock{state->mutex};
            cleanup_plans(*state, Clock::now());
            const auto found = state->plans.find(token);
            if (found == state->plans.end() || found->second->owner_id != context.owner_id || found->second->claimed) {
                return std::unexpected(
                    operation_error("package_plan_not_found", "package import plan is expired or unknown"));
            }
            std::vector<PathAccess> accesses{{found->second->target, PathAccessMode::shared}};
            accesses.reserve(found->second->inputs.size() + 2U);
            for (const auto &input_reference : found->second->inputs) {
                if (const auto *file = std::get_if<FileRef>(&input_reference.reference))
                    accesses.push_back({*file, PathAccessMode::shared});
            }
            accesses.push_back({found->second->output, PathAccessMode::exclusive});
            return accesses;
        });
    if (!accesses_bound)
        return accesses_bound;
    return {};
}
