#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "axklib/sdk/export.hpp"
#include "axklib/sdk/result.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
// Exported PIMPL classes keep unique_ptr construction and destruction out of
// line. C4251 does not account for this DLL-safe ownership pattern.
#pragma warning(disable : 4251)
#endif

namespace axk {

struct progress_event {
  std::uint32_t phase{};
  std::uint64_t completed{};
  std::optional<std::uint64_t> total;
  std::string label;
  std::optional<std::string> output_path;
};

class AXK_SDK_API progress_sink {
public:
  virtual ~progress_sink();
  virtual void report(const progress_event &event) = 0;
};

class AXK_SDK_API operation_context final {
public:
  operation_context();
  ~operation_context();
  operation_context(operation_context &&) noexcept;
  operation_context &operator=(operation_context &&) noexcept;
  operation_context(const operation_context &) = delete;
  operation_context &operator=(const operation_context &) = delete;

  void cancel() noexcept;
  void reset_cancel() noexcept;
  void set_progress_sink(progress_sink *sink) noexcept;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
  friend class image;
  friend class build_plan;
  friend class portable_package;
  friend class package_import_plan;
  friend class transaction;
};

struct content_node {
  std::string id;
  std::string type;
  std::string display_name;
  std::string object_key;
  std::string object_type;
  std::string quality;
  std::uint64_t child_count{};
};

struct object_info {
  std::string key;
  std::string type;
  std::string name;
  std::uint32_t partition_index{};
  std::string partition_name;
  std::string volume_name;
  std::string category_name;
  std::uint32_t sfs_id{};
  std::uint64_t payload_size{};
  std::optional<std::uint32_t> sample_rate;
  std::optional<std::uint32_t> root_key;
  std::optional<std::uint64_t> frame_count;
  std::optional<std::uint32_t> sample_width_bytes;
  std::optional<std::uint64_t> member_count;
};

struct relationship_info {
  std::string key;
  std::string source_key;
  std::optional<std::string> target_key;
  std::vector<std::string> candidate_keys;
  std::string type;
  std::string quality;
  std::string basis;
  std::string notes;
  std::string scope_key;
  std::optional<std::uint64_t> assignment_index;
  std::string assignment_name;
  std::string assignment_state;
};

struct validation_issue {
  std::string code;
  std::string severity;
  std::string message;
  std::string sampler_path;
  std::string object_key;
};

template <typename T> struct page {
  std::vector<T> items;
  std::uint64_t total_count{};
};

struct validation_summary {
  bool valid{};
  std::uint64_t issue_count{};
  std::uint64_t error_count{};
  std::uint64_t warning_count{};
  std::uint64_t object_count{};
  std::uint64_t relationship_count{};
};

struct preview_bin {
  std::int32_t minimum{};
  std::int32_t maximum{};
};

struct waveform_preview {
  std::uint64_t frame_count{};
  std::vector<preview_bin> bins;
};

struct export_options {
  bool overwrite{};
  bool include_sfz{};
};

struct export_summary {
  std::uint64_t written_file_count{};
  std::uint64_t warning_count{};
};

struct write_options {
  bool overwrite{};
};

struct plan_summary {
  std::uint64_t partition_count{};
  std::uint64_t operation_count{};
  std::uint64_t size_bytes{};
  bool applies_changes{};
};

enum class package_root_kind : std::uint8_t {
  volume,
  program,
  bank_group,
  sample_bank,
  sample,
  sequence
};

enum class package_waveform_reuse_scope : std::uint8_t { volume, hardware_proven_partition };

struct package_root_selector {
  package_root_kind kind{package_root_kind::volume};
  std::optional<std::uint32_t> partition_index;
  std::string group_name;
  std::string volume_name;
  std::string object_name;
  std::optional<std::string> object_key;
};

struct package_export_summary {
  std::string output_path;
  std::string package_id;
  std::string package_kind;
  std::string required_extension;
  std::uint64_t size_bytes{};
};

struct package_summary {
  std::string schema_version;
  std::string package_id;
  std::string package_kind;
  std::string required_extension;
  std::string source_media_kind;
  std::uint64_t root_count{};
  std::uint64_t object_count{};
  std::uint64_t relationship_count{};
  std::uint64_t issue_count{};
  bool payloads_verified{};
};

struct package_issue_info {
  std::string code;
  std::string message;
  bool fatal{};
};

struct package_root_destination {
  std::uint64_t package_index{};
  std::uint64_t root_index{};
  std::optional<std::uint32_t> partition_index;
  std::string group_name;
  std::string volume_name;
  std::string raw_group;
  std::string raw_volume;
  bool create_destination{};
};

struct package_node_rename {
  std::uint64_t package_index{};
  std::string node_id;
  std::string destination_name;
};

struct package_import_request {
  std::vector<package_root_destination> root_destinations;
  std::vector<package_node_rename> renames;
  package_waveform_reuse_scope sfs_waveform_reuse_scope{package_waveform_reuse_scope::volume};
};

struct package_conflict_info {
  std::string code;
  std::string message;
  std::optional<std::uint64_t> package_index;
  std::optional<std::uint64_t> root_index;
  std::string package_id;
  std::string node_id;
  std::optional<std::uint32_t> partition_index;
  std::string group_name;
  std::string volume_name;
  std::string raw_group;
  std::string raw_volume;
};

struct package_action_info {
  std::string action_id;
  std::uint64_t package_index{};
  std::uint64_t root_index{};
  std::string package_id;
  std::string node_id;
  std::string object_type;
  std::string source_name;
  std::string destination_name;
  std::uint32_t partition_index{};
  std::string group_name;
  std::string volume_name;
  std::string raw_group;
  std::string raw_volume;
  std::vector<std::string> actions;
  std::optional<std::string> canonical_action_id;
  std::optional<std::uint32_t> target_sfs_id;
  std::optional<std::uint32_t> target_link_id;
};

struct package_allocation_info {
  std::uint32_t partition_index{};
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
};

struct package_import_summary {
  std::string schema_version;
  std::string plan_id;
  std::string target_kind;
  std::string target_snapshot_id;
  std::uint64_t package_count{};
  std::uint64_t destination_count{};
  std::uint64_t object_count{};
  std::uint64_t conflict_count{};
  std::uint64_t warning_count{};
  bool valid{};
};

struct package_import_result {
  std::string output_path;
  std::string plan_id;
  std::string source_snapshot_id;
  std::string output_snapshot_id;
  std::uint64_t object_count{};
  bool applied{};
};

class snapshot;
class portable_package;
class package_import_plan;

class AXK_SDK_API image final {
public:
  image();
  ~image();
  image(image &&) noexcept;
  image &operator=(image &&) noexcept;
  image(const image &) = delete;
  image &operator=(const image &) = delete;

