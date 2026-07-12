#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "axklib/sdk/export.hpp"
#include "axklib/sdk/result.hpp"

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

class snapshot;

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
