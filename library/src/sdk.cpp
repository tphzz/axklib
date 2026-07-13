#include "axklib/sdk.hpp"
#include "axklib/sdk/version.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <ranges>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/catalog.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"
#include "axklib/sfs.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"
#include "axklib/writer.hpp"

namespace axk {
namespace {

error public_error(const Error &failure) {
  error_context context;
  context.source_path = failure.context.source_path;
  context.partition_index = failure.context.partition_index;
  context.volume_name = failure.context.volume_name;
  context.object_type = failure.context.object_type;
  context.object_name = failure.context.object_name;
  context.raw_offset = failure.context.raw_offset;
  return {
      static_cast<error_code>(failure.code),
      static_cast<error_category>(failure.category),
      failure.message,
      std::move(context),
  };
}

error invalid_argument(std::string message) {
  return {error_code::invalid_argument, error_category::internal, std::move(message), {}};
}

error internal_error(std::string message) {
  return {error_code::internal_invariant, error_category::internal, std::move(message), {}};
}

template <typename T, typename Function> result<T> protect(Function &&function) noexcept {
  try {
    return std::forward<Function>(function)();
  } catch (const std::bad_alloc &) {
    return internal_error("native allocation failed");
  } catch (const std::exception &exception) {
    return internal_error(exception.what());
  } catch (...) {
    return internal_error("unexpected native exception");
  }
}

result<std::filesystem::path> checked_path(const std::string &value, std::string label) {
  if (value.empty())
    return invalid_argument(std::move(label) + " is required");
  auto path = text::path_from_utf8(value);
  if (!path)
    return public_error(path.error());
  return std::move(*path);
}

std::string object_type_name(ObjectType type) {
  switch (type) {
  case ObjectType::smpl:
    return "SMPL";
  case ObjectType::sbnk:
    return "SBNK";
  case ObjectType::sbac:
    return "SBAC";
  case ObjectType::prog:
    return "PROG";
  case ObjectType::sequ:
    return "SEQU";
  case ObjectType::prf3:
    return "PRF3";
  case ObjectType::unknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

struct image_state {
  Container container;
  ObjectCatalog catalog;
  RelationshipGraph graph;
  ContentTree tree;
  ValidationReport validation;
};

const std::vector<ContentNode> *find_children(const ContentTree &tree, std::string_view parent) {
  if (parent.empty())
    return &tree.roots;
  const std::vector<ContentNode> *result_value{};
  const auto visit = [&](const auto &self, const std::vector<ContentNode> &nodes) -> void {
    for (const auto &node : nodes) {
      if (node.node_id == parent) {
        result_value = &node.children;
        return;
      }
      self(self, node.children);
      if (result_value != nullptr)
        return;
    }
  };
  visit(visit, tree.roots);
  return result_value;
}

content_node public_node(const ContentNode &node) {
  return {
      node.node_id,         node.node_type,   node.display_name,
      node.object_key,      node.object_type, std::string{relationship_quality_name(node.quality)},
      node.children.size(),
  };
}

object_info public_object(const ObjectSnapshot &item) {
  object_info value;
  value.key = item.key;
  value.type = object_type_name(item.object.header.type);
  value.name = item.object.header.name;
  value.partition_index = item.partition.value;
  value.sfs_id = item.sfs_id.value;
  value.payload_size = static_cast<std::uint64_t>(item.object.header.header_size) +
                       item.object.header.payload_bytes_0x1c;
  if (const auto *waveform = std::get_if<CurrentSmpl>(&item.object.payload)) {
    value.sample_rate = waveform->sample_rate.value;
    value.root_key = waveform->root_key.value;
    value.frame_count = waveform->wave_length_frames.value;
    value.sample_width_bytes = waveform->stored_sample_width_bytes.value;
  } else if (const auto *bank = std::get_if<CurrentSbnk>(&item.object.payload)) {
    value.member_count = bank->right.has_value() ? 2U : 1U;
  } else if (const auto *group = std::get_if<CurrentSbac>(&item.object.payload)) {
    value.member_count = group->active_slot_count;
  } else if (const auto *program = std::get_if<CurrentProg>(&item.object.payload)) {
    value.name = program->program_name;
    value.member_count = program->assignments.size();
  }
  if (item.placement) {
    value.partition_name = item.placement->partition_name;
    value.volume_name = item.placement->volume_name;
    value.category_name = item.placement->category_name;
  }
  return value;
}

result<page<content_node>> content_page(const std::shared_ptr<const image_state> &state,
                                        const std::string &parent_id, std::uint64_t offset,
                                        std::uint64_t limit) {
  if (!state)
    return invalid_argument("image session is not open");
  if (limit == 0U)
    return invalid_argument("content page limit must be nonzero");
  const auto *children = find_children(state->tree, parent_id);
  if (children == nullptr)
    return invalid_argument("content node is not part of this image session");
  page<content_node> output;
  output.total_count = children->size();
  const auto begin = std::min<std::uint64_t>(offset, children->size());
  const auto remaining = static_cast<std::uint64_t>(children->size()) - begin;
  const auto count = std::min(limit, remaining);
  output.items.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index)
    output.items.push_back(public_node((*children)[static_cast<std::size_t>(begin + index)]));
  return output;
}

result<page<object_info>> object_page(const std::shared_ptr<const image_state> &state,
                                      std::uint64_t offset, std::uint64_t limit) {
  if (!state)
    return invalid_argument("image session is not open");
  if (limit == 0U)
    return invalid_argument("object page limit must be nonzero");
  page<object_info> output;
  output.total_count = state->catalog.objects.size();
  const auto begin = std::min<std::uint64_t>(offset, state->catalog.objects.size());
  const auto remaining = static_cast<std::uint64_t>(state->catalog.objects.size()) - begin;
  const auto count = std::min(limit, remaining);
  output.items.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index)
    output.items.push_back(
        public_object(state->catalog.objects[static_cast<std::size_t>(begin + index)]));
  return output;
}

relationship_info public_relationship(const Relationship &item) {
  relationship_info value;
  value.key = item.key;
  value.source_key = item.source_key;
  value.target_key = item.target_key;
  value.candidate_keys = item.candidate_keys;
  value.type = item.type;
  value.quality = relationship_quality_name(item.quality);
  value.basis = item.basis;
  value.notes = item.notes;
  value.scope_key = item.scope_key;
  if (item.assignment_index)
    value.assignment_index = *item.assignment_index;
  value.assignment_name = item.assignment_name;
  value.assignment_state = assignment_state_name(item.assignment_state);
  return value;
}

result<page<relationship_info>> relationship_page(const std::shared_ptr<const image_state> &state,
                                                  std::uint64_t offset, std::uint64_t limit) {
  if (!state)
    return invalid_argument("image session is not open");
  if (limit == 0U)
    return invalid_argument("relationship page limit must be nonzero");
  page<relationship_info> output;
  output.total_count = state->graph.relationships.size();
  const auto begin = std::min<std::uint64_t>(offset, output.total_count);
  const auto count = std::min(limit, output.total_count - begin);
  output.items.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index)
    output.items.push_back(
        public_relationship(state->graph.relationships[static_cast<std::size_t>(begin + index)]));
  return output;
}

validation_summary public_validation(const image_state &state) {
  std::uint64_t errors{};
  std::uint64_t warnings{};
  for (const auto &issue : state.validation.issues) {
    errors += issue.severity == ValidationSeverity::error ? 1U : 0U;
    warnings += issue.severity == ValidationSeverity::warning ? 1U : 0U;
  }
  return {
      state.validation.valid(),
      state.validation.issues.size(),
      errors,
      warnings,
      state.validation.coverage.object_count,
      state.validation.coverage.relationship_count,
  };
}

std::string validation_severity_name(ValidationSeverity severity) {
  switch (severity) {
  case ValidationSeverity::info:
    return "info";
  case ValidationSeverity::warning:
    return "warning";
  case ValidationSeverity::error:
    return "error";
  }
  return "error";
}

result<page<validation_issue>>
validation_issue_page(const std::shared_ptr<const image_state> &state, std::uint64_t offset,
                      std::uint64_t limit) {
  if (!state)
    return invalid_argument("image session is not open");
  if (limit == 0U)
    return invalid_argument("validation issue page limit must be nonzero");
  page<validation_issue> output;
  output.total_count = state->validation.issues.size();
  const auto begin = std::min<std::uint64_t>(offset, output.total_count);
  const auto count = std::min(limit, output.total_count - begin);
  output.items.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count; ++index) {
    const auto &issue = state->validation.issues[static_cast<std::size_t>(begin + index)];
    output.items.push_back({issue.code, validation_severity_name(issue.severity), issue.message,
                            issue.sampler_path, issue.object_key});
  }
  return output;
}

} // namespace

