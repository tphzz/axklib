#include "axklib/package.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/file_publication.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/utf8.hpp"
#include "package_internal.hpp"
#include "package_manifest_internal.hpp"

namespace axk {
namespace {

using package_internal::derive_kind;
using package_internal::lower_extension;
using package_internal::ManifestArchiveEntry;
using package_internal::maximum_package_file_bytes;
using package_internal::package_error;
using package_internal::recognized_extension;

Result<std::filesystem::path> resolve_output_path(const std::filesystem::path &requested, PackageKind kind) {
    const auto path_text = text::path_to_utf8(requested);
    if (!text::is_valid_utf8(path_text)) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package output path is not valid UTF-8")};
    }
    if (requested.empty() || requested.filename().empty() || requested.filename() == "." ||
        requested.filename() == "..") {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package output path must name a file")};
    }

    const auto required = required_package_extension(kind);
    const auto extension = lower_extension(text::path_to_utf8(requested.filename()));
    if (!extension.empty()) {
        if (extension != required) {
            const auto qualifier = recognized_extension(extension) ? "recognized package" : "unrelated";
            return std::unexpected{make_error(
                ErrorCode::invalid_argument, ErrorCategory::io,
                std::format("package output has {} extension {}; {} is required", qualifier, extension, required))};
        }
        return requested;
    }

    auto suffix = text::path_from_utf8(required);
    if (!suffix)
        return std::unexpected{suffix.error()};
    auto result = requested;
    result += suffix->native();
    return result;
}

Result<void> preflight_output(const std::filesystem::path &path, bool overwrite) {
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not inspect package output path")};
    }
    if (!overwrite && exists) {
        ErrorContext context;
        context.source_path = text::path_to_utf8(path);
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "refusing to replace an existing package", std::move(context))};
    }
    return {};
}

Result<std::vector<std::byte>> read_package_reader(const RandomAccessReader &reader,
                                                   const CancellationToken &cancellation);

Result<std::vector<std::byte>> read_package_file(const std::filesystem::path &path,
                                                 const CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    return read_package_reader(**reader, cancellation);
}

Result<std::vector<std::byte>> read_package_reader(const RandomAccessReader &reader,
                                                   const CancellationToken &cancellation) {
    if (reader.size() > maximum_package_file_bytes ||
        reader.size() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                          "package file exceeds the configured archive limit")};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(reader.size()));
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (const auto read = reader.read_exact_at(0U, bytes); !read)
        return std::unexpected{read.error()};
    return bytes;
}

} // namespace

std::string_view package_root_kind_name(PackageRootKind kind) noexcept {
    switch (kind) {
    case PackageRootKind::volume:
        return "volume";
    case PackageRootKind::prog:
        return "prog";
    case PackageRootKind::sbac:
        return "sbac";
    case PackageRootKind::sbnk:
        return "sbnk";
    case PackageRootKind::smpl:
        return "smpl";
    case PackageRootKind::sequ:
        return "sequ";
    }
    return "volume";
}

std::string_view package_kind_name(PackageKind kind) noexcept {
    switch (kind) {
    case PackageKind::volume:
        return "volume";
    case PackageKind::program:
        return "program";
    case PackageKind::sbac:
        return "sbac";
    case PackageKind::sbnk:
        return "sbnk";
    case PackageKind::smpl:
        return "smpl";
    case PackageKind::sequence:
        return "sequence";
    case PackageKind::bundle:
        return "bundle";
    }
    return "bundle";
}

std::string_view required_package_extension(PackageKind kind) noexcept {
    switch (kind) {
    case PackageKind::volume:
        return ".axkvol";
    case PackageKind::program:
        return ".axkprg";
    case PackageKind::sbac:
        return ".axksbac";
    case PackageKind::sbnk:
        return ".axksbnk";
    case PackageKind::smpl:
        return ".axksmpl";
    case PackageKind::sequence:
        return ".axkseq";
    case PackageKind::bundle:
        return ".axkpkg";
    }
    return ".axkpkg";
}

