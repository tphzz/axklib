#include "axklib/c_api.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/catalog.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

struct axk_context {
  mutable std::mutex mutex;
  std::string last_error;
  axk::Error last_native_error;
  axk_status last_status{AXK_STATUS_OK};
  axk::CancellationSource cancellation;
  axk_progress_callback progress_callback{};
  axk_progress_callback_v1 progress_callback_v1{};
  void *progress_user_data{};
  bool used{};
  std::atomic_uint callback_depth{};
};

struct ImageState {
  axk::Container container;
  axk::ObjectCatalog catalog;
  axk::RelationshipGraph graph;
  axk::ContentTree tree;
  axk::ValidationReport validation;
};

struct axk_image {
  std::shared_ptr<const ImageState> state;
};

struct axk_snapshot {
  std::shared_ptr<const ImageState> state;
};

struct NodeArena {
  std::string id;
  std::string type;
  std::string display_name;
  std::string object_key;
  std::string object_type;
  std::string quality;
  std::uint64_t child_count{};
};

struct axk_node_result {
  std::uint64_t total_count{};
  std::vector<NodeArena> nodes;
};

struct ObjectArena {
  std::string object_key;
  std::string object_type;
  std::string object_name;
  std::string partition_name;
  std::string volume_name;
  std::string category_name;
  std::uint32_t partition_index{};
  std::uint32_t sfs_id{};
  std::uint64_t payload_size{};
};

struct axk_object_result {
  std::uint64_t total_count{};
  std::vector<ObjectArena> objects;
};

struct axk_preview_result {
  axk::PreviewEnvelope envelope;
};

struct axk_buffer {
  std::vector<std::byte> bytes;
};

struct axk_build_plan {
  axk::HdsBuildManifest manifest;
  std::vector<axk::PartitionGeometry> geometry;
  std::thread::id owner;
};

struct axk_transaction {
  std::filesystem::path source;
  axk::AlterationManifest manifest;
  axk::TransactionPlan plan;
  std::thread::id owner;
};