progress_sink::~progress_sink() = default;

struct AXK_SDK_HIDDEN operation_context::impl final : ProgressSink {
  CancellationSource cancellation;
  std::mutex mutex;
  progress_sink *destination{};

  void report(const Progress &progress) noexcept override {
    const std::scoped_lock lock{mutex};
    if (destination == nullptr)
      return;
    progress_event event{
        static_cast<std::uint32_t>(progress.phase),
        progress.completed,
        progress.total,
        progress.label,
        progress.output_path,
    };
    try {
      destination->report(event);
    } catch (...) {
      // A progress observer cannot abort or unwind a native operation.
    }
  }
};

operation_context::operation_context() : impl_(std::make_unique<impl>()) {}
operation_context::~operation_context() = default;
operation_context::operation_context(operation_context &&) noexcept = default;
operation_context &operation_context::operator=(operation_context &&) noexcept = default;
void operation_context::cancel() noexcept {
  if (impl_)
    impl_->cancellation.cancel();
}
void operation_context::reset_cancel() noexcept {
  if (impl_)
    impl_->cancellation.reset();
}
void operation_context::set_progress_sink(progress_sink *sink) noexcept {
  if (!impl_)
    return;
  const std::scoped_lock lock{impl_->mutex};
  impl_->destination = sink;
}

