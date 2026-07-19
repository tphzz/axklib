#include "axklib/sdk.hpp"
#include "axklib/sdk/version.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
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
#include "axklib/package.hpp"
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

PackageRootKind internal_root_kind(package_root_kind kind) {
    switch (kind) {
    case package_root_kind::volume:
        return PackageRootKind::volume;
    case package_root_kind::program:
        return PackageRootKind::prog;
    case package_root_kind::bank_group:
        return PackageRootKind::sbac;
    case package_root_kind::sample:
        return PackageRootKind::sbnk;
    case package_root_kind::wave_data:
        return PackageRootKind::smpl;
    }
    return PackageRootKind::volume;
}

std::string media_kind_name(MediaKind kind) {
    switch (kind) {
    case MediaKind::sfs:
        return "sfs";
    case MediaKind::fat12_floppy:
        return "fat12-floppy";
    case MediaKind::iso9660:
        return "iso9660";
    case MediaKind::standalone_object:
        return "standalone-object";
    }
    return "unknown";
}

result<std::vector<PackageRootSelector>> internal_roots(const std::vector<package_root_selector> &roots) {
    std::vector<PackageRootSelector> result_value;
    result_value.reserve(roots.size());
    for (const auto &root : roots) {
        if (root.partition_index && *root.partition_index > std::numeric_limits<std::uint8_t>::max())
            return invalid_argument("package root partition index is out of range");
        PackageRootSelector item;
        item.kind = internal_root_kind(root.kind);
        if (root.partition_index)
            item.partition_index = static_cast<std::uint8_t>(*root.partition_index);
        item.group_name = root.group_name;
        item.volume_name = root.volume_name;
        item.object_name = root.object_name;
        item.object_key = root.object_key;
        result_value.push_back(std::move(item));
    }
    return result_value;
}

package_issue_info public_package_issue(const PackageIssue &issue) { return {issue.code, issue.message, issue.fatal}; }

package_summary public_package_summary(const PortablePackage &package) {
    return {
        package.schema_version,
        package.package_id,
        std::string{package_kind_name(package.kind)},
        std::string{required_package_extension(package.kind)},
        package.source_media_kind,
        package.roots.size(),
        package.nodes.size(),
        package.relationships.size(),
        package.issues.size(),
        package.payloads_verified,
    };
}

package_action_info public_package_action(const PlannedPackageObject &object) {
    package_action_info result_value;
    result_value.action_id = object.action_id;
    result_value.package_index = object.package_index;
    result_value.root_index = object.root_index;
    result_value.package_id = object.package_id;
    result_value.node_id = object.node_id;
    result_value.object_type = object.object_type;
    result_value.source_name = object.source_name;
    result_value.destination_name = object.destination_name;
    result_value.partition_index = object.partition_index;
    result_value.group_name = object.group_name;
    result_value.volume_name = object.volume_name;
    result_value.raw_group = object.raw_group;
    result_value.raw_volume = object.raw_volume;
    result_value.actions.reserve(object.actions.size());
    for (const auto action : object.actions)
        result_value.actions.emplace_back(package_import_action_name(action));
    result_value.canonical_action_id = object.canonical_action_id;
    result_value.target_sfs_id = object.target_sfs_id;
    result_value.target_link_id = object.target_link_id;
    return result_value;
}

package_allocation_info public_package_allocation(const PackageAllocationDelta &allocation) {
    return {
        allocation.partition_index,
        allocation.group_name,
        allocation.volume_name,
        allocation.raw_group,
        allocation.raw_volume,
        allocation.inserted_object_count,
        allocation.reused_object_count,
        allocation.payload_clusters,
        allocation.payload_sectors,
        allocation.continuation_clusters,
        allocation.directory_growth_bytes,
        allocation.remaining_object_ids,
        allocation.remaining_clusters,
        allocation.projected_image_sectors,
        allocation.projected_image_size_bytes,
    };
}

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
    value.payload_size =
        static_cast<std::uint64_t>(item.object.header.header_size) + item.object.header.payload_bytes_0x1c;
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

