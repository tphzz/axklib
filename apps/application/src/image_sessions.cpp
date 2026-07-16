#include "axklib/application/image_sessions.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <utility>

#include "axklib/application/secure_random.hpp"
#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"

namespace {

axk::app::Error session_error(std::string code, std::string message, bool retryable = false) {
    return {std::move(code), std::move(message), {}, retryable};
}

axk::app::Error core_error(const axk::Error &error, const axk::app::FileRef &source) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    context.relative_path = source.relative_path;
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "image_open_failed",
            error.message, std::move(context), false};
}

axk::app::Result<std::string> random_identifier(std::string_view prefix) {
    auto suffix = axk::app::secure_random_hex(16U);
    if (!suffix)
        return std::unexpected(suffix.error());
    return std::string{prefix} + std::move(*suffix);
}

std::string media_kind_name(axk::MediaKind kind) {
    switch (kind) {
    case axk::MediaKind::sfs:
        return "sfs";
    case axk::MediaKind::fat12_floppy:
        return "fat12";
    case axk::MediaKind::iso9660:
        return "iso9660";
    case axk::MediaKind::standalone_object:
        return "standalone-object";
    }
    return "unknown";
}

std::string object_format_name(axk::ObjectFormat format) {
    switch (format) {
    case axk::ObjectFormat::current:
        return "current";
    case axk::ObjectFormat::alternating_byte:
        return "alternating-byte";
    case axk::ObjectFormat::unknown:
        return "unknown";
    }
    return "unknown";
}

std::optional<std::string> mapped_id(const std::unordered_map<std::string, std::string> &ids, const std::string &key) {
    if (const auto found = ids.find(key); found != ids.end())
        return found->second;
    return std::nullopt;
}

} // namespace

struct axk::app::ImageSessionManager::Implementation {
    struct CursorSet {
        struct Position {
            std::size_t offset{};
            std::string scope;
        };

        std::mutex mutex;
        std::unordered_map<std::string, Position> positions;
        std::unordered_map<std::string, std::string> cursors;
    };

    struct Session {
        std::string image_id;
        std::string owner_id;
        FileRef source;
        std::string format;
        std::vector<ImageContentItem> content;
        std::unordered_map<std::string, std::vector<std::size_t>> content_children;
        std::vector<ImageObjectItem> objects;
        std::unordered_map<std::string, std::vector<std::size_t>> object_indices_by_type;
        std::vector<ImageRelationshipItem> relationships;
        std::vector<ImageValidationItem> validation;
        std::optional<MediaContainer> media;
        std::unordered_map<std::string, MediaObjectDescriptor> descriptors_by_id;
        std::size_t root_count{};
        std::mutex access_mutex;
        std::chrono::steady_clock::time_point last_access;
        CursorSet content_cursors;
        CursorSet object_cursors;
        CursorSet relationship_cursors;
        CursorSet validation_cursors;
    };

    const Sandbox &sandbox;
    std::size_t maximum_sessions;
    std::size_t maximum_page_size;
    std::chrono::seconds idle_retention;
    Clock clock;
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions;

    Implementation(const Sandbox &sandbox_value, std::size_t session_count, std::size_t page_size,
                   std::chrono::seconds retention, Clock now)
        : sandbox(sandbox_value), maximum_sessions(std::max<std::size_t>(session_count, 1U)),
          maximum_page_size(std::max<std::size_t>(page_size, 1U)), idle_retention(retention), clock(std::move(now)) {}

    void cleanup_locked() {
        const auto now = clock();
        std::erase_if(sessions, [&](const auto &item) {
            const auto &session = item.second;
            const std::scoped_lock access_lock{session->access_mutex};
            return session->last_access + idle_retention <= now;
        });
    }