namespace {

constexpr std::uint32_t abi_version = AXK_ABI_VERSION;

enum class HandleKind : std::uint8_t {
  context,
  image,
  snapshot,
  nodes,
  objects,
  preview,
  buffer,
  build_plan,
  transaction,
};

std::mutex handle_mutex;
std::unordered_map<const void *, HandleKind> handles;

template <typename T> T *track(T *handle, HandleKind kind) {
  const std::scoped_lock lock{handle_mutex};
  handles.emplace(handle, kind);
  return handle;
}

bool valid_handle(const void *handle, HandleKind kind) {
  if (handle == nullptr)
    return false;
  const std::scoped_lock lock{handle_mutex};
  const auto found = handles.find(handle);
  return found != handles.end() && found->second == kind;
}

bool release_handle(const void *handle, HandleKind kind) {
  if (handle == nullptr)
    return true;
  const std::scoped_lock lock{handle_mutex};
  const auto found = handles.find(handle);
  if (found == handles.end() || found->second != kind)
    return false;
  handles.erase(found);
  return true;
}

axk_string_view view(const std::string &value) { return {value.data(), value.size()}; }

std::string string(axk_string_view value) {
  return value.size == 0U ? std::string{} : std::string{value.data, value.size};
}

std::filesystem::path path_from_utf8(axk_string_view value) {
  const auto bytes = string(value);
  const std::u8string utf8{reinterpret_cast<const char8_t *>(bytes.data()),
                           reinterpret_cast<const char8_t *>(bytes.data() + bytes.size())};
  return std::filesystem::path{utf8};
}

axk_status fail(axk_context *context, axk_status status, std::string message) {
  if (context != nullptr) {
    const std::scoped_lock lock{context->mutex};
    context->last_error = std::move(message);
    context->last_status = status;
    context->last_native_error = {};
  }
  return status;
}

axk_status fail(axk_context *context, axk_status status, const axk::Error &error) {
  if (context != nullptr) {
    const std::scoped_lock lock{context->mutex};
    context->last_error = axk::render_error(error);
    context->last_status = status;
    context->last_native_error = error;
  }
  return status;
}

void succeed(axk_context *context) {
  if (context == nullptr)
    return;
  const std::scoped_lock lock{context->mutex};
  context->last_error.clear();
  context->last_status = AXK_STATUS_OK;
  context->last_native_error = {};
  context->used = true;
}

void begin_use(axk_context *context) {
  const std::scoped_lock lock{context->mutex};
  context->used = true;
}

bool callback_reentry(const axk_context *context) {
  return context->callback_depth.load(std::memory_order_relaxed) != 0U;
}

template <typename T> axk_status validate_struct(axk_context *context, const T *value) {
  if (value == nullptr)
    return fail(context, AXK_STATUS_INVALID_ARGUMENT, "versioned output structure is required");
  if (value->struct_size < sizeof(T))
    return fail(context, AXK_STATUS_STRUCT_TOO_SMALL, "versioned structure is too small");
  if ((value->abi_version >> 16U) != AXK_ABI_VERSION_MAJOR)
    return fail(context, AXK_STATUS_UNSUPPORTED_ABI, "unsupported C ABI major version");
  return AXK_STATUS_OK;
}

axk_status error_status(const axk::Error &error) {
  if (error.code == axk::ErrorCode::operation_cancelled)
    return AXK_STATUS_CANCELLED;
  if (error.code == axk::ErrorCode::io_open_failed ||
      error.code == axk::ErrorCode::io_read_failed) {
    if (error.message.find("already exists") != std::string::npos)
      return AXK_STATUS_OUTPUT_CONFLICT;
    return AXK_STATUS_NOT_FOUND;
  }
  return AXK_STATUS_FORMAT_ERROR;
}

const std::vector<axk::ContentNode> *find_children(const axk::ContentTree &tree,
                                                   std::string_view parent) {
  if (parent.empty())
    return &tree.roots;
  const std::vector<axk::ContentNode> *result{};
  const auto visit = [&](const auto &self, const std::vector<axk::ContentNode> &nodes) -> void {
    for (const auto &node : nodes) {
      if (node.node_id == parent) {
        result = &node.children;
        return;
      }
      self(self, node.children);
      if (result != nullptr)
        return;
    }
  };
  visit(visit, tree.roots);
  return result;
}

NodeArena arena_node(const axk::ContentNode &node) {
  return {
      node.node_id,         node.node_type,
      node.display_name,    node.object_key,
      node.object_type,     std::string{axk::relationship_quality_name(node.quality)},
      node.children.size(),
  };
}

std::string_view c_object_type(axk::ObjectType type) {
  switch (type) {
  case axk::ObjectType::smpl:
    return "SMPL";
  case axk::ObjectType::sbnk:
    return "SBNK";
  case axk::ObjectType::sbac:
    return "SBAC";
  case axk::ObjectType::prog:
    return "PROG";
  case axk::ObjectType::sequ:
    return "SEQU";
  case axk::ObjectType::prf3:
    return "PRF3";
  case axk::ObjectType::unknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

class CallbackProgressSink final : public axk::ProgressSink {
public:
  explicit CallbackProgressSink(axk_context *context) : context_(context) {
    const std::scoped_lock lock{context->mutex};
    callback_ = context->progress_callback;
    callback_v1_ = context->progress_callback_v1;
    user_data_ = context->progress_user_data;
  }

  void report(const axk::Progress &progress) noexcept override {
    context_->callback_depth.fetch_add(1U, std::memory_order_relaxed);
    try {
      if (callback_ != nullptr) {
        callback_(user_data_, static_cast<std::uint32_t>(progress.phase), progress.completed,
                  progress.total.value_or(0U), progress.total ? 1 : 0, view(progress.label));
      }
      if (callback_v1_ != nullptr) {
        const axk_progress_event event{sizeof(axk_progress_event),
                                       AXK_ABI_VERSION,
                                       static_cast<std::uint32_t>(progress.phase),
                                       progress.completed,
                                       progress.total.value_or(0U),
                                       progress.total ? 1 : 0,
                                       view(progress.label)};
        callback_v1_(user_data_, &event);
      }
    } catch (...) {
      // Foreign callbacks must never unwind through the C ABI or native writer.
    }
    context_->callback_depth.fetch_sub(1U, std::memory_order_relaxed);
  }

private:
  axk_progress_callback callback_{};
  axk_progress_callback_v1 callback_v1_{};
  void *user_data_{};
  axk_context *context_{};
};

} // namespace

extern "C" {

std::uint32_t axk_abi_version(void) { return abi_version; }
std::uint32_t axk_abi_version_major(void) { return AXK_ABI_VERSION_MAJOR; }
std::uint32_t axk_abi_version_minor(void) { return AXK_ABI_VERSION_MINOR; }

axk_status axk_context_create(axk_context **out_context) {
  if (out_context == nullptr || *out_context != nullptr)
    return AXK_STATUS_INVALID_ARGUMENT;
  try {
    *out_context = track(new axk_context{}, HandleKind::context);
    return AXK_STATUS_OK;
  } catch (...) {
    return AXK_STATUS_INTERNAL_ERROR;
  }
}

axk_status axk_context_destroy(axk_context **context) {
  if (context == nullptr)
    return AXK_STATUS_OK;
  if (!release_handle(*context, HandleKind::context)) {
    *context = nullptr;
    return AXK_STATUS_INVALID_HANDLE;
  }
  delete *context;
  *context = nullptr;
  return AXK_STATUS_OK;
}

axk_status axk_context_cancel(axk_context *context) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  context->cancellation.cancel();
  return AXK_STATUS_OK;
}

axk_status axk_context_reset_cancel(axk_context *context) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  context->cancellation.reset();
  succeed(context);
  return AXK_STATUS_OK;
}

axk_status axk_context_set_progress_callback(axk_context *context, axk_progress_callback callback,
                                             void *user_data) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  const std::scoped_lock lock{context->mutex};
  if (context->used)
    return AXK_STATUS_INVALID_ARGUMENT;
  context->progress_callback = callback;
  context->progress_callback_v1 = nullptr;
  context->progress_user_data = user_data;
  return AXK_STATUS_OK;
}