result<page<content_node>> content_page(const std::shared_ptr<const image_state> &state, const std::string &parent_id,
                                        std::uint64_t offset, std::uint64_t limit) {
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

result<page<object_info>> object_page(const std::shared_ptr<const image_state> &state, std::uint64_t offset,
                                      std::uint64_t limit) {
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
        output.items.push_back(public_object(state->catalog.objects[static_cast<std::size_t>(begin + index)]));
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

result<page<relationship_info>> relationship_page(const std::shared_ptr<const image_state> &state, std::uint64_t offset,
                                                  std::uint64_t limit) {
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

result<page<validation_issue>> validation_issue_page(const std::shared_ptr<const image_state> &state,
                                                     std::uint64_t offset, std::uint64_t limit) {
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
        output.items.push_back({issue.code, validation_severity_name(issue.severity), issue.message, issue.sampler_path,
                                issue.object_key});
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

result<page<content_node>> image::content_children(const std::string &parent_id, std::uint64_t offset,
                                                   std::uint64_t limit, operation_context &context) const {
    return protect<page<content_node>>([&]() -> result<page<content_node>> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        const auto cancellation = context.impl_->cancellation.token().check();
        if (!cancellation)
            return public_error(cancellation.error());
        return content_page(impl_ ? impl_->state : nullptr, parent_id, offset, limit);
    });
}

result<page<object_info>> image::objects(std::uint64_t offset, std::uint64_t limit, operation_context &context) const {
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
        if (object_key.empty() || bin_count == 0U || bin_count > std::numeric_limits<std::size_t>::max())
            return invalid_argument("waveform key and nonzero bin count are required");
        const auto found = std::ranges::find(impl_->state->catalog.objects, object_key, &ObjectSnapshot::key);
        if (found == impl_->state->catalog.objects.end())
            return error{
                error_code::object_missing, error_category::object, "waveform is not part of this image session", {}};
        auto waveform = decode_waveform(impl_->state->container, *found, context.impl_->cancellation.token());
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

result<std::vector<std::uint8_t>> image::waveform_pcm(const std::string &object_key, operation_context &context) const {
    return protect<std::vector<std::uint8_t>>([&]() -> result<std::vector<std::uint8_t>> {
        if (!impl_ || !impl_->state)
            return invalid_argument("image session is not open");
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        if (object_key.empty())
            return invalid_argument("waveform key is required");
        const auto found = std::ranges::find(impl_->state->catalog.objects, object_key, &ObjectSnapshot::key);
        if (found == impl_->state->catalog.objects.end())
            return error{
                error_code::object_missing, error_category::object, "waveform is not part of this image session", {}};
        auto waveform = decode_waveform(impl_->state->container, *found, context.impl_->cancellation.token());
        if (!waveform)
            return public_error(waveform.error());
        std::vector<std::uint8_t> bytes;
        bytes.reserve(waveform->pcm.size());
        for (const auto byte : waveform->pcm)
            bytes.push_back(std::to_integer<std::uint8_t>(byte));
        return bytes;
    });
}

result<export_summary> image::export_audio(const std::string &utf8_output_directory, const export_options &options,
                                           operation_context &context) const {
    return protect<export_summary>([&]() -> result<export_summary> {
        if (!impl_ || !impl_->state)
            return invalid_argument("image session is not open");
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto directory = checked_path(utf8_output_directory, "output directory");
        if (!directory)
            return directory.error();
        auto plan = build_export_plan(impl_->state->container, impl_->state->catalog, impl_->state->graph,
                                      context.impl_->cancellation.token());
        if (!plan)
            return public_error(plan.error());
        auto audio = write_export_audio(*plan, *directory, options.overwrite, context.impl_->cancellation.token());
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

struct portable_package::impl {
    PortablePackage package;
    std::filesystem::path path;
};

portable_package::portable_package() = default;
portable_package::~portable_package() = default;
portable_package::portable_package(portable_package &&) noexcept = default;
portable_package &portable_package::operator=(portable_package &&) noexcept = default;

result<portable_package> portable_package::open(const std::string &utf8_path, operation_context &context) {
    return protect<portable_package>([&]() -> result<portable_package> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto path = checked_path(utf8_path, "package path");
        if (!path)
            return path.error();
        auto opened = inspect_portable_package(*path, context.impl_->cancellation.token());
        if (!opened)
            return public_error(opened.error());
        portable_package result_value;
        result_value.impl_ = std::make_unique<impl>(impl{std::move(*opened), std::move(*path)});
        return result_value;
    });
}

result<package_export_summary> portable_package::export_from(const std::string &utf8_source_path,
                                                             const std::vector<package_root_selector> &roots,
                                                             const std::string &utf8_output_path,
                                                             const write_options &options, operation_context &context) {
    return protect<package_export_summary>([&]() -> result<package_export_summary> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto source = checked_path(utf8_source_path, "package source path");
        if (!source)
            return source.error();
        auto output = checked_path(utf8_output_path, "package output path");
        if (!output)
            return output.error();
        auto selectors = internal_roots(roots);
        if (!selectors)
            return selectors.error();
        auto media = open_media(*source, context.impl_->cancellation.token());
        if (!media)
            return public_error(media.error());
        context.impl_->report(
            {ProgressPhase::exporting, 0U, 1U, "exporting portable package", text::path_to_utf8(*output)});
        auto published = export_portable_package(*media, *selectors, *output, options.overwrite,
                                                 context.impl_->cancellation.token());
        if (!published)
            return public_error(published.error());
        context.impl_->report({ProgressPhase::exporting, 1U, 1U, "exported portable package",
                               text::path_to_utf8(published->output_path)});
        return package_export_summary{
            text::path_to_utf8(published->output_path),
            published->package_id,
            std::string{package_kind_name(published->kind)},
            std::string{required_package_extension(published->kind)},
            published->size_bytes,
        };
    });
}

result<package_summary> portable_package::summary() const {
    return protect<package_summary>([&]() -> result<package_summary> {
        if (!impl_)
            return invalid_argument("portable package is not open");
        return public_package_summary(impl_->package);
    });
}

result<std::vector<package_issue_info>> portable_package::issues() const {
    return protect<std::vector<package_issue_info>>([&]() -> result<std::vector<package_issue_info>> {
        if (!impl_)
            return invalid_argument("portable package is not open");
        std::vector<package_issue_info> result_value;
        result_value.reserve(impl_->package.issues.size());
        std::ranges::transform(impl_->package.issues, std::back_inserter(result_value), public_package_issue);
        return result_value;
    });
}

result<void> portable_package::verify(operation_context &context) const {
    return protect<void>([&]() -> result<void> {
        if (!impl_)
            return invalid_argument("portable package is not open");
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        if (const auto checked = context.impl_->cancellation.token().check(); !checked)
            return public_error(checked.error());
        auto verified = open_portable_package(impl_->path, context.impl_->cancellation.token());
        if (!verified)
            return public_error(verified.error());
        if (verified->package_id != impl_->package.package_id)
            return invalid_argument("portable package changed after it was opened");
        return {};
    });
}

struct package_import_plan::impl {
    std::filesystem::path target_path;
    std::vector<PortablePackage> packages;
    PackageImportPlan plan;
    std::thread::id owner;
};

package_import_plan::package_import_plan() = default;
package_import_plan::~package_import_plan() = default;
package_import_plan::package_import_plan(package_import_plan &&) noexcept = default;
package_import_plan &package_import_plan::operator=(package_import_plan &&) noexcept = default;

result<package_import_plan> package_import_plan::create(const std::string &utf8_target_path,
                                                        const std::vector<std::string> &utf8_package_paths,
                                                        const package_import_request &request,
                                                        operation_context &context) {
    return protect<package_import_plan>([&]() -> result<package_import_plan> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto target = checked_path(utf8_target_path, "package import target path");
        if (!target)
            return target.error();
        if (utf8_package_paths.empty())
            return invalid_argument("at least one package path is required");

        std::vector<PortablePackage> packages;
        packages.reserve(utf8_package_paths.size());
        for (const auto &path_value : utf8_package_paths) {
            auto path = checked_path(path_value, "package path");
            if (!path)
                return path.error();
            auto package = open_portable_package(*path, context.impl_->cancellation.token());
            if (!package)
                return public_error(package.error());
            packages.push_back(std::move(*package));
        }

        PackageImportRequest internal_request;
        internal_request.root_destinations.reserve(request.root_destinations.size());
        for (const auto &destination : request.root_destinations) {
            if (destination.package_index > std::numeric_limits<std::size_t>::max() ||
                destination.root_index > std::numeric_limits<std::size_t>::max() ||
                (destination.partition_index &&
                 *destination.partition_index > std::numeric_limits<std::uint8_t>::max())) {
                return invalid_argument("package destination index is out of range");
            }
            PackageRootDestination item;
            item.package_index = static_cast<std::size_t>(destination.package_index);
            item.root_index = static_cast<std::size_t>(destination.root_index);
            if (destination.partition_index)
                item.partition_index = static_cast<std::uint8_t>(*destination.partition_index);
            item.group_name = destination.group_name;
            item.volume_name = destination.volume_name;
            item.raw_group = destination.raw_group;
            item.raw_volume = destination.raw_volume;
            item.create_destination = destination.create_destination;
            internal_request.root_destinations.push_back(std::move(item));
        }
        internal_request.policy.renames.reserve(request.renames.size());
        for (const auto &rename : request.renames) {
            if (rename.package_index > std::numeric_limits<std::size_t>::max())
                return invalid_argument("package rename index is out of range");
            internal_request.policy.renames.push_back(
                {static_cast<std::size_t>(rename.package_index), rename.node_id, rename.destination_name});
        }

        auto planned = plan_package_import(*target, packages, internal_request, context.impl_->cancellation.token());
        if (!planned)
            return public_error(planned.error());
        package_import_plan result_value;
        result_value.impl_ = std::make_unique<impl>(impl{
            std::move(*target),
            std::move(packages),
            std::move(*planned),
            std::this_thread::get_id(),
        });
        return result_value;
    });
}

result<package_import_summary> package_import_plan::summary() const {
    return protect<package_import_summary>([&]() -> result<package_import_summary> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        return package_import_summary{
            impl_->plan.schema_version,
            impl_->plan.plan_id,
            media_kind_name(impl_->plan.target_kind),
            impl_->plan.target_snapshot_id,
            impl_->plan.package_ids.size(),
            impl_->plan.destinations.size(),
            impl_->plan.objects.size(),
            impl_->plan.conflicts.size(),
            impl_->plan.warnings.size(),
            impl_->plan.valid(),
        };
    });
}

result<std::vector<package_issue_info>> package_import_plan::warnings() const {
    return protect<std::vector<package_issue_info>>([&]() -> result<std::vector<package_issue_info>> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        std::vector<package_issue_info> result_value;
        result_value.reserve(impl_->plan.warnings.size());
        std::ranges::transform(impl_->plan.warnings, std::back_inserter(result_value), public_package_issue);
        return result_value;
    });
}

result<std::vector<package_conflict_info>> package_import_plan::conflicts() const {
    return protect<std::vector<package_conflict_info>>([&]() -> result<std::vector<package_conflict_info>> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        std::vector<package_conflict_info> result_value;
        result_value.reserve(impl_->plan.conflicts.size());
        for (const auto &conflict : impl_->plan.conflicts) {
            result_value.push_back({
                conflict.code,
                conflict.message,
                conflict.package_index,
                conflict.root_index,
                conflict.package_id,
                conflict.node_id,
                conflict.partition_index,
                conflict.group_name,
                conflict.volume_name,
                conflict.raw_group,
                conflict.raw_volume,
            });
        }
        return result_value;
    });
}

result<std::vector<package_action_info>> package_import_plan::actions() const {
    return protect<std::vector<package_action_info>>([&]() -> result<std::vector<package_action_info>> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        std::vector<package_action_info> result_value;
        result_value.reserve(impl_->plan.objects.size());
        std::ranges::transform(impl_->plan.objects, std::back_inserter(result_value), public_package_action);
        return result_value;
    });
}

