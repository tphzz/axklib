#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/media.hpp"

namespace axk {

enum class PackageRootKind : std::uint8_t { volume, prog, sbac, sbnk, smpl, sequ };
enum class PackageKind : std::uint8_t { volume, program, sbac, sbnk, smpl, sequence, bundle };

struct PackageRootSelector {
    PackageRootKind kind{PackageRootKind::volume};
    std::optional<std::uint8_t> partition_index;
    std::string group_name;
    std::string volume_name;
    std::string object_name;
    std::optional<std::string> object_key;
};

struct PackageRelocation {
    std::uint32_t offset{};
    std::uint32_t width{};
    std::string mask_hex;
    std::string role;
    std::string expected_hex;
    std::vector<std::string> edge_ids;

    friend bool operator==(const PackageRelocation &, const PackageRelocation &) = default;
};

struct PackagePlacementHint {
    std::string group_name;
    std::string volume_name;
    std::string category_name;
    std::string entry_name;

    friend bool operator==(const PackagePlacementHint &, const PackagePlacementHint &) = default;
};

struct PackageNode {
    std::string node_id;
    std::string object_type;
    std::string object_format;
    std::string name;
    std::string payload_path;
    std::string payload_sha256;
    std::string normalized_sha256;
    std::optional<std::string> semantic_sha256;
    std::optional<std::string> audio_sha256;
    PackagePlacementHint placement_hint;
    std::vector<PackageRelocation> relocations;
    std::vector<std::byte> raw_payload;

    friend bool operator==(const PackageNode &, const PackageNode &) = default;
};

struct PackageRelationship {
    std::string edge_id;
    std::string source_node_id;
    std::string target_node_id;
    std::string role;
    std::uint32_t ordinal{};

    friend bool operator==(const PackageRelationship &, const PackageRelationship &) = default;
};

struct PackageRoot {
    PackageRootKind kind{PackageRootKind::volume};
    std::string display_name;
    std::vector<std::string> node_ids;

    friend bool operator==(const PackageRoot &, const PackageRoot &) = default;
};

struct PackageIssue {
    std::string code;
    std::string message;
    bool fatal{};

    friend bool operator==(const PackageIssue &, const PackageIssue &) = default;
};

struct PortablePackage {
    std::string schema_version;
    PackageKind kind{PackageKind::bundle};
    std::string package_id;
    std::string source_media_kind;
    std::vector<PackageRoot> roots;
    std::vector<PackageNode> nodes;
    std::vector<PackageRelationship> relationships;
    std::vector<PackageIssue> issues;
    bool payloads_verified{};
};

struct PackageBuild {
    PortablePackage package;
    std::string required_extension;
    std::vector<std::byte> archive;
};

struct PackagePublication {
    std::filesystem::path output_path;
    std::string package_id;
    PackageKind kind{PackageKind::bundle};
    std::uint64_t size_bytes{};
};

enum class PackageImportObjectAction : std::uint8_t {
    reuse,
    rename,
    relocate,
    insert,
    conflict,
};

struct PackageRootDestination {
    std::size_t package_index{};
    std::size_t root_index{};
    std::optional<std::uint8_t> partition_index;
    std::string group_name;
    std::string volume_name;
    std::string raw_group;
    std::string raw_volume;
    bool create_destination{};

    friend bool operator==(const PackageRootDestination &, const PackageRootDestination &) = default;
};

struct PackageNodeRename {
    std::size_t package_index{};
    std::string node_id;
    std::string destination_name;

    friend bool operator==(const PackageNodeRename &, const PackageNodeRename &) = default;
};

struct PackageImportPolicy {
    std::vector<PackageNodeRename> renames;

    friend bool operator==(const PackageImportPolicy &, const PackageImportPolicy &) = default;
};

struct PackageImportRequest {
    std::vector<PackageRootDestination> root_destinations;
    PackageImportPolicy policy;

    friend bool operator==(const PackageImportRequest &, const PackageImportRequest &) = default;
};

struct PackageImportConflict {
    std::string code;
    std::string message;
    std::optional<std::size_t> package_index;
    std::optional<std::size_t> root_index;
    std::string package_id;
    std::string node_id;
    std::optional<std::uint8_t> partition_index;
    std::string group_name;
    std::string volume_name;
    std::string raw_group;
    std::string raw_volume;

    friend bool operator==(const PackageImportConflict &, const PackageImportConflict &) = default;
};

struct PlannedPackageObject {
    std::string action_id;
    std::size_t package_index{};
    std::size_t root_index{};
    std::string package_id;
    std::string node_id;
    std::string object_type;
    std::string source_name;
    std::string destination_name;
    std::string normalized_sha256;
    std::uint8_t partition_index{};
    std::string group_name;
    std::string volume_name;
    std::string raw_group;
    std::string raw_volume;
    std::vector<PackageImportObjectAction> actions;
    std::optional<std::string> canonical_action_id;
    std::optional<std::string> existing_object_key;
    std::optional<std::uint32_t> target_sfs_id;
    std::optional<std::uint32_t> target_link_id;
    std::vector<std::uint8_t> target_program_numbers;
    bool target_grouped{};
    std::uint64_t payload_clusters{};
    std::uint64_t payload_sectors{};
    std::uint64_t continuation_clusters{};