    Result<std::shared_ptr<Session>> owned(std::string_view image_id, std::string_view owner_id) {
        std::shared_ptr<Session> result;
        {
            const std::scoped_lock lock{mutex};
            cleanup_locked();
            const auto found = sessions.find(std::string{image_id});
            if (found == sessions.end() || found->second->owner_id != owner_id)
                return std::unexpected(session_error("image_not_found", "image session does not exist"));
            result = found->second;
        }
        {
            const std::scoped_lock lock{result->access_mutex};
            result->last_access = clock();
        }
        return result;
    }

    template <typename Item>
    Result<ImagePage<Item>> page(const std::vector<Item> &items, CursorSet &cursors, std::size_t limit,
                                 std::optional<std::string_view> cursor, std::string_view scope = {},
                                 const std::vector<std::size_t> *indices = nullptr) const {
        if (limit == 0U || limit > maximum_page_size)
            return std::unexpected(session_error("invalid_page", "page limit is outside the configured range"));
        std::size_t offset{};
        {
            const std::scoped_lock lock{cursors.mutex};
            if (cursor) {
                const auto found = cursors.positions.find(std::string{*cursor});
                if (found == cursors.positions.end() || found->second.scope != scope)
                    return std::unexpected(session_error("invalid_cursor", "page cursor is invalid or stale"));
                offset = found->second.offset;
            }
        }
        const auto item_count = indices == nullptr ? items.size() : indices->size();
        if (offset > item_count)
            return std::unexpected(session_error("invalid_cursor", "page cursor is invalid or stale"));
        const auto count = std::min(limit, item_count - offset);
        ImagePage<Item> result;
        result.total_count = item_count;
        result.items.reserve(count);
        for (std::size_t index = offset; index < offset + count; ++index)
            result.items.push_back(indices == nullptr ? items[index] : items[indices->at(index)]);
        if (offset + count < item_count) {
            const auto next_offset = offset + count;
            const auto cursor_key = std::string{scope} + '\0' + std::to_string(next_offset);
            const std::scoped_lock lock{cursors.mutex};
            auto [found, inserted] = cursors.cursors.emplace(cursor_key, std::string{});
            if (inserted) {
                do {
                    auto identifier = random_identifier("cursor-");
                    if (!identifier)
                        return std::unexpected(identifier.error());
                    found->second = std::move(*identifier);
                } while (cursors.positions.contains(found->second));
                cursors.positions.emplace(found->second, CursorSet::Position{next_offset, std::string{scope}});
            }
            result.next_cursor = found->second;
        }
        return result;
    }
};

axk::app::ImageSessionManager::ImageSessionManager(const Sandbox &sandbox, std::size_t maximum_sessions,
                                                   std::size_t maximum_page_size, std::chrono::seconds idle_retention,
                                                   Clock clock)
    : implementation_(std::make_unique<Implementation>(sandbox, maximum_sessions, maximum_page_size, idle_retention,
                                                       std::move(clock))) {}

axk::app::ImageSessionManager::~ImageSessionManager() = default;
axk::app::ImageSessionManager::ImageSessionManager(ImageSessionManager &&) noexcept = default;
axk::app::ImageSessionManager &axk::app::ImageSessionManager::operator=(ImageSessionManager &&) noexcept = default;