axk_status axk_context_set_progress_callback_v1(axk_context *context,
                                                axk_progress_callback_v1 callback,
                                                void *user_data) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  const std::scoped_lock lock{context->mutex};
  if (context->used)
    return AXK_STATUS_INVALID_ARGUMENT;
  context->progress_callback = nullptr;
  context->progress_callback_v1 = callback;
  context->progress_user_data = user_data;
  return AXK_STATUS_OK;
}

axk_string_view axk_context_last_error(const axk_context *context) {
  return !valid_handle(context, HandleKind::context) ? axk_string_view{}
                                                     : view(context->last_error);
}

axk_status axk_context_last_error_info(const axk_context *context, axk_error_info *out_error) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  const auto status = validate_struct(const_cast<axk_context *>(context), out_error);
  if (status != AXK_STATUS_OK)
    return status;
  const std::scoped_lock lock{context->mutex};
  const auto &error = context->last_native_error;
  *out_error = {sizeof(axk_error_info),
                AXK_ABI_VERSION,
                static_cast<std::uint32_t>(context->last_status),
                static_cast<std::uint32_t>(error.code),
                view(context->last_error),
                error.context.source_path ? view(*error.context.source_path) : axk_string_view{},
                error.context.volume_name ? view(*error.context.volume_name) : axk_string_view{},
                error.context.object_type ? view(*error.context.object_type) : axk_string_view{},
                error.context.object_name ? view(*error.context.object_name) : axk_string_view{},
                error.context.raw_offset.value_or(0U),
                error.context.raw_offset ? 1 : 0,
                error.context.partition_index
                    ? static_cast<std::int32_t>(*error.context.partition_index)
                    : -1};
  return AXK_STATUS_OK;
}

