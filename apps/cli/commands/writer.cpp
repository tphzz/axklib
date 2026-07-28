#include "handlers.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "local_operations.hpp"
#include "schema/operations_v1.hpp"
#include "support.hpp"

#include "axklib/alteration.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/write_operations.hpp"
#include "axklib/catalog.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/media.hpp"
#include "axklib/utf8.hpp"
#include "axklib/writer.hpp"

namespace axk::cli::commands {
namespace {

axk::Result<void> publish_manifest(const std::filesystem::path &output_path, std::string_view contents, bool overwrite,
                                   std::string_view kind) {
    std::error_code filesystem_error;
    if (!overwrite && std::filesystem::exists(output_path, filesystem_error)) {
        return std::unexpected{axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io,
                                               "refusing to replace existing " + std::string{kind} +
                                                   " manifest: " + axk::text::path_to_utf8(output_path))};
    }
    if (filesystem_error) {
        return std::unexpected{axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io,
                                               "could not inspect manifest output path")};
    }
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return std::unexpected{axk::make_error(axk::ErrorCode::io_open_failed, axk::ErrorCategory::io,
                                               "could not create manifest output directory")};
    }
    auto temporary = axk::detail::write_temporary_file(output_path, [&](const axk::detail::TemporaryFileSink &sink) {
        return sink(std::as_bytes(std::span{contents.data(), contents.size()}));
    });
    if (!temporary)
        return std::unexpected{temporary.error()};
    if (auto published = axk::detail::publish_temporary_file(*temporary, output_path, overwrite); !published) {
        axk::detail::discard_temporary_file(*temporary);
        return published;
    }
    return {};
}

} // namespace

int run_create_hds(const std::filesystem::path &manifest_path, const std::filesystem::path &output_path, bool overwrite,
                   bool dry_run) {
    const auto created = create_image("HDS", manifest_path, output_path, overwrite, !dry_run);
    if (!created)
        return report_application_failure(created.error());
    if (dry_run) {
        std::cout << "plan=create kind=" << created->plan.kind << " output=" << axk::text::path_to_utf8(output_path)
                  << " format=" << created->plan.format << " objects=" << created->plan.object_count
                  << " size_bytes=" << created->plan.size_bytes.value_or(0U)
                  << " partitions=" << created->plan.partition_count.value_or(0U)
                  << " overwrite=" << (created->plan.overwrite ? "true" : "false") << " valid=true\n";
        return exit_code(ExitStatus::success);
    }
    const auto &written = *created->written;
    std::cout << "image=" << axk::text::path_to_utf8(output_path) << " size_bytes=" << written.size_bytes
              << " partitions=" << written.partitions.size() << " objects=" << written.object_count
              << " unused_tail_sectors=" << written.unused_tail_sectors << '\n';
    for (const auto &partition : written.partitions) {
        std::cout << "partition=" << partition.index << " name='" << partition.name
                  << "' start_sector=" << partition.start_sector << " sector_count=" << partition.sector_count
                  << " cluster_count=" << partition.cluster_count << " free_kib=" << partition.free_kib << '\n';
    }
    return exit_code(ExitStatus::success);
}

int run_create_media(const std::filesystem::path &manifest_path, const std::filesystem::path &output_path,
                     std::string_view expected_format, bool overwrite, bool dry_run) {
    const auto service_kind = expected_format == "fat12_floppy" ? std::string_view{"FLOPPY"} : std::string_view{"ISO"};
    const auto created = create_image(service_kind, manifest_path, output_path, overwrite, !dry_run);
    if (!created)
        return report_application_failure(created.error());
    if (dry_run) {
        std::cout << "plan=create kind=" << created->plan.kind << " output=" << axk::text::path_to_utf8(output_path)
                  << " format=" << created->plan.format << " objects=" << created->plan.object_count
                  << " overwrite=" << (created->plan.overwrite ? "true" : "false") << " valid=true\n";
        return exit_code(ExitStatus::success);
    }
    const auto &written = *created->written;
    std::cout << "image=" << axk::text::path_to_utf8(output_path) << " format=" << expected_format
              << " size_bytes=" << written.size_bytes << " objects=" << written.object_count << '\n';
    return exit_code(ExitStatus::success);
}

int run_create_manifest(const axk::app::OperationRegistry &registry, std::string_view kind,
                        const std::filesystem::path &output_path, bool overwrite) {
    std::string service_kind;
    if (kind == "hds")
        service_kind = "HDS";
    else if (kind == "floppy")
        service_kind = "FLOPPY";
    else if (kind == "iso")
        service_kind = "ISO";
    if (service_kind.empty()) {
        std::cerr << "manifest kind must be hds, floppy, or iso\n";
        return exit_code(ExitStatus::invalid_request);
    }
    const auto manifest = create_manifest_document(registry, service_kind);
    if (!manifest)
        return report_application_failure(manifest.error());
    const auto written = publish_manifest(output_path, *manifest, overwrite, "build");
    if (!written) {
        std::cerr << axk::render_error(written.error()) << '\n';
        return exit_code(core_error_status(written.error()));
    }
    std::cout << "manifest=" << axk::text::path_to_utf8(output_path) << " kind=" << kind << '\n';
    return exit_code(ExitStatus::success);
}

int run_alter_manifest(const axk::app::OperationRegistry &registry, const std::filesystem::path &output_path,
                       bool overwrite) {
    const auto manifest = alteration_manifest_document(registry);
    if (!manifest)
        return report_application_failure(manifest.error());
    const auto written = publish_manifest(output_path, *manifest, overwrite, "alteration");
    if (!written) {
        std::cerr << axk::render_error(written.error()) << '\n';
        return exit_code(core_error_status(written.error()));
    }
    std::cout << "manifest=" << axk::text::path_to_utf8(output_path) << " kind=alteration\n";
    return exit_code(ExitStatus::success);
}

int run_alter_hds(const std::filesystem::path &source_path, const std::filesystem::path &manifest_path,
                  const std::optional<std::filesystem::path> &output_path, bool pretty) {
    const auto altered = alter_image(source_path, manifest_path, output_path);
    if (!altered)
        return report_application_failure(altered.error());
    const auto serialized = axk::cli::schema::operations_v1::serialize(*altered, pretty);
    if (!serialized) {
        std::cerr << axk::render_error(serialized.error()) << '\n';
        return exit_code(core_error_status(serialized.error()));
    }
    std::cout << *serialized << '\n';
    return exit_code(ExitStatus::success);
}

} // namespace axk::cli::commands