axk::app::Result<axk::app::ImageSessionSummary>
axk::app::ImageSessionManager::open(const FileRef &source, std::string owner_id,
                                    const CancellationToken &cancellation) {
    if (owner_id.empty())
        return std::unexpected(session_error("invalid_owner", "image session owner is required"));
    const auto path = implementation_->sandbox.resolve_file(source);
    if (!path)
        return std::unexpected(path.error());
    const auto media = axk::open_media(*path, cancellation);
    if (!media)
        return std::unexpected(core_error(media.error(), source));
    auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata, 64U * 1024U * 1024U,
                                                cancellation);
    if (!inventory)
        return std::unexpected(core_error(inventory.error(), source));
    auto graph = axk::build_relationship_graph(inventory->catalog);
    auto tree = axk::build_content_tree(*media, inventory->catalog, graph);

    auto session = std::make_shared<Implementation::Session>();
    session->owner_id = std::move(owner_id);
    session->source = source;
    session->format = media_kind_name(media->kind());
    session->root_count = tree.roots.size();
    session->last_access = implementation_->clock();

    std::unordered_map<std::string, std::string> object_ids;
    object_ids.reserve(inventory->catalog.objects.size());
    for (const auto &object : inventory->catalog.objects) {
        auto identifier = random_identifier("object-");
        if (!identifier)
            return std::unexpected(identifier.error());
        object_ids.emplace(object.key, std::move(*identifier));
    }
    for (const auto &descriptor : inventory->objects)
        session->descriptors_by_id.emplace(object_ids.at(descriptor.key), descriptor);

    session->objects.reserve(inventory->catalog.objects.size());
    for (const auto &object : inventory->catalog.objects) {
        ImageObjectItem item;
        item.id = object_ids.at(object.key);
        item.type = object.object.header.raw_type;
        item.name = object.object.header.name;
        item.format = object_format_name(object.object.format);
        item.stored_size_bytes = session->descriptors_by_id.at(item.id).size;
        if (object.placement) {
            item.partition_index = object.partition.value;
            item.partition_name = object.placement->partition_name;
            item.volume_name = object.placement->volume_name;
            item.category_name = object.placement->category_name;
            item.entry_name = object.placement->entry_name;
        }
        if (const auto *waveform = std::get_if<axk::CurrentSmpl>(&object.object.payload)) {
            item.waveform = WaveformMetadata{.sample_rate = waveform->sample_rate.value,
                                             .sample_width_bytes = waveform->stored_sample_width_bytes.value,
                                             .root_key = waveform->root_key.value,
                                             .fine_tune_cents = waveform->fine_tune_cents.value,
                                             .loop_mode = waveform->loop_mode.value,
                                             .loop_mode_label = waveform->loop_mode_label,
                                             .frame_count = waveform->wave_length_frames.value,
                                             .loop_start_frame = waveform->loop_start_frame.value,
                                             .loop_length_frames = waveform->loop_length_frames.value};
        }
        session->object_indices_by_type[item.type].push_back(session->objects.size());
        session->objects.push_back(std::move(item));
    }

    session->relationships.reserve(graph.relationships.size());
    for (const auto &relationship : graph.relationships) {
        const auto source_id = mapped_id(object_ids, relationship.source_key);
        if (!source_id)
            continue;
        ImageRelationshipItem item;
        auto relationship_id = random_identifier("relationship-");
        if (!relationship_id)
            return std::unexpected(relationship_id.error());
        item.id = std::move(*relationship_id);
        item.source_object_id = *source_id;
        if (relationship.target_key)
            item.target_object_id = mapped_id(object_ids, *relationship.target_key);
        for (const auto &candidate : relationship.candidate_keys) {
            if (const auto candidate_id = mapped_id(object_ids, candidate))
                item.candidate_object_ids.push_back(*candidate_id);
        }
        item.type = relationship.type;
        item.quality = axk::relationship_quality_name(relationship.quality);
        item.basis = relationship.basis;
        item.notes = relationship.notes;
        item.assignment_index = relationship.assignment_index;
        item.assignment_name = relationship.assignment_name;
        item.assignment_state = axk::assignment_state_name(relationship.assignment_state);
        session->relationships.push_back(std::move(item));
    }

    const auto append_content = [&](const auto &self, const axk::ContentNode &node,
                                    const std::optional<std::string> &parent_id,
                                    std::size_t depth) -> axk::app::Result<void> {
        auto generated_id = random_identifier("content-");
        if (!generated_id)
            return std::unexpected(generated_id.error());
        const auto id = std::move(*generated_id);
        ImageContentItem item{.id = id,
                              .parent_id = parent_id,
                              .depth = depth,
                              .kind = node.node_type,
                              .display_name = node.display_name,
                              .child_count = node.children.size(),
                              .object_id = std::nullopt,
                              .object_type = std::nullopt,
                              .quality = std::string{axk::relationship_quality_name(node.quality)},
                              .basis = node.basis,
                              .notes = node.notes,
                              .details = node.details};
        if (!node.object_key.empty())
            item.object_id = mapped_id(object_ids, node.object_key);
        if (!node.object_type.empty())
            item.object_type = node.object_type;
        const auto item_index = session->content.size();
        session->content.push_back(std::move(item));
        session->content_children[parent_id.value_or("")].push_back(item_index);
        session->content_children.try_emplace(id);
        for (const auto &child : node.children) {
            if (auto appended = self(self, child, id, depth + 1U); !appended)
                return appended;
        }
        return {};
    };
    session->content_children.try_emplace("");
    for (const auto &root : tree.roots) {
        if (auto appended = append_content(append_content, root, std::nullopt, 0U); !appended)
            return std::unexpected(appended.error());
    }

    const auto append_validation = [&](std::string code, std::string severity, std::string message,
                                       std::string sampler_path, std::optional<std::string> object_id) {
        session->validation.push_back(
            {std::move(code), std::move(severity), std::move(message), std::move(sampler_path), std::move(object_id)});
    };
    for (const auto &issue : tree.issues) {
        append_validation(issue.code, issue.severity, issue.message, issue.sampler_path,
                          issue.object_key.empty() ? std::nullopt : mapped_id(object_ids, issue.object_key));
    }
    for (const auto &issue : media->validation_issues())
        append_validation(issue.code, "warning", issue.message, issue.sampler_path, std::nullopt);
    if (const auto *sfs = std::get_if<axk::Container>(&media->storage())) {
        const auto report = axk::validate_semantics(*sfs, inventory->catalog, graph);
        for (const auto &issue : report.issues) {
            std::string severity;
            switch (issue.severity) {
            case axk::ValidationSeverity::info:
                severity = "info";
                break;
            case axk::ValidationSeverity::warning:
                severity = "warning";
                break;
            case axk::ValidationSeverity::error:
                severity = "error";
                break;
            }
            append_validation(issue.code, std::move(severity), issue.message, issue.sampler_path,
                              issue.object_key.empty() ? std::nullopt : mapped_id(object_ids, issue.object_key));
        }
    }
    session->media.emplace(std::move(*media));

    do {
        auto image_id = random_identifier("image-");
        if (!image_id)
            return std::unexpected(image_id.error());
        session->image_id = std::move(*image_id);
        const std::scoped_lock lock{implementation_->mutex};
        implementation_->cleanup_locked();
        if (implementation_->sessions.size() >= implementation_->maximum_sessions)
            return std::unexpected(
                session_error("image_capacity_exhausted", "image session capacity is exhausted", true));
        if (!implementation_->sessions.contains(session->image_id)) {
            implementation_->sessions.emplace(session->image_id, session);
            break;
        }
    } while (true);
    return inspect(session->image_id, session->owner_id);
}