std::string_view package_import_action_name(PackageImportObjectAction action) noexcept {
    switch (action) {
    case PackageImportObjectAction::reuse:
        return "reuse";
    case PackageImportObjectAction::rename:
        return "rename";
    case PackageImportObjectAction::relocate:
        return "relocate";
    case PackageImportObjectAction::insert:
        return "insert";
    case PackageImportObjectAction::conflict:
        return "conflict";
    }
    return "conflict";
}

std::string_view package_program_assignment_origin_name(PackageProgramAssignmentOrigin origin) noexcept {
    switch (origin) {
    case PackageProgramAssignmentOrigin::imported_program:
        return "imported-program";
    case PackageProgramAssignmentOrigin::existing_program:
        return "existing-program";
    }
    return "imported-program";
}

std::string_view package_program_assignment_disposition_name(PackageProgramAssignmentDisposition disposition) noexcept {
    switch (disposition) {
    case PackageProgramAssignmentDisposition::clear_assignment:
        return "clear-assignment";
    }
    return "clear-assignment";
}

Result<PortablePackage> open_portable_package(std::span<const std::byte> archive, std::string_view filename) {
    auto entries = package_internal::read_archive(archive);
    if (!entries)
        return std::unexpected{entries.error()};
    std::map<std::string, std::vector<std::byte>, std::less<>> entry_bytes;
    for (auto &entry : *entries)
        entry_bytes.emplace(std::move(entry.path), std::move(entry.bytes));
    const auto manifest_entry = entry_bytes.find("manifest.json");
    if (manifest_entry == entry_bytes.end())
        return std::unexpected{package_error("package manifest is absent")};
    std::map<std::string, ManifestArchiveEntry, std::less<>> descriptors;
    for (const auto &[path, bytes] : entry_bytes)
        descriptors.emplace(path, ManifestArchiveEntry{bytes.size(), &bytes});
    return package_internal::parse_package_manifest(manifest_entry->second, descriptors, true, filename);
}

Result<PackagePublication> publish_portable_package(const PackageBuild &build, const std::filesystem::path &output_path,
                                                    bool overwrite, const CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    const auto required = std::string{required_package_extension(build.package.kind)};
    if (build.required_extension != required) {
        return std::unexpected{package_error("package build required extension is inconsistent")};
    }
    auto resolved = resolve_output_path(output_path, build.package.kind);
    if (!resolved)
        return std::unexpected{resolved.error()};
    if (const auto available = preflight_output(*resolved, overwrite); !available)
        return std::unexpected{available.error()};
    auto verified = open_portable_package(build.archive, text::path_to_utf8(resolved->filename()));
    if (!verified)
        return std::unexpected{verified.error()};
    if (verified->package_id != build.package.package_id || verified->kind != build.package.kind) {
        return std::unexpected{package_error("package build metadata disagrees with its archive")};
    }

    std::error_code filesystem_error;
    if (!resolved->parent_path().empty())
        std::filesystem::create_directories(resolved->parent_path(), filesystem_error);
    if (filesystem_error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create package output directory")};
    }
    auto publication = detail::TemporaryPublication::create(*resolved);
    if (!publication)
        return std::unexpected{publication.error()};
    if (auto resized = publication->resize(build.archive.size()); !resized)
        return std::unexpected{resized.error()};
    if (auto written = publication->write_at(0U, build.archive); !written)
        return std::unexpected{written.error()};
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (const auto flushed = publication->flush(); !flushed)
        return std::unexpected{flushed.error()};
    auto reopened = open_portable_package(publication->path(), cancellation);
    if (!reopened)
        return std::unexpected{reopened.error()};
    if (reopened->package_id != build.package.package_id) {
        return std::unexpected{package_error("temporary package failed identity verification")};
    }
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (const auto available = preflight_output(*resolved, overwrite); !available)
        return std::unexpected{available.error()};
    const auto mode = overwrite ? detail::PublicationMode::replace_existing : detail::PublicationMode::create_only;
    auto published = publication->publish(mode);
    if (!published) {
        return std::unexpected{published.error()};
    }
    return PackagePublication{*resolved, build.package.package_id, build.package.kind,
                              static_cast<std::uint64_t>(build.archive.size()), std::move(*published)};
}