axk_status axk_image_open(axk_context *context, axk_string_view utf8_path, axk_image **out_image) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  begin_use(context);
  if (out_image == nullptr || *out_image != nullptr ||
      (utf8_path.data == nullptr && utf8_path.size != 0U) || utf8_path.size == 0U) {
    return fail(context, AXK_STATUS_INVALID_ARGUMENT, "image path and output handle are required");
  }
  try {
    axk::OpenOptions options;
    options.cancellation = context->cancellation.token();
    auto container = axk::open_image(path_from_utf8(utf8_path), options);
    if (!container) {
      return fail(context, error_status(container.error()), container.error());
    }
    auto catalog = axk::build_object_catalog(*container, 64U * 1024U * 1024U, options.cancellation);
    if (!catalog) {
      return fail(context, error_status(catalog.error()), catalog.error());
    }
    auto graph = axk::build_relationship_graph(*catalog);
    auto tree = axk::build_content_tree(*container, *catalog, graph);
    auto validation = axk::validate_semantics(*container, *catalog, graph);
    auto state = std::make_shared<ImageState>(ImageState{
        std::move(*container), std::move(*catalog), std::move(graph), std::move(tree),
        std::move(validation)});
    *out_image = track(new axk_image{std::move(state)}, HandleKind::image);
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status axk_image_close(axk_image **image) {
  if (image == nullptr)
    return AXK_STATUS_OK;
  if (!release_handle(*image, HandleKind::image)) {
    *image = nullptr;
    return AXK_STATUS_INVALID_HANDLE;
  }
  delete *image;
  *image = nullptr;
  return AXK_STATUS_OK;
}

axk_status axk_image_snapshot(const axk_image *image, axk_snapshot **out_snapshot) {
  if (!valid_handle(image, HandleKind::image))
    return AXK_STATUS_INVALID_HANDLE;
  if (out_snapshot == nullptr || *out_snapshot != nullptr)
    return AXK_STATUS_INVALID_ARGUMENT;
  try {
    *out_snapshot = track(new axk_snapshot{image->state}, HandleKind::snapshot);
    return AXK_STATUS_OK;
  } catch (...) {
    return AXK_STATUS_INTERNAL_ERROR;
  }
}

axk_status axk_snapshot_destroy(axk_snapshot **snapshot) {
  if (snapshot == nullptr)
    return AXK_STATUS_OK;
  if (!release_handle(*snapshot, HandleKind::snapshot)) {
    *snapshot = nullptr;
    return AXK_STATUS_INVALID_HANDLE;
  }
  delete *snapshot;
  *snapshot = nullptr;
  return AXK_STATUS_OK;
}

axk_status axk_image_content_children(axk_context *context, const axk_image *image,
                                      axk_string_view parent_node_id, std::uint64_t offset,
                                      std::uint64_t limit, axk_node_result **out_result) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  begin_use(context);
  if (!valid_handle(image, HandleKind::image))
    return fail(context, AXK_STATUS_INVALID_HANDLE, "image handle is stale or invalid");
  if (out_result == nullptr || *out_result != nullptr ||
      (parent_node_id.data == nullptr && parent_node_id.size != 0U) || limit == 0U) {
    return fail(context, AXK_STATUS_INVALID_ARGUMENT, "valid image, page, and output are required");
  }
  try {
    const auto *children = find_children(image->state->tree, string(parent_node_id));
    if (children == nullptr) {
      return fail(context, AXK_STATUS_NOT_FOUND, "content node is not part of this image session");
    }
    auto result = std::make_unique<axk_node_result>();
    result->total_count = children->size();
    const auto begin = std::min<std::uint64_t>(offset, children->size());
    const auto end = std::min<std::uint64_t>(children->size(), begin + limit);
    result->nodes.reserve(static_cast<std::size_t>(end - begin));
    for (auto index = begin; index < end; ++index) {
      result->nodes.push_back(arena_node((*children)[static_cast<std::size_t>(index)]));
    }
    *out_result = track(result.release(), HandleKind::nodes);
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status axk_snapshot_content_children(axk_context *context, const axk_snapshot *snapshot,
                                         axk_string_view parent_node_id, std::uint64_t offset,
                                         std::uint64_t limit, axk_node_result **out_result) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  begin_use(context);
  if (!valid_handle(snapshot, HandleKind::snapshot))
    return fail(context, AXK_STATUS_INVALID_HANDLE, "snapshot handle is stale or invalid");
  if (out_result == nullptr || *out_result != nullptr ||
      (parent_node_id.data == nullptr && parent_node_id.size != 0U) || limit == 0U) {
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "valid snapshot, page, and output are required");
  }
  try {
    const auto *children = find_children(snapshot->state->tree, string(parent_node_id));
    if (children == nullptr)
      return fail(context, AXK_STATUS_NOT_FOUND, "content node is not part of this snapshot");
    auto result = std::make_unique<axk_node_result>();
    result->total_count = children->size();
    const auto begin = std::min<std::uint64_t>(offset, children->size());
    const auto end = std::min<std::uint64_t>(children->size(), begin + limit);
    result->nodes.reserve(static_cast<std::size_t>(end - begin));
    for (auto index = begin; index < end; ++index)
      result->nodes.push_back(arena_node((*children)[static_cast<std::size_t>(index)]));
    *out_result = track(result.release(), HandleKind::nodes);
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status snapshot_objects(axk_context *context, const std::shared_ptr<const ImageState> &state,
                            std::uint64_t offset, std::uint64_t limit,
                            axk_object_result **out_result) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  begin_use(context);
  if (!state || out_result == nullptr || *out_result != nullptr ||
      limit == 0U)
    return fail(context, AXK_STATUS_INVALID_ARGUMENT, "valid snapshot, page, and output are required");
  try {
    auto result = std::make_unique<axk_object_result>();
    result->total_count = state->catalog.objects.size();
    const auto begin = std::min<std::uint64_t>(offset, state->catalog.objects.size());
    const auto end = std::min<std::uint64_t>(state->catalog.objects.size(), begin + limit);
    result->objects.reserve(static_cast<std::size_t>(end - begin));
    for (auto index = begin; index < end; ++index) {
      const auto &item = state->catalog.objects[static_cast<std::size_t>(index)];
      ObjectArena value;
      value.object_key = item.key;
      value.object_type = c_object_type(item.object.header.type);
      value.object_name = item.object.header.name;
      value.partition_index = item.partition.value;
      value.sfs_id = item.sfs_id.value;
      value.payload_size = static_cast<std::uint64_t>(item.object.header.header_size) +
                           item.object.header.payload_bytes_0x1c;
      if (item.placement) {
        value.partition_name = item.placement->partition_name;
        value.volume_name = item.placement->volume_name;
        value.category_name = item.placement->category_name;
      }
      result->objects.push_back(std::move(value));
    }
    *out_result = track(result.release(), HandleKind::objects);
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status axk_image_objects(axk_context *context, const axk_image *image, std::uint64_t offset,
                             std::uint64_t limit, axk_object_result **out_result) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  begin_use(context);
  return !valid_handle(image, HandleKind::image)
             ? fail(context, AXK_STATUS_INVALID_HANDLE, "image handle is stale or invalid")
             : snapshot_objects(context, image->state, offset, limit, out_result);
}

axk_status axk_snapshot_objects(axk_context *context, const axk_snapshot *snapshot,
                                std::uint64_t offset, std::uint64_t limit,
                                axk_object_result **out_result) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  begin_use(context);
  return !valid_handle(snapshot, HandleKind::snapshot)
             ? fail(context, AXK_STATUS_INVALID_HANDLE, "snapshot handle is stale or invalid")
             : snapshot_objects(context, snapshot->state, offset, limit, out_result);
}

axk_status axk_image_validation_summary(axk_context *context, const axk_image *image,
                                        axk_validation_summary *out_summary) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  begin_use(context);
  if (!valid_handle(image, HandleKind::image))
    return fail(context, AXK_STATUS_INVALID_HANDLE, "image handle is stale or invalid");
  if (out_summary == nullptr) {
    return fail(context, AXK_STATUS_INVALID_ARGUMENT, "image and validation output are required");
  }
  if (const auto status = validate_struct(context, out_summary); status != AXK_STATUS_OK)
    return status;
  std::uint64_t errors{};
  std::uint64_t warnings{};
  for (const auto &issue : image->state->validation.issues) {
    errors += issue.severity == axk::ValidationSeverity::error ? 1U : 0U;
    warnings += issue.severity == axk::ValidationSeverity::warning ? 1U : 0U;
  }
  *out_summary = {
      sizeof(axk_validation_summary),
      AXK_ABI_VERSION,
      image->state->validation.issues.size(),
      errors,
      warnings,
      image->state->validation.coverage.object_count,
      image->state->validation.coverage.relationship_count,
      image->state->validation.valid() ? 1 : 0,
  };
  succeed(context);
  return AXK_STATUS_OK;
}