axk::app::Result<axk::app::ImageSessionSummary> axk::app::ImageSessionManager::inspect(std::string_view image_id,
                                                                                       std::string_view owner_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    ImageValidationSummary validation;
    for (const auto &issue : (*session)->validation) {
        if (issue.severity == "error")
            ++validation.error_count;
        else if (issue.severity == "warning")
            ++validation.warning_count;
        else
            ++validation.info_count;
    }
    return ImageSessionSummary{.image_id = (*session)->image_id,
                               .source = (*session)->source,
                               .format = (*session)->format,
                               .available_operations = {"images.content", "images.objects", "images.relationships",
                                                        "images.validation.issues", "images.preview"},
                               .root_count = (*session)->root_count,
                               .object_count = (*session)->objects.size(),
                               .relationship_count = (*session)->relationships.size(),
                               .validation = validation};
}

axk::app::Result<void> axk::app::ImageSessionManager::close(std::string_view image_id, std::string_view owner_id) {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
    const auto found = implementation_->sessions.find(std::string{image_id});
    if (found == implementation_->sessions.end())
        return {};
    if (found->second->owner_id != owner_id)
        return std::unexpected(session_error("image_not_found", "image session does not exist"));
    implementation_->sessions.erase(found);
    return {};
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageContentItem>>
axk::app::ImageSessionManager::content(std::string_view image_id, std::string_view owner_id, std::size_t limit,
                                       std::optional<std::string_view> cursor,
                                       std::optional<std::string_view> parent_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const auto scope = parent_id.value_or("");
    const auto children = (*session)->content_children.find(std::string{scope});
    if (children == (*session)->content_children.end())
        return std::unexpected(session_error("content_not_found", "content parent does not exist"));
    return implementation_->page((*session)->content, (*session)->content_cursors, limit, cursor, scope,
                                 &children->second);
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageObjectItem>>
axk::app::ImageSessionManager::objects(std::string_view image_id, std::string_view owner_id, std::size_t limit,
                                       std::optional<std::string_view> cursor,
                                       std::optional<std::string_view> object_type) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    if (!object_type)
        return implementation_->page((*session)->objects, (*session)->object_cursors, limit, cursor);
    const auto scope = "type:" + std::string{*object_type};
    const auto found = (*session)->object_indices_by_type.find(std::string{*object_type});
    if (found == (*session)->object_indices_by_type.end()) {
        static const std::vector<std::size_t> empty;
        return implementation_->page((*session)->objects, (*session)->object_cursors, limit, cursor, scope, &empty);
    }
    return implementation_->page((*session)->objects, (*session)->object_cursors, limit, cursor, scope, &found->second);
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageRelationshipItem>>
axk::app::ImageSessionManager::relationships(std::string_view image_id, std::string_view owner_id, std::size_t limit,
                                             std::optional<std::string_view> cursor) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    return implementation_->page((*session)->relationships, (*session)->relationship_cursors, limit, cursor);
}

axk::app::Result<axk::app::ImagePage<axk::app::ImageValidationItem>>
axk::app::ImageSessionManager::validation_issues(std::string_view image_id, std::string_view owner_id,
                                                 std::size_t limit, std::optional<std::string_view> cursor) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    return implementation_->page((*session)->validation, (*session)->validation_cursors, limit, cursor);
}