result<std::vector<package_allocation_info>> package_import_plan::allocation() const {
    return protect<std::vector<package_allocation_info>>([&]() -> result<std::vector<package_allocation_info>> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        std::vector<package_allocation_info> result_value;
        result_value.reserve(impl_->plan.allocation.size());
        std::ranges::transform(impl_->plan.allocation, std::back_inserter(result_value), public_package_allocation);
        return result_value;
    });
}

result<package_import_result> package_import_plan::apply(const std::string &utf8_output_path,
                                                         const write_options &options, operation_context &context) {
    return protect<package_import_result>([&]() -> result<package_import_result> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        if (impl_->owner != std::this_thread::get_id())
            return invalid_argument("package import plan used from a different thread");
        auto output = checked_path(utf8_output_path, "package import output path");
        if (!output)
            return output.error();
        auto applied =
            apply_package_import(impl_->target_path, impl_->packages, impl_->plan, *output, options.overwrite,
                                 context.impl_->cancellation.token(), context.impl_.get());
        if (!applied)
            return public_error(applied.error());
        return package_import_result{
            text::path_to_utf8(applied->output_path),
            applied->plan_id,
            applied->source_snapshot_id,
            applied->output_snapshot_id,
            applied->objects.size(),
            applied->applied,
        };
    });
}