struct image::impl {
  std::shared_ptr<const image_state> state;
};

struct snapshot::impl {
  std::shared_ptr<const image_state> state;
};

image::image() = default;
image::~image() = default;
image::image(image &&) noexcept = default;
image &image::operator=(image &&) noexcept = default;

result<image> image::open(const std::string &utf8_path, operation_context &context) {
  return protect<image>([&]() -> result<image> {
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    auto path = checked_path(utf8_path, "image path");
    if (!path)
      return path.error();
    OpenOptions options;
    options.cancellation = context.impl_->cancellation.token();
    options.progress = context.impl_.get();
    auto container = open_image(*path, options);
    if (!container)
      return public_error(container.error());
    auto catalog = build_object_catalog(*container, 64U * 1024U * 1024U, options.cancellation);
    if (!catalog)
      return public_error(catalog.error());
    auto graph = build_relationship_graph(*catalog);
    auto tree = build_content_tree(*container, *catalog, graph);
    auto validation = validate_semantics(*container, *catalog, graph);
    auto state = std::make_shared<image_state>(image_state{
        std::move(*container),
        std::move(*catalog),
        std::move(graph),
        std::move(tree),
        std::move(validation),
    });
    image output;
    output.impl_ = std::make_unique<impl>(impl{std::move(state)});
    return output;
  });
}

result<snapshot> image::make_snapshot() const {
  return protect<snapshot>([&]() -> result<snapshot> {
    if (!impl_ || !impl_->state)
      return invalid_argument("image session is not open");
    snapshot output;
    output.impl_ = std::make_unique<snapshot::impl>(snapshot::impl{impl_->state});
    return output;
  });
}

result<page<content_node>> image::content_children(const std::string &parent_id,
                                                   std::uint64_t offset, std::uint64_t limit,
                                                   operation_context &context) const {
  return protect<page<content_node>>([&]() -> result<page<content_node>> {
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    const auto cancellation = context.impl_->cancellation.token().check();
    if (!cancellation)
      return public_error(cancellation.error());
    return content_page(impl_ ? impl_->state : nullptr, parent_id, offset, limit);
  });
}