void axk::app::ImageSessionManager::cleanup() {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
}

bool axk::app::ImageSessionManager::root_in_use(std::string_view root_id) {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->cleanup_locked();
    return std::ranges::any_of(implementation_->sessions,
                               [&](const auto &entry) { return entry.second->source.root_id == root_id; });
}

axk::app::Result<axk::app::ImageWaveformPreview>
axk::app::ImageSessionManager::preview(std::string_view image_id, std::string_view owner_id, std::string_view object_id,
                                       std::size_t bin_count, const CancellationToken &cancellation) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    if (bin_count == 0U || bin_count > implementation_->maximum_page_size)
        return std::unexpected(session_error("invalid_preview", "preview bin count is outside the configured range"));
    const auto descriptor = (*session)->descriptors_by_id.find(std::string{object_id});
    if (descriptor == (*session)->descriptors_by_id.end())
        return std::unexpected(session_error("object_not_found", "image object does not exist"));
    const auto loaded = load_media_object(*(*session)->media, descriptor->second, 64U * 1024U * 1024U, cancellation);
    if (!loaded)
        return std::unexpected(core_error(loaded.error(), (*session)->source));
    const auto waveform = decode_waveform(*loaded);
    if (!waveform)
        return std::unexpected(core_error(waveform.error(), (*session)->source));
    const auto envelope = build_preview_envelope(*waveform, bin_count);
    if (!envelope)
        return std::unexpected(core_error(envelope.error(), (*session)->source));
    ImageWaveformPreview result{.object_id = std::string{object_id}, .frame_count = envelope->frame_count, .bins = {}};
    result.bins.reserve(envelope->bins.size());
    for (const auto &bin : envelope->bins)
        result.bins.push_back({bin.minimum, bin.maximum});
    return result;
}
