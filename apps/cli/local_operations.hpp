#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/application/contracts.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/package.hpp"

#include "schema/info_v1.hpp"
#include "schema/operations_v1.hpp"
#include "schema/package_v1.hpp"

namespace axk::app {
class Sandbox;
class UploadStore;
} // namespace axk::app

namespace axk::cli {

struct ReportResult {
    std::size_t source_count{};
    std::size_t loaded_count{};
    std::size_t failed_count{};
    std::size_t row_count{};
    std::size_t ambiguous_count{};
    std::size_t decode_issue_count{};
};

struct OrphanSummary {
    std::string source_path;
    std::size_t waveform_count{};
    std::size_t referenced_count{};
    std::size_t known_unreferenced_count{};
    std::size_t ambiguous_or_unresolved_count{};
};

struct ValidationResult {
    std::size_t issue_count{};
    bool failed{};
    std::string policy;
};

struct CorpusAuditResult {
    std::size_t loaded_count{};
    std::size_t failed_count{};
    std::size_t object_count{};
    std::size_t validation_issue_count{};
    std::size_t relationship_count{};
    std::size_t wave_smoke_decoded{};
    std::size_t wave_smoke_error_count{};
    bool validation_failed{};
};

struct ExtractionWarning {
    std::string code;
    std::string message;
};

struct ExtractionArtifact {
    std::string relative_path;
    std::string sha256;
};

struct ExtractionResult {
    std::size_t waveform_count{};
    std::size_t written_file_count{};
    std::size_t selection_graph_count{};
    std::size_t sfz_file_count{};
    std::size_t decode_error_count{};
    std::size_t load_error_count{};
    std::vector<ExtractionWarning> warnings;
    std::vector<ExtractionArtifact> artifacts;
};

struct ImagePartitionResult {
    std::uint32_t index{};
    std::string name;
    std::uint32_t start_sector{};
    std::uint32_t sector_count{};
    std::uint32_t cluster_count{};
    std::uint64_t free_kib{};
};

struct ImageWriteResult {
    std::uint64_t size_bytes{};
    std::size_t object_count{};
    std::uint64_t unused_tail_sectors{};
    std::vector<ImagePartitionResult> partitions;
};

struct LocalInfoSource {
    std::optional<app::FileRef> file;
    std::optional<app::DirectoryRef> object_directory;
};

// Adapts trusted CLI paths to the same sandbox references accepted by the
// application operations. The network server continues to use only its
// explicitly configured roots.
class LocalOperationRuntime {
  public:
    [[nodiscard]] static app::Result<std::unique_ptr<LocalOperationRuntime>>
    create(std::span<const std::filesystem::path> paths);

    ~LocalOperationRuntime();
    LocalOperationRuntime(const LocalOperationRuntime &) = delete;
    LocalOperationRuntime &operator=(const LocalOperationRuntime &) = delete;
    LocalOperationRuntime(LocalOperationRuntime &&) = delete;
    LocalOperationRuntime &operator=(LocalOperationRuntime &&) = delete;

    [[nodiscard]] app::Result<app::FileRef> file_ref(const std::filesystem::path &path) const;
    [[nodiscard]] app::Result<app::DirectoryRef> directory_ref(const std::filesystem::path &path) const;
    [[nodiscard]] app::FileRef scratch_file_ref(std::string filename) const;
    [[nodiscard]] app::Result<std::filesystem::path> resolve_file(const app::FileRef &reference) const;

