#include "axklib/package.hpp"

#include <optional>

#include "axklib/package_archive.hpp"
#include "axklib/package_import_planning.hpp"

#include "package_import_internal.hpp"
#include "package_import_support.hpp"

namespace axk {
namespace {

using namespace package_import_internal;

static Result<PackageImportPlan>
plan_package_import_impl(std::shared_ptr<const RandomAccessReader> target_reader, std::filesystem::path target_path,
                         const MediaContainer *retained_target,
                         const package_import_internal::RetainedPackageImportTarget *retained_session,
                         std::span<const PortablePackage> packages, const PackageImportRequest &request,
                         bool revalidate_target, const CancellationToken &cancellation) {
    if (packages.empty())
        return std::unexpected{planner_error("package import requires at least one package")};
    if (!target_reader)
        return std::unexpected{planner_error("package import target reader is required")};
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};

    std::optional<MediaContainer> opened_target;
    const MediaContainer *target = retained_target;
    if (target == nullptr) {
        auto opened = open_media(target_reader, target_path, cancellation);
        if (!opened)
            return std::unexpected{opened.error()};
        opened_target.emplace(std::move(*opened));
        target = &*opened_target;
    }

    std::optional<package_internal::Sha256Digest> before;
    if (retained_session == nullptr || target->kind() != MediaKind::sfs) {
        auto digest = package_internal::sha256_reader(*target_reader, cancellation);
        if (!digest)
            return std::unexpected{digest.error()};
        before.emplace(*digest);
    } else if (retained_session->snapshot_id.size() != 64U) {
        return std::unexpected{planner_error("retained target snapshot identity is invalid")};
    }

    PackageImportPlan plan;
    plan.schema_version = "1.0";
    plan.target_kind = target->kind();
    plan.target_snapshot_id = before ? package_internal::hex_digest(*before) : retained_session->snapshot_id;
    plan.policy_digest = policy_digest(request.policy);
    for (std::size_t package_index = 0; package_index < packages.size(); ++package_index) {
        const auto &package = packages[package_index];
        if ((retained_session == nullptr || !retained_session->packages_verified)) {
            if (const auto verified = verify_portable_package(package); !verified)
                return std::unexpected{verified.error()};
        }
        plan.package_ids.push_back(package.package_id);
        for (const auto &issue : package.issues) {
            if (!issue.fatal)
                plan.warnings.push_back(issue);
        }
    }
    if (target->kind() != MediaKind::sfs && !request.policy.program_slot_assignments.empty()) {
        add_conflict(plan, "PROGRAM_SLOT_POLICY_UNSUPPORTED",
                     "explicit Program slot assignments are only supported for SFS package imports");
    }

    if (target->kind() == MediaKind::fat12_floppy) {
        return plan_fat12_import(*target_reader, packages, request, *target, std::move(plan), *before,
                                 revalidate_target, cancellation);
    }
    if (target->kind() == MediaKind::iso9660) {
        return plan_iso9660_import(*target_reader, packages, request, *target, std::move(plan), *before,
                                   revalidate_target, cancellation);
    }
    if (target->kind() != MediaKind::sfs) {
        add_conflict(plan, "TARGET_ADAPTER_UNSUPPORTED",
                     "portable package import is not yet implemented for this "
                     "target profile");
        plan.plan_id = package_import_internal::plan_identity(plan);
        return plan;
    }

    return package_import_internal::plan_sfs_import(std::move(target_reader), packages, request, target,
                                                    retained_session, std::move(plan), before, revalidate_target,
                                                    cancellation);
}

} // namespace

Result<PackageImportPlan> plan_package_import(std::shared_ptr<const RandomAccessReader> target_reader,
                                              std::filesystem::path target_path,
                                              std::span<const PortablePackage> packages,
                                              const PackageImportRequest &request,
                                              const CancellationToken &cancellation) {
    return plan_package_import_impl(std::move(target_reader), std::move(target_path), nullptr, nullptr, packages,
                                    request, true, cancellation);
}

Result<PackageImportPlan> package_import_internal::plan_package_import_retained(
    const RetainedPackageImportTarget &target, std::span<const PortablePackage> packages,
    const PackageImportRequest &request, const CancellationToken &cancellation) {
    if (target.media == nullptr)
        return std::unexpected{planner_error("retained package import target media is required")};
    return plan_package_import_impl(target.reader, target.path, target.media, &target, packages, request, false,
                                    cancellation);
}

Result<PackageImportPlan> plan_package_import(const std::filesystem::path &target_path,
                                              std::span<const PortablePackage> packages,
                                              const PackageImportRequest &request,
                                              const CancellationToken &cancellation) {
    auto target_reader = FileReader::open(target_path);
    if (!target_reader)
        return std::unexpected{target_reader.error()};
    return plan_package_import(std::move(*target_reader), target_path, packages, request, cancellation);
}

} // namespace axk