snapshot::snapshot() = default;
snapshot::~snapshot() = default;
snapshot::snapshot(snapshot &&) noexcept = default;
snapshot &snapshot::operator=(snapshot &&) noexcept = default;

result<page<content_node>> snapshot::content_children(const std::string &parent_id, std::uint64_t offset,
                                                      std::uint64_t limit) const {
    return protect<page<content_node>>(
        [&]() { return content_page(impl_ ? impl_->state : nullptr, parent_id, offset, limit); });
}

result<page<object_info>> snapshot::objects(std::uint64_t offset, std::uint64_t limit) const {
    return protect<page<object_info>>([&]() { return object_page(impl_ ? impl_->state : nullptr, offset, limit); });
}

result<page<relationship_info>> snapshot::relationships(std::uint64_t offset, std::uint64_t limit) const {
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

result<page<validation_issue>> snapshot::validation_issues(std::uint64_t offset, std::uint64_t limit) const {
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

result<build_plan> build_plan::from_manifest(const std::string &utf8_manifest_path, operation_context &context) {
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
            output.impl_ =
                std::make_unique<impl>(impl{std::move(*manifest), std::move(*geometry), std::this_thread::get_id()});
        } else {
            auto media_manifest = load_media_build_manifest(*path);
            if (!media_manifest)
                return public_error(media_manifest.error());
            output.impl_ = std::make_unique<impl>(impl{std::move(*media_manifest), {}, std::this_thread::get_id()});
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
            auto written = write_hds_image(*hds, *path, options.overwrite, context.impl_->cancellation.token());
            if (!written)
                return public_error(written.error());
        } else {
            auto written = write_media_image(std::get<MediaBuildManifest>(impl_->manifest), *path, options.overwrite,
                                             context.impl_->cancellation.token());
            if (!written)
                return public_error(written.error());
        }
        return {};
    });
}