  static result<image> open(const std::string &utf8_path, operation_context &context);
  result<snapshot> make_snapshot() const;
  result<page<content_node>> content_children(const std::string &parent_id, std::uint64_t offset,
                                              std::uint64_t limit,
                                              operation_context &context) const;
  result<page<object_info>> objects(std::uint64_t offset, std::uint64_t limit,
                                    operation_context &context) const;
  result<page<relationship_info>> relationships(std::uint64_t offset, std::uint64_t limit,
                                                operation_context &context) const;
  result<validation_summary> validation(operation_context &context) const;
  result<page<validation_issue>> validation_issues(std::uint64_t offset, std::uint64_t limit,
                                                   operation_context &context) const;
  result<waveform_preview> preview(const std::string &object_key, std::uint64_t bin_count,
                                   operation_context &context) const;
  result<std::vector<std::uint8_t>> waveform_pcm(const std::string &object_key,
                                                 operation_context &context) const;
  result<export_summary> export_audio(const std::string &utf8_output_directory,
                                      const export_options &options,
                                      operation_context &context) const;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
  friend class snapshot;
};

class AXK_SDK_API portable_package final {
public:
  portable_package();
  ~portable_package();
  portable_package(portable_package &&) noexcept;
  portable_package &operator=(portable_package &&) noexcept;
  portable_package(const portable_package &) = delete;
  portable_package &operator=(const portable_package &) = delete;