axk_status axk_image_waveform_preview(axk_context *context, const axk_image *image,
                                      axk_string_view object_key, std::uint64_t bin_count,
                                      axk_preview_result **out_result) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  begin_use(context);
  if (!valid_handle(image, HandleKind::image))
    return fail(context, AXK_STATUS_INVALID_HANDLE, "image handle is stale or invalid");
  if (out_result == nullptr || *out_result != nullptr ||
      object_key.data == nullptr || object_key.size == 0U || bin_count == 0U ||
      bin_count > std::numeric_limits<std::size_t>::max()) {
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "waveform key, bins, and output are required");
  }
  try {
    const auto key = string(object_key);
    const auto found =
        std::ranges::find(image->state->catalog.objects, key, &axk::ObjectSnapshot::key);
    if (found == image->state->catalog.objects.end()) {
      return fail(context, AXK_STATUS_NOT_FOUND, "waveform is not part of this image session");
    }
    const auto waveform = axk::decode_waveform(image->state->container, *found);
    if (!waveform) {
      return fail(context, error_status(waveform.error()), waveform.error());
    }
    auto envelope = axk::build_preview_envelope(*waveform, static_cast<std::size_t>(bin_count));
    if (!envelope) {
      return fail(context, error_status(envelope.error()), envelope.error());
    }
    *out_result =
        track(new axk_preview_result{std::move(*envelope)}, HandleKind::preview);
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status axk_image_waveform_pcm(axk_context *context, const axk_image *image,
                                  axk_string_view object_key, axk_buffer **out_buffer) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  begin_use(context);
  if (!valid_handle(image, HandleKind::image))
    return fail(context, AXK_STATUS_INVALID_HANDLE, "image handle is stale or invalid");
  if (out_buffer == nullptr || *out_buffer != nullptr ||
      object_key.data == nullptr || object_key.size == 0U)
    return fail(context, AXK_STATUS_INVALID_ARGUMENT, "waveform key and output are required");
  try {
    const auto key = string(object_key);
    const auto found = std::ranges::find(image->state->catalog.objects, key,
                                         &axk::ObjectSnapshot::key);
    if (found == image->state->catalog.objects.end())
      return fail(context, AXK_STATUS_NOT_FOUND, "waveform is not part of this image session");
    auto waveform = axk::decode_waveform(image->state->container, *found);
    if (!waveform)
      return fail(context, error_status(waveform.error()), waveform.error());
    *out_buffer = track(new axk_buffer{std::move(waveform->pcm)}, HandleKind::buffer);
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status axk_image_export_audio(axk_context *context, const axk_image *image,
                                  axk_string_view utf8_output_directory, int overwrite,
                                  int include_sfz, std::uint64_t *out_written_file_count) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  if (callback_reentry(context))
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "audio export cannot be invoked from its progress callback");
  begin_use(context);
  if (!valid_handle(image, HandleKind::image))
    return fail(context, AXK_STATUS_INVALID_HANDLE, "image handle is stale or invalid");
  if (out_written_file_count == nullptr ||
      utf8_output_directory.data == nullptr || utf8_output_directory.size == 0U) {
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "image, output directory, and count are required");
  }
  try {
    const auto plan = axk::build_export_plan(image->state->container, image->state->catalog,
                                             image->state->graph);
    if (!plan)
      return fail(context, error_status(plan.error()), plan.error());
    const auto directory = path_from_utf8(utf8_output_directory);
    const auto audio = axk::write_export_audio(*plan, directory, overwrite != 0);
    if (!audio)
      return fail(context, error_status(audio.error()), audio.error());
    std::uint64_t count = audio->written_files.size();
    if (include_sfz != 0) {
      const auto sfz = axk::write_sfz(*plan, directory, overwrite != 0);
      if (!sfz)
        return fail(context, error_status(sfz.error()), sfz.error());
      count += sfz->written_files.size();
    }
    *out_written_file_count = count;
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status axk_hds_build_plan_create(axk_context *context, axk_string_view utf8_manifest_path,
                                     axk_build_plan **out_plan) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  if (callback_reentry(context))
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "build-plan creation cannot be invoked from a progress callback");
  begin_use(context);
  if (out_plan == nullptr || *out_plan != nullptr ||
      utf8_manifest_path.data == nullptr || utf8_manifest_path.size == 0U)
    return fail(context, AXK_STATUS_INVALID_ARGUMENT, "manifest path and output are required");
  try {
    auto manifest = axk::load_hds_build_manifest(path_from_utf8(utf8_manifest_path));
    if (!manifest)
      return fail(context, error_status(manifest.error()), manifest.error());
    auto geometry = axk::plan_hds_geometry(*manifest);
    if (!geometry)
      return fail(context, error_status(geometry.error()), geometry.error());
    *out_plan = track(new axk_build_plan{std::move(*manifest), std::move(*geometry),
                                         std::this_thread::get_id()},
                      HandleKind::build_plan);
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status axk_build_plan_summary(const axk_build_plan *plan, axk_plan_summary *out_summary) {
  if (!valid_handle(plan, HandleKind::build_plan))
    return AXK_STATUS_INVALID_HANDLE;
  if (out_summary == nullptr)
    return AXK_STATUS_INVALID_ARGUMENT;
  if (out_summary->struct_size < sizeof(axk_plan_summary))
    return AXK_STATUS_STRUCT_TOO_SMALL;
  if ((out_summary->abi_version >> 16U) != AXK_ABI_VERSION_MAJOR)
    return AXK_STATUS_UNSUPPORTED_ABI;
  *out_summary = {sizeof(axk_plan_summary), AXK_ABI_VERSION, plan->geometry.size(), 0U,
                  plan->manifest.size_bytes, 1};
  return AXK_STATUS_OK;
}

axk_status axk_build_plan_apply(axk_context *context, axk_build_plan *plan,
                                axk_string_view utf8_output_path,
                                const axk_write_options *options) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  if (callback_reentry(context))
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "build-plan mutation cannot be invoked from its progress callback");
  begin_use(context);
  if (!valid_handle(plan, HandleKind::build_plan))
    return fail(context, AXK_STATUS_INVALID_HANDLE, "build plan handle is stale or invalid");
  if (utf8_output_path.data == nullptr ||
      utf8_output_path.size == 0U)
    return fail(context, AXK_STATUS_INVALID_ARGUMENT, "build plan and output path are required");
  if (plan->owner != std::this_thread::get_id())
    return fail(context, AXK_STATUS_WRONG_THREAD, "build plan used from a different thread");
  if (const auto status = validate_struct(context, options); status != AXK_STATUS_OK)
    return status;
  try {
    auto written = axk::write_hds_image(plan->manifest, path_from_utf8(utf8_output_path),
                                        options->overwrite != 0, context->cancellation.token());
    if (!written)
      return fail(context, error_status(written.error()), written.error());
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status axk_build_plan_destroy(axk_build_plan **plan) {
  if (plan == nullptr)
    return AXK_STATUS_OK;
  if (!release_handle(*plan, HandleKind::build_plan)) {
    *plan = nullptr;
    return AXK_STATUS_INVALID_HANDLE;
  }
  delete *plan;
  *plan = nullptr;
  return AXK_STATUS_OK;
}

axk_status axk_hds_create(axk_context *context, axk_string_view utf8_manifest_path,
                          axk_string_view utf8_output_path, int overwrite,
                          std::uint64_t *out_partition_count) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  if (callback_reentry(context))
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "image creation cannot be invoked from its progress callback");
  begin_use(context);
  if (out_partition_count == nullptr || utf8_manifest_path.data == nullptr ||
      utf8_manifest_path.size == 0U || utf8_output_path.data == nullptr ||
      utf8_output_path.size == 0U) {
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "manifest, output path, and count are required");
  }
  try {
    const auto manifest = axk::load_hds_build_manifest(path_from_utf8(utf8_manifest_path));
    if (!manifest)
      return fail(context, error_status(manifest.error()), manifest.error());
    const auto written = axk::write_hds_image(*manifest, path_from_utf8(utf8_output_path),
                                              overwrite != 0, context->cancellation.token());
    if (!written)
      return fail(context, error_status(written.error()), written.error());
    *out_partition_count = written->partitions.size();
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status axk_hds_transaction_create(axk_context *context, axk_string_view utf8_source_path,
                                      axk_string_view utf8_manifest_path,
                                      axk_transaction **out_transaction) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  if (callback_reentry(context))
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "transaction creation cannot be invoked from a progress callback");
  begin_use(context);
  if (out_transaction == nullptr || *out_transaction != nullptr ||
      utf8_source_path.data == nullptr || utf8_source_path.size == 0U ||
      utf8_manifest_path.data == nullptr || utf8_manifest_path.size == 0U)
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "source, transaction manifest, and output are required");
  try {
    auto manifest = axk::load_alteration_manifest(path_from_utf8(utf8_manifest_path));
    if (!manifest)
      return fail(context, error_status(manifest.error()), manifest.error());
    auto source = path_from_utf8(utf8_source_path);
    auto plan = axk::plan_hds_alteration(source, *manifest, context->cancellation.token());
    if (!plan)
      return fail(context, error_status(plan.error()), plan.error());
    *out_transaction =
        track(new axk_transaction{std::move(source), std::move(*manifest), std::move(*plan),
                                  std::this_thread::get_id()},
              HandleKind::transaction);
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status axk_transaction_summary(const axk_transaction *transaction,
                                   axk_plan_summary *out_summary) {
  if (!valid_handle(transaction, HandleKind::transaction))
    return AXK_STATUS_INVALID_HANDLE;
  if (out_summary == nullptr)
    return AXK_STATUS_INVALID_ARGUMENT;
  if (out_summary->struct_size < sizeof(axk_plan_summary))
    return AXK_STATUS_STRUCT_TOO_SMALL;
  if ((out_summary->abi_version >> 16U) != AXK_ABI_VERSION_MAJOR)
    return AXK_STATUS_UNSUPPORTED_ABI;
  *out_summary = {sizeof(axk_plan_summary), AXK_ABI_VERSION, 0U,
                  transaction->plan.operations.size(), 0U, 1};
  return AXK_STATUS_OK;
}

axk_status axk_transaction_apply(axk_context *context, axk_transaction *transaction,
                                 axk_string_view utf8_output_path,
                                 const axk_write_options *options) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  if (callback_reentry(context))
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "transaction mutation cannot be invoked from its progress callback");
  begin_use(context);
  if (!valid_handle(transaction, HandleKind::transaction))
    return fail(context, AXK_STATUS_INVALID_HANDLE, "transaction handle is stale or invalid");
  if (utf8_output_path.data == nullptr ||
      utf8_output_path.size == 0U)
    return fail(context, AXK_STATUS_INVALID_ARGUMENT, "transaction and output path are required");
  if (transaction->owner != std::this_thread::get_id())
    return fail(context, AXK_STATUS_WRONG_THREAD, "transaction used from a different thread");
  if (const auto status = validate_struct(context, options); status != AXK_STATUS_OK)
    return status;
  try {
    if (options->overwrite == 0 && std::filesystem::exists(path_from_utf8(utf8_output_path)))
      return fail(context, AXK_STATUS_OUTPUT_CONFLICT, "output image already exists");
    CallbackProgressSink progress{context};
    auto altered = axk::alter_hds(transaction->source, transaction->manifest,
                                  path_from_utf8(utf8_output_path),
                                  context->cancellation.token(), &progress);
    if (!altered)
      return fail(context, error_status(altered.error()), altered.error());
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

axk_status axk_transaction_destroy(axk_transaction **transaction) {
  if (transaction == nullptr)
    return AXK_STATUS_OK;
  if (!release_handle(*transaction, HandleKind::transaction)) {
    *transaction = nullptr;
    return AXK_STATUS_INVALID_HANDLE;
  }
  delete *transaction;
  *transaction = nullptr;
  return AXK_STATUS_OK;
}

axk_status axk_hds_alter(axk_context *context, axk_string_view utf8_source_path,
                         axk_string_view utf8_manifest_path, axk_string_view utf8_output_path,
                         std::uint64_t *out_operation_count, int *out_applied) {
  if (!valid_handle(context, HandleKind::context))
    return AXK_STATUS_INVALID_HANDLE;
  if (callback_reentry(context))
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "image alteration cannot be invoked from its progress callback");
  begin_use(context);
  if (out_operation_count == nullptr || out_applied == nullptr ||
      utf8_source_path.data == nullptr || utf8_source_path.size == 0U ||
      utf8_manifest_path.data == nullptr || utf8_manifest_path.size == 0U ||
      (utf8_output_path.data == nullptr && utf8_output_path.size != 0U)) {
    return fail(context, AXK_STATUS_INVALID_ARGUMENT,
                "source, manifest, operation count, and applied output are required");
  }
  try {
    const auto manifest = axk::load_alteration_manifest(path_from_utf8(utf8_manifest_path));
    if (!manifest) {
      return fail(context, error_status(manifest.error()), manifest.error());
    }
    const auto output =
        utf8_output_path.size == 0U
            ? std::optional<std::filesystem::path>{}
            : std::optional<std::filesystem::path>{path_from_utf8(utf8_output_path)};
    CallbackProgressSink progress{context};
    const auto altered = axk::alter_hds(path_from_utf8(utf8_source_path), *manifest, output,
                                        context->cancellation.token(), &progress);
    if (!altered) {
      return fail(context, error_status(altered.error()), altered.error());
    }
    *out_operation_count = altered->operations.size();
    *out_applied = altered->applied ? 1 : 0;
    succeed(context);
    return AXK_STATUS_OK;
  } catch (const std::exception &exception) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(context, AXK_STATUS_INTERNAL_ERROR, "unexpected native exception");
  }
}