    friend bool operator==(const PlannedPackageObject &, const PlannedPackageObject &) = default;
};

struct PackageAllocationDelta {
    std::uint8_t partition_index{};
    std::string group_name;
    std::string volume_name;
    std::string raw_group;
    std::string raw_volume;
    std::uint64_t inserted_object_count{};
    std::uint64_t reused_object_count{};
    std::uint64_t payload_clusters{};
    std::uint64_t payload_sectors{};
    std::uint64_t continuation_clusters{};
    std::uint64_t directory_growth_bytes{};
    std::uint64_t remaining_object_ids{};
    std::uint64_t remaining_clusters{};
    std::uint64_t projected_image_sectors{};
    std::uint64_t projected_image_size_bytes{};

    friend bool operator==(const PackageAllocationDelta &, const PackageAllocationDelta &) = default;
};

struct PlannedPackageDestination {
    std::uint8_t partition_index{};
    std::string group_name;
    std::string volume_name;
    std::string raw_group;
    std::string raw_volume;
    bool create{};
    std::vector<std::uint32_t> infrastructure_sfs_ids;
    std::uint64_t infrastructure_clusters{};
    std::uint64_t root_directory_growth_bytes{};

    friend bool operator==(const PlannedPackageDestination &, const PlannedPackageDestination &) = default;
};

struct PackageImportPlan {
    std::string schema_version;
    MediaKind target_kind{MediaKind::sfs};
    std::string target_snapshot_id;
    std::string policy_digest;
    std::string plan_id;
    std::vector<std::string> package_ids;
    std::vector<PackageIssue> warnings;
    std::vector<PlannedPackageDestination> destinations;
    std::vector<PlannedPackageObject> objects;
    std::vector<PackageAllocationDelta> allocation;
    std::vector<PackageImportConflict> conflicts;

    [[nodiscard]] bool valid() const noexcept { return conflicts.empty(); }
};

struct PackageImportReport {
    std::filesystem::path source_path;
    std::filesystem::path output_path;
    std::string plan_id;
    std::string source_snapshot_id;
    std::string output_snapshot_id;
    bool applied{};
    std::vector<PlannedPackageObject> objects;
    std::vector<PackageAllocationDelta> allocation;
};

AXK_API std::string_view package_root_kind_name(PackageRootKind kind) noexcept;
AXK_API std::string_view package_kind_name(PackageKind kind) noexcept;
AXK_API std::string_view required_package_extension(PackageKind kind) noexcept;
AXK_API std::string_view package_import_action_name(PackageImportObjectAction action) noexcept;

AXK_API Result<void> verify_portable_package(const PortablePackage &package);
AXK_API Result<void> verify_package_import_plan(const PackageImportPlan &plan);

AXK_API Result<PackageBuild> build_portable_package(const MediaContainer &source,
                                                    std::span<const PackageRootSelector> roots,
                                                    const CancellationToken &cancellation = {});

AXK_API Result<PackagePublication> publish_portable_package(const PackageBuild &build,
                                                            const std::filesystem::path &output_path,
                                                            bool overwrite = false,
                                                            const CancellationToken &cancellation = {});

AXK_API Result<PackagePublication> export_portable_package(const MediaContainer &source,
                                                           std::span<const PackageRootSelector> roots,
                                                           const std::filesystem::path &output_path,
                                                           bool overwrite = false,
                                                           const CancellationToken &cancellation = {});

AXK_API Result<PortablePackage> open_portable_package(std::span<const std::byte> archive,
                                                      std::string_view filename = {});

AXK_API Result<PortablePackage> open_portable_package(const std::filesystem::path &path,
                                                      const CancellationToken &cancellation = {});

AXK_API Result<PortablePackage> inspect_portable_package(const std::filesystem::path &path,
                                                         const CancellationToken &cancellation = {});

AXK_API Result<PackageImportPlan> plan_package_import(const std::filesystem::path &target_path,
                                                      std::span<const PortablePackage> packages,
                                                      const PackageImportRequest &request,
                                                      const CancellationToken &cancellation = {});

AXK_AUDIO_API Result<PackageImportReport>
apply_package_import(const std::filesystem::path &target_path, std::span<const PortablePackage> packages,
                     const PackageImportPlan &plan, const std::filesystem::path &output_path, bool overwrite = false,
                     const CancellationToken &cancellation = {}, ProgressSink *progress = nullptr);

} // namespace axk