  static result<portable_package> open(const std::string &utf8_path, operation_context &context);
  static result<package_export_summary> export_from(const std::string &utf8_source_path,
                                                    const std::vector<package_root_selector> &roots,
                                                    const std::string &utf8_output_path,
                                                    const write_options &options,
                                                    operation_context &context);
  result<package_summary> summary() const;
  result<std::vector<package_issue_info>> issues() const;
  result<void> verify(operation_context &context) const;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
  friend class package_import_plan;
};

class AXK_SDK_API package_import_plan final {
public:
  package_import_plan();
  ~package_import_plan();
  package_import_plan(package_import_plan &&) noexcept;
  package_import_plan &operator=(package_import_plan &&) noexcept;
  package_import_plan(const package_import_plan &) = delete;
  package_import_plan &operator=(const package_import_plan &) = delete;

  static result<package_import_plan> create(const std::string &utf8_target_path,
                                            const std::vector<std::string> &utf8_package_paths,
                                            const package_import_request &request,
                                            operation_context &context);
  result<package_import_summary> summary() const;
  result<std::vector<package_issue_info>> warnings() const;
  result<std::vector<package_conflict_info>> conflicts() const;
  result<std::vector<package_action_info>> actions() const;
  result<std::vector<package_allocation_info>> allocation() const;
  result<package_import_result> apply(const std::string &utf8_output_path,
                                      const write_options &options, operation_context &context);

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

class AXK_SDK_API snapshot final {
public:
  snapshot();
  ~snapshot();
  snapshot(snapshot &&) noexcept;
  snapshot &operator=(snapshot &&) noexcept;
  snapshot(const snapshot &) = delete;
  snapshot &operator=(const snapshot &) = delete;

  result<page<content_node>> content_children(const std::string &parent_id, std::uint64_t offset,
                                              std::uint64_t limit) const;
  result<page<object_info>> objects(std::uint64_t offset, std::uint64_t limit) const;
  result<page<relationship_info>> relationships(std::uint64_t offset, std::uint64_t limit) const;
  result<validation_summary> validation() const;
  result<page<validation_issue>> validation_issues(std::uint64_t offset, std::uint64_t limit) const;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
  friend class image;
};

class AXK_SDK_API build_plan final {
public:
  build_plan();
  ~build_plan();
  build_plan(build_plan &&) noexcept;
  build_plan &operator=(build_plan &&) noexcept;
  build_plan(const build_plan &) = delete;
  build_plan &operator=(const build_plan &) = delete;

  static result<build_plan> from_manifest(const std::string &utf8_manifest_path,
                                          operation_context &context);
  plan_summary summary() const noexcept;
  result<void> apply(const std::string &utf8_output_path, const write_options &options,
                     operation_context &context);

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

class AXK_SDK_API transaction final {
public:
  transaction();
  ~transaction();
  transaction(transaction &&) noexcept;
  transaction &operator=(transaction &&) noexcept;
  transaction(const transaction &) = delete;
  transaction &operator=(const transaction &) = delete;

  static result<transaction> from_manifest(const std::string &utf8_source_path,
                                           const std::string &utf8_manifest_path,
                                           operation_context &context);
  plan_summary summary() const noexcept;
  result<void> apply(const std::string &utf8_output_path, const write_options &options,
                     operation_context &context);

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

AXK_SDK_API std::string sdk_version();
AXK_SDK_API std::string render_error(const error &failure);

} // namespace axk

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