std::uint64_t axk_node_result_total_count(const axk_node_result *result) {
  return !valid_handle(result, HandleKind::nodes) ? 0U : result->total_count;
}

std::uint64_t axk_node_result_count(const axk_node_result *result) {
  return !valid_handle(result, HandleKind::nodes) ? 0U : result->nodes.size();
}

axk_status axk_node_result_at(const axk_node_result *result, std::uint64_t index,
                              axk_content_node *out_node) {
  if (!valid_handle(result, HandleKind::nodes))
    return AXK_STATUS_INVALID_HANDLE;
  if (out_node == nullptr || index >= result->nodes.size())
    return AXK_STATUS_INVALID_ARGUMENT;
  if (out_node->struct_size < sizeof(axk_content_node))
    return AXK_STATUS_STRUCT_TOO_SMALL;
  if ((out_node->abi_version >> 16U) != AXK_ABI_VERSION_MAJOR)
    return AXK_STATUS_UNSUPPORTED_ABI;
  const auto &node = result->nodes[static_cast<std::size_t>(index)];
  *out_node = {
      sizeof(axk_content_node), AXK_ABI_VERSION,  view(node.id),
      view(node.type),          view(node.display_name), view(node.object_key),
      view(node.object_type),   view(node.quality),      node.child_count,
  };
  return AXK_STATUS_OK;
}