result<plan_summary> alteration::inspect(const std::string &utf8_source_path, const std::string &utf8_manifest_path,
                                         operation_context &context) {
    return protect<plan_summary>([&]() -> result<plan_summary> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto source = checked_path(utf8_source_path, "source image path");
        if (!source)
            return source.error();
        auto manifest_path = checked_path(utf8_manifest_path, "alteration manifest path");
        if (!manifest_path)
            return manifest_path.error();
        auto manifest = load_alteration_manifest(*manifest_path);
        if (!manifest)
            return public_error(manifest.error());
        auto inspection =
            inspect_hds_alteration(*source, *manifest, context.impl_->cancellation.token(), context.impl_.get());
        if (!inspection)
            return public_error(inspection.error());
        return plan_summary{0U, inspection->operations.size(), 0U, !inspection->operations.empty()};
    });
}

result<void> alteration::apply(const std::string &utf8_source_path, const std::string &utf8_manifest_path,
                               const std::string &utf8_output_path, const write_options &options,
                               operation_context &context) {
    return protect<void>([&]() -> result<void> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto source = checked_path(utf8_source_path, "source image path");
        if (!source)
            return source.error();
        auto manifest_path = checked_path(utf8_manifest_path, "alteration manifest path");
        if (!manifest_path)
            return manifest_path.error();
        auto manifest = load_alteration_manifest(*manifest_path);
        if (!manifest)
            return public_error(manifest.error());
        auto output = checked_path(utf8_output_path, "output image path");
        if (!output)
            return output.error();
        if (!options.overwrite && std::filesystem::exists(*output))
            return error{error_code::io_open_failed, error_category::io, "output image already exists", {}};
        auto altered = alter_hds(*source, *manifest, *output, context.impl_->cancellation.token(), context.impl_.get(),
                                 options.overwrite);
        if (!altered)
            return public_error(altered.error());
        return {};
    });
}

std::string sdk_version() { return version_string; }

build_info sdk_build_info() noexcept { return current_build_info(); }

std::string render_error(const error &failure) {
    ErrorContext context;
    context.source_path = failure.context.source_path;
    context.partition_index = failure.context.partition_index;
    context.volume_name = failure.context.volume_name;
    context.object_type = failure.context.object_type;
    context.object_name = failure.context.object_name;
    context.raw_offset = failure.context.raw_offset;
    return axk::render_error(Error{static_cast<ErrorCode>(failure.code), static_cast<ErrorCategory>(failure.category),
                                   failure.message, std::move(context)});
}

} // namespace axk