result<page<object_info>> image::objects(std::uint64_t offset, std::uint64_t limit,
                                         operation_context &context) const {
  return protect<page<object_info>>([&]() -> result<page<object_info>> {
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    const auto cancellation = context.impl_->cancellation.token().check();
    if (!cancellation)
      return public_error(cancellation.error());
    return object_page(impl_ ? impl_->state : nullptr, offset, limit);
  });
}

result<page<relationship_info>> image::relationships(std::uint64_t offset, std::uint64_t limit,
                                                     operation_context &context) const {
  return protect<page<relationship_info>>([&]() -> result<page<relationship_info>> {
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    const auto cancellation = context.impl_->cancellation.token().check();
    if (!cancellation)
      return public_error(cancellation.error());
    return relationship_page(impl_ ? impl_->state : nullptr, offset, limit);
  });
}

result<validation_summary> image::validation(operation_context &context) const {
  return protect<validation_summary>([&]() -> result<validation_summary> {
    if (!impl_ || !impl_->state)
      return invalid_argument("image session is not open");
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    const auto cancellation = context.impl_->cancellation.token().check();
    if (!cancellation)
      return public_error(cancellation.error());
    return public_validation(*impl_->state);
  });
}

result<page<validation_issue>> image::validation_issues(std::uint64_t offset, std::uint64_t limit,
                                                        operation_context &context) const {
  return protect<page<validation_issue>>([&]() -> result<page<validation_issue>> {
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    const auto cancellation = context.impl_->cancellation.token().check();
    if (!cancellation)
      return public_error(cancellation.error());
    return validation_issue_page(impl_ ? impl_->state : nullptr, offset, limit);
  });
}

result<waveform_preview> image::preview(const std::string &object_key, std::uint64_t bin_count,
                                        operation_context &context) const {
  return protect<waveform_preview>([&]() -> result<waveform_preview> {
    if (!impl_ || !impl_->state)
      return invalid_argument("image session is not open");
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    if (object_key.empty() || bin_count == 0U ||
        bin_count > std::numeric_limits<std::size_t>::max())
      return invalid_argument("waveform key and nonzero bin count are required");
    const auto found =
        std::ranges::find(impl_->state->catalog.objects, object_key, &ObjectSnapshot::key);
    if (found == impl_->state->catalog.objects.end())
      return error{error_code::object_missing,
                   error_category::object,
                   "waveform is not part of this image session",
                   {}};
    auto waveform =
        decode_waveform(impl_->state->container, *found, context.impl_->cancellation.token());
    if (!waveform)
      return public_error(waveform.error());
    auto envelope = build_preview_envelope(*waveform, static_cast<std::size_t>(bin_count));
    if (!envelope)
      return public_error(envelope.error());
    waveform_preview output;
    output.frame_count = envelope->frame_count;
    output.bins.reserve(envelope->bins.size());
    for (const auto &bin : envelope->bins)
      output.bins.push_back({bin.minimum, bin.maximum});
    return output;
  });
}

result<std::vector<std::uint8_t>> image::waveform_pcm(const std::string &object_key,
                                                      operation_context &context) const {
  return protect<std::vector<std::uint8_t>>([&]() -> result<std::vector<std::uint8_t>> {
    if (!impl_ || !impl_->state)
      return invalid_argument("image session is not open");
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    if (object_key.empty())
      return invalid_argument("waveform key is required");
    const auto found =
        std::ranges::find(impl_->state->catalog.objects, object_key, &ObjectSnapshot::key);
    if (found == impl_->state->catalog.objects.end())
      return error{error_code::object_missing,
                   error_category::object,
                   "waveform is not part of this image session",
                   {}};
    auto waveform =
        decode_waveform(impl_->state->container, *found, context.impl_->cancellation.token());
    if (!waveform)
      return public_error(waveform.error());
    std::vector<std::uint8_t> bytes;
    bytes.reserve(waveform->pcm.size());
    for (const auto byte : waveform->pcm)
      bytes.push_back(std::to_integer<std::uint8_t>(byte));
    return bytes;
  });
}