axk_status axk_node_result_destroy(axk_node_result **result) {
  if (result == nullptr)
    return AXK_STATUS_OK;
  if (!release_handle(*result, HandleKind::nodes)) {
    *result = nullptr;
    return AXK_STATUS_INVALID_HANDLE;
  }
  delete *result;
  *result = nullptr;
  return AXK_STATUS_OK;
}

std::uint64_t axk_object_result_total_count(const axk_object_result *result) {
  return !valid_handle(result, HandleKind::objects) ? 0U : result->total_count;
}

std::uint64_t axk_object_result_count(const axk_object_result *result) {
  return !valid_handle(result, HandleKind::objects) ? 0U : result->objects.size();
}

axk_status axk_object_result_at(const axk_object_result *result, std::uint64_t index,
                                axk_object_info *out_object) {
  if (!valid_handle(result, HandleKind::objects))
    return AXK_STATUS_INVALID_HANDLE;
  if (out_object == nullptr || index >= result->objects.size())
    return AXK_STATUS_INVALID_ARGUMENT;
  if (out_object->struct_size < sizeof(axk_object_info))
    return AXK_STATUS_STRUCT_TOO_SMALL;
  if ((out_object->abi_version >> 16U) != AXK_ABI_VERSION_MAJOR)
    return AXK_STATUS_UNSUPPORTED_ABI;
  const auto &item = result->objects[static_cast<std::size_t>(index)];
  *out_object = {sizeof(axk_object_info),
                 AXK_ABI_VERSION,
                 view(item.object_key),
                 view(item.object_type),
                 view(item.object_name),
                 view(item.partition_name),
                 view(item.volume_name),
                 view(item.category_name),
                 item.partition_index,
                 item.sfs_id,
                 item.payload_size};
  return AXK_STATUS_OK;
}