Result<PackagePublication> export_portable_package(const MediaContainer &source,
                                                   std::span<const PackageRootSelector> roots,
                                                   const std::filesystem::path &output_path, bool overwrite,
                                                   const CancellationToken &cancellation) {
    if (roots.empty()) {
        return std::unexpected{
            package_error("at least one package root selector is required", ErrorCode::invalid_argument)};
    }
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    const auto expected_kind = derive_kind(roots);
    auto resolved = resolve_output_path(output_path, expected_kind);
    if (!resolved)
        return std::unexpected{resolved.error()};
    if (const auto available = preflight_output(*resolved, overwrite); !available)
        return std::unexpected{available.error()};
    auto build = build_portable_package(source, roots, cancellation);
    if (!build)
        return std::unexpected{build.error()};
    if (build->package.kind != expected_kind) {
        return std::unexpected{package_error("resolved package roots changed the requested root kind")};
    }
    return publish_portable_package(*build, *resolved, overwrite, cancellation);
}

Result<PortablePackage> open_portable_package(const std::filesystem::path &path,
                                              const CancellationToken &cancellation) {
    const auto filename = text::path_to_utf8(path.filename());
    return open_portable_package(path, filename, cancellation);
}

Result<PortablePackage> open_portable_package(const std::filesystem::path &path, std::string_view filename,
                                              const CancellationToken &cancellation) {
    if (!text::is_valid_utf8(filename)) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package input path is not valid UTF-8")};
    }
    auto archive = read_package_file(path, cancellation);
    if (!archive)
        return std::unexpected{archive.error()};
    return open_portable_package(*archive, filename);
}

Result<PortablePackage> open_portable_package(const RandomAccessReader &reader, std::string_view filename,
                                              const CancellationToken &cancellation) {
    if (!text::is_valid_utf8(filename)) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package input path is not valid UTF-8")};
    }
    auto archive = read_package_reader(reader, cancellation);
    if (!archive)
        return std::unexpected{archive.error()};
    return open_portable_package(*archive, filename);
}

Result<PortablePackage> inspect_portable_package(const std::filesystem::path &path,
                                                 const CancellationToken &cancellation) {
    const auto filename = text::path_to_utf8(path.filename());
    return inspect_portable_package(path, filename, cancellation);
}

Result<PortablePackage> inspect_portable_package(const std::filesystem::path &path, std::string_view filename,
                                                 const CancellationToken &cancellation) {
    if (!text::is_valid_utf8(filename)) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package input path is not valid UTF-8")};
    }
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    auto inspected = package_internal::inspect_archive(**reader, cancellation);
    if (!inspected)
        return std::unexpected{inspected.error()};
    std::map<std::string, ManifestArchiveEntry, std::less<>> descriptors;
    for (const auto &entry : inspected->entries)
        descriptors.emplace(entry.path, ManifestArchiveEntry{entry.size, nullptr});
    return package_internal::parse_package_manifest(inspected->manifest.bytes, descriptors, false, filename);
}

Result<PortablePackage> inspect_portable_package(const RandomAccessReader &reader, std::string_view filename,
                                                 const CancellationToken &cancellation) {
    if (!text::is_valid_utf8(filename)) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "package input path is not valid UTF-8")};
    }
    auto inspected = package_internal::inspect_archive(reader, cancellation);
    if (!inspected)
        return std::unexpected{inspected.error()};
    std::map<std::string, ManifestArchiveEntry, std::less<>> descriptors;
    for (const auto &entry : inspected->entries)
        descriptors.emplace(entry.path, ManifestArchiveEntry{entry.size, nullptr});
    return package_internal::parse_package_manifest(inspected->manifest.bytes, descriptors, false, filename);
}

} // namespace axk