result<export_summary> image::export_audio(const std::string &utf8_output_directory,
                                           const export_options &options,
                                           operation_context &context) const {
  return protect<export_summary>([&]() -> result<export_summary> {
    if (!impl_ || !impl_->state)
      return invalid_argument("image session is not open");
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    auto directory = checked_path(utf8_output_directory, "output directory");
    if (!directory)
      return directory.error();
    auto plan = build_export_plan(impl_->state->container, impl_->state->catalog,
                                  impl_->state->graph, context.impl_->cancellation.token());
    if (!plan)
      return public_error(plan.error());
    auto audio = write_export_audio(*plan, *directory, options.overwrite,
                                    context.impl_->cancellation.token());
    if (!audio)
      return public_error(audio.error());
    export_summary output{audio->written_files.size(), audio->warnings.size()};
    if (options.include_sfz) {
      auto sfz = write_sfz(*plan, *directory, options.overwrite);
      if (!sfz)
        return public_error(sfz.error());
      output.written_file_count += sfz->written_files.size();
      output.warning_count += sfz->warnings.size();
    }
    return output;
  });
}

snapshot::snapshot() = default;
snapshot::~snapshot() = default;
snapshot::snapshot(snapshot &&) noexcept = default;
snapshot &snapshot::operator=(snapshot &&) noexcept = default;

result<page<content_node>> snapshot::content_children(const std::string &parent_id,
                                                      std::uint64_t offset,
                                                      std::uint64_t limit) const {
  return protect<page<content_node>>(
      [&]() { return content_page(impl_ ? impl_->state : nullptr, parent_id, offset, limit); });
}

result<page<object_info>> snapshot::objects(std::uint64_t offset, std::uint64_t limit) const {
  return protect<page<object_info>>(
      [&]() { return object_page(impl_ ? impl_->state : nullptr, offset, limit); });
}

result<page<relationship_info>> snapshot::relationships(std::uint64_t offset,
                                                        std::uint64_t limit) const {
  return protect<page<relationship_info>>(
      [&]() { return relationship_page(impl_ ? impl_->state : nullptr, offset, limit); });
}

result<validation_summary> snapshot::validation() const {
  return protect<validation_summary>([&]() -> result<validation_summary> {
    if (!impl_ || !impl_->state)
      return invalid_argument("image session is not open");
    return public_validation(*impl_->state);
  });
}

result<page<validation_issue>> snapshot::validation_issues(std::uint64_t offset,
                                                           std::uint64_t limit) const {
  return protect<page<validation_issue>>(
      [&]() { return validation_issue_page(impl_ ? impl_->state : nullptr, offset, limit); });
}

struct build_plan::impl {
  std::variant<HdsBuildManifest, MediaBuildManifest> manifest;
  std::vector<PartitionGeometry> geometry;
  std::thread::id owner;
};

build_plan::build_plan() = default;
build_plan::~build_plan() = default;
build_plan::build_plan(build_plan &&) noexcept = default;
build_plan &build_plan::operator=(build_plan &&) noexcept = default;

result<build_plan> build_plan::from_manifest(const std::string &utf8_manifest_path,
                                             operation_context &context) {
  return protect<build_plan>([&]() -> result<build_plan> {
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    auto path = checked_path(utf8_manifest_path, "build manifest path");
    if (!path)
      return path.error();
    build_plan output;
    if (auto manifest = load_hds_build_manifest(*path); manifest) {
      auto geometry = plan_hds_geometry(*manifest);
      if (!geometry)
        return public_error(geometry.error());
      output.impl_ = std::make_unique<impl>(
          impl{std::move(*manifest), std::move(*geometry), std::this_thread::get_id()});
    } else {
      auto media_manifest = load_media_build_manifest(*path);
      if (!media_manifest)
        return public_error(media_manifest.error());
      output.impl_ =
          std::make_unique<impl>(impl{std::move(*media_manifest), {}, std::this_thread::get_id()});
    }
    return output;
  });
}