axk_status axk_object_result_destroy(axk_object_result **result) {
  if (result == nullptr)
    return AXK_STATUS_OK;
  if (!release_handle(*result, HandleKind::objects)) {
    *result = nullptr;
    return AXK_STATUS_INVALID_HANDLE;
  }
  delete *result;
  *result = nullptr;
  return AXK_STATUS_OK;
}

std::uint64_t axk_preview_result_frame_count(const axk_preview_result *result) {
  return !valid_handle(result, HandleKind::preview) ? 0U : result->envelope.frame_count;
}

std::uint64_t axk_preview_result_count(const axk_preview_result *result) {
  return !valid_handle(result, HandleKind::preview) ? 0U : result->envelope.bins.size();
}

axk_status axk_preview_result_at(const axk_preview_result *result, std::uint64_t index,
                                 axk_preview_bin *out_bin) {
  if (!valid_handle(result, HandleKind::preview))
    return AXK_STATUS_INVALID_HANDLE;
  if (out_bin == nullptr || index >= result->envelope.bins.size())
    return AXK_STATUS_INVALID_ARGUMENT;
  if (out_bin->struct_size < sizeof(axk_preview_bin))
    return AXK_STATUS_STRUCT_TOO_SMALL;
  if ((out_bin->abi_version >> 16U) != AXK_ABI_VERSION_MAJOR)
    return AXK_STATUS_UNSUPPORTED_ABI;
  const auto &bin = result->envelope.bins[static_cast<std::size_t>(index)];
  *out_bin = {sizeof(axk_preview_bin), AXK_ABI_VERSION, bin.minimum, bin.maximum};
  return AXK_STATUS_OK;
}

axk_status axk_preview_result_destroy(axk_preview_result **result) {
  if (result == nullptr)
    return AXK_STATUS_OK;
  if (!release_handle(*result, HandleKind::preview)) {
    *result = nullptr;
    return AXK_STATUS_INVALID_HANDLE;
  }
  delete *result;
  *result = nullptr;
  return AXK_STATUS_OK;
}

const std::uint8_t *axk_buffer_data(const axk_buffer *buffer) {
  return !valid_handle(buffer, HandleKind::buffer) || buffer->bytes.empty()
             ? nullptr
             : reinterpret_cast<const std::uint8_t *>(buffer->bytes.data());
}

std::uint64_t axk_buffer_size(const axk_buffer *buffer) {
  return !valid_handle(buffer, HandleKind::buffer) ? 0U : buffer->bytes.size();
}

axk_status axk_buffer_destroy(axk_buffer **buffer) {
  if (buffer == nullptr)
    return AXK_STATUS_OK;
  if (!release_handle(*buffer, HandleKind::buffer)) {
    *buffer = nullptr;
    return AXK_STATUS_INVALID_HANDLE;
  }
  delete *buffer;
  *buffer = nullptr;
  return AXK_STATUS_OK;
}

} // extern "C"