    [[nodiscard]] app::Result<ReportResult> report(std::string_view operation_id, std::span<const app::FileRef> sources,
                                                   const app::DirectoryRef &destination, bool overwrite,
                                                   bool strict = false, bool include_payloads = false,
                                                   bool pretty = false,
                                                   const std::optional<std::string> &object_type = std::nullopt) const;
    [[nodiscard]] app::Result<std::vector<OrphanSummary>>
    report_orphans(std::span<const app::FileRef> sources, const app::DirectoryRef &destination, bool overwrite) const;
    [[nodiscard]] app::Result<ValidationResult> report_validation(std::span<const app::FileRef> sources,
                                                                  const app::DirectoryRef &destination,
                                                                  const std::optional<app::DirectoryRef> &exports,
                                                                  std::string_view policy, bool overwrite) const;
    [[nodiscard]] app::Result<CorpusAuditResult> corpus_audit(std::span<const app::FileRef> sources,
                                                              const app::DirectoryRef &destination,
                                                              std::string_view policy, std::size_t wave_smoke_limit,
                                                              bool skip_wave_smoke, bool overwrite) const;
    [[nodiscard]] app::Result<schema::info_v1::InfoOutput> info(std::span<const LocalInfoSource> sources, bool strict,
                                                                bool include_default_programs) const;
    [[nodiscard]] app::Result<ExtractionResult> extract(bool sfz, std::span<const app::FileRef> sources,
                                                        const app::DirectoryRef &destination, std::string_view scope,
                                                        std::span<const std::string> selectors, std::string_view stereo,
                                                        bool overwrite, bool strict) const;

    [[nodiscard]] app::Result<schema::package_v1::PackageOutput>
    package_export(const app::FileRef &source, const app::FileRef &output, std::span<const PackageRootSelector> roots,
                   bool overwrite) const;
    [[nodiscard]] app::Result<schema::package_v1::PackageOutput>
    package_inspect(const std::filesystem::path &package_path, const app::FileRef &package, bool verify) const;
    [[nodiscard]] app::Result<schema::package_v1::PlanOutput>
    package_import(const std::filesystem::path &target_path, std::span<const std::filesystem::path> package_paths,
                   const app::FileRef &target, const app::FileRef &output, std::span<const app::FileRef> packages,
                   const axk::PackageImportRequest &request, bool apply, bool overwrite) const;

  private:
    friend app::Result<ImageWriteResult> create_image(std::string_view kind, const std::filesystem::path &manifest_path,
                                                      const std::filesystem::path &output_path, bool overwrite);
    friend app::Result<schema::operations_v1::AlterationOutput>
    alter_image(const std::filesystem::path &source_path, const std::filesystem::path &manifest_path,
                const std::optional<std::filesystem::path> &output_path);

    struct RootMapping;

    LocalOperationRuntime(std::vector<RootMapping> roots, std::filesystem::path staging_directory,
                          std::map<std::filesystem::path, std::string> display_paths,
                          std::unique_ptr<app::Sandbox> sandbox, std::unique_ptr<app::UploadStore> uploads,
                          app::OperationRegistry registry);

    [[nodiscard]] app::Result<app::FileRef> reference(const std::filesystem::path &path) const;
    [[nodiscard]] std::string display_path(const app::FileRef &reference) const;

    std::vector<RootMapping> roots_;
    std::filesystem::path staging_directory_;
    std::map<std::filesystem::path, std::string> display_paths_;
    std::unique_ptr<app::Sandbox> sandbox_;
    std::unique_ptr<app::UploadStore> uploads_;
    app::OperationRegistry registry_;
};

[[nodiscard]] app::Result<ImageWriteResult> create_image(std::string_view kind,
                                                         const std::filesystem::path &manifest_path,
                                                         const std::filesystem::path &output_path, bool overwrite);
[[nodiscard]] app::Result<schema::operations_v1::AlterationOutput>
alter_image(const std::filesystem::path &source_path, const std::filesystem::path &manifest_path,
            const std::optional<std::filesystem::path> &output_path);
[[nodiscard]] app::Result<std::string> create_manifest_document(const app::OperationRegistry &registry,
                                                                std::string_view kind);
[[nodiscard]] app::Result<std::string> alteration_manifest_document(const app::OperationRegistry &registry);

int report_application_failure(const app::Error &error);

} // namespace axk::cli