plan_summary build_plan::summary() const noexcept {
  if (!impl_)
    return {};
  if (const auto *hds = std::get_if<HdsBuildManifest>(&impl_->manifest))
    return {impl_->geometry.size(), 0U, hds->size_bytes, true};
  const auto &media = std::get<MediaBuildManifest>(impl_->manifest);
  return {0U, 0U, media.format == MediaImageFormat::fat12_floppy ? 1'474'560U : 0U, true};
}

result<void> build_plan::apply(const std::string &utf8_output_path, const write_options &options,
                               operation_context &context) {
  return protect<void>([&]() -> result<void> {
    if (!impl_)
      return invalid_argument("build plan is not initialized");
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    if (impl_->owner != std::this_thread::get_id())
      return invalid_argument("build plan used from a different thread");
    auto path = checked_path(utf8_output_path, "output image path");
    if (!path)
      return path.error();
    if (const auto *hds = std::get_if<HdsBuildManifest>(&impl_->manifest)) {
      auto written =
          write_hds_image(*hds, *path, options.overwrite, context.impl_->cancellation.token());
      if (!written)
        return public_error(written.error());
    } else {
      auto written = write_media_image(std::get<MediaBuildManifest>(impl_->manifest), *path,
                                       options.overwrite, context.impl_->cancellation.token());
      if (!written)
        return public_error(written.error());
    }
    return {};
  });
}

struct transaction::impl {
  std::filesystem::path source;
  AlterationManifest manifest;
  TransactionPlan plan;
  std::thread::id owner;
};

transaction::transaction() = default;
transaction::~transaction() = default;
transaction::transaction(transaction &&) noexcept = default;
transaction &transaction::operator=(transaction &&) noexcept = default;

result<transaction> transaction::from_manifest(const std::string &utf8_source_path,
                                               const std::string &utf8_manifest_path,
                                               operation_context &context) {
  return protect<transaction>([&]() -> result<transaction> {
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    auto source = checked_path(utf8_source_path, "source image path");
    if (!source)
      return source.error();
    auto manifest_path = checked_path(utf8_manifest_path, "transaction manifest path");
    if (!manifest_path)
      return manifest_path.error();
    auto manifest = load_alteration_manifest(*manifest_path);
    if (!manifest)
      return public_error(manifest.error());
    auto plan = plan_hds_alteration(*source, *manifest, context.impl_->cancellation.token(),
                                    context.impl_.get());
    if (!plan)
      return public_error(plan.error());
    transaction output;
    output.impl_ = std::make_unique<impl>(impl{
        std::move(*source),
        std::move(*manifest),
        std::move(*plan),
        std::this_thread::get_id(),
    });
    return output;
  });
}

plan_summary transaction::summary() const noexcept {
  if (!impl_)
    return {};
  return {0U, impl_->plan.operations.size(), 0U, !impl_->plan.operations.empty()};
}

result<void> transaction::apply(const std::string &utf8_output_path, const write_options &options,
                                operation_context &context) {
  return protect<void>([&]() -> result<void> {
    if (!impl_)
      return invalid_argument("transaction is not initialized");
    if (!context.impl_)
      return invalid_argument("operation context is not initialized");
    if (impl_->owner != std::this_thread::get_id())
      return invalid_argument("transaction used from a different thread");
    auto output = checked_path(utf8_output_path, "output image path");
    if (!output)
      return output.error();
    if (!options.overwrite && std::filesystem::exists(*output))
      return error{
          error_code::io_open_failed, error_category::io, "output image already exists", {}};
    auto altered = alter_hds(impl_->source, impl_->manifest, *output,
                             context.impl_->cancellation.token(), context.impl_.get());
    if (!altered)
      return public_error(altered.error());
    return {};
  });
}

std::string sdk_version() { return version_string; }

std::string render_error(const error &failure) {
  ErrorContext context;
  context.source_path = failure.context.source_path;
  context.partition_index = failure.context.partition_index;
  context.volume_name = failure.context.volume_name;
  context.object_type = failure.context.object_type;
  context.object_name = failure.context.object_name;
  context.raw_offset = failure.context.raw_offset;
  return axk::render_error(Error{static_cast<ErrorCode>(failure.code),
                                 static_cast<ErrorCategory>(failure.category), failure.message,
                                 std::move(context)});
}

} // namespace axk
