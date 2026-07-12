#pragma once

#include <stddef.h>
#include <stdint.h>

#define AXK_ABI_VERSION_MAJOR 1U
#define AXK_ABI_VERSION_MINOR 0U
#define AXK_ABI_VERSION ((AXK_ABI_VERSION_MAJOR << 16U) | AXK_ABI_VERSION_MINOR)

#if defined(_WIN32) && defined(AXK_C_SHARED_LIBRARY)
#if defined(AXK_C_EXPORTS)
#define AXK_C_API __declspec(dllexport)
#else
#define AXK_C_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(AXK_C_SHARED_LIBRARY)
#define AXK_C_API __attribute__((visibility("default")))
#else
#define AXK_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct axk_context axk_context;
typedef struct axk_image axk_image;
typedef struct axk_snapshot axk_snapshot;
typedef struct axk_node_result axk_node_result;
typedef struct axk_object_result axk_object_result;
typedef struct axk_preview_result axk_preview_result;
typedef struct axk_buffer axk_buffer;
typedef struct axk_build_plan axk_build_plan;
typedef struct axk_transaction axk_transaction;

typedef enum axk_status {
  AXK_STATUS_OK = 0,
  AXK_STATUS_INVALID_ARGUMENT = 1,
  AXK_STATUS_NOT_FOUND = 2,
  AXK_STATUS_CANCELLED = 3,
  AXK_STATUS_FORMAT_ERROR = 4,
  AXK_STATUS_INTERNAL_ERROR = 5,
  AXK_STATUS_UNSUPPORTED_ABI = 6,
  AXK_STATUS_STRUCT_TOO_SMALL = 7,
  AXK_STATUS_WRONG_THREAD = 8,
  AXK_STATUS_INVALID_HANDLE = 9,
  AXK_STATUS_OUTPUT_CONFLICT = 10
} axk_status;

typedef struct axk_string_view {
  const char *data;
  size_t size;
} axk_string_view;

typedef struct axk_struct_header {
  uint32_t struct_size;
  uint32_t abi_version;
} axk_struct_header;

#define AXK_INIT_STRUCT(value)                                                                    \
  do {                                                                                            \
    (value).struct_size = (uint32_t)sizeof(value);                                                 \
    (value).abi_version = AXK_ABI_VERSION;                                                         \
  } while (0)

typedef struct axk_content_node {
  uint32_t struct_size;
  uint32_t abi_version;
  axk_string_view id;
  axk_string_view type;
  axk_string_view display_name;
  axk_string_view object_key;
  axk_string_view object_type;
  axk_string_view quality;
  uint64_t child_count;
} axk_content_node;

typedef struct axk_object_info {
  uint32_t struct_size;
  uint32_t abi_version;
  axk_string_view object_key;
  axk_string_view object_type;
  axk_string_view object_name;
  axk_string_view partition_name;
  axk_string_view volume_name;
  axk_string_view category_name;
  uint32_t partition_index;
  uint32_t sfs_id;
  uint64_t payload_size;
} axk_object_info;

typedef struct axk_validation_summary {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t issue_count;
  uint64_t error_count;
  uint64_t warning_count;
  uint64_t object_count;
  uint64_t relationship_count;
  int32_t valid;
} axk_validation_summary;

typedef struct axk_preview_bin {
  uint32_t struct_size;
  uint32_t abi_version;
  int32_t minimum;
  int32_t maximum;
} axk_preview_bin;

typedef struct axk_error_info {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t status;
  uint32_t error_code;
  axk_string_view message;
  axk_string_view source_path;
  axk_string_view volume_name;
  axk_string_view object_type;
  axk_string_view object_name;
  uint64_t raw_offset;
  int32_t has_raw_offset;
  int32_t partition_index;
} axk_error_info;

typedef struct axk_write_options {
  uint32_t struct_size;
  uint32_t abi_version;
  int32_t overwrite;
  int32_t include_sfz;
} axk_write_options;

typedef struct axk_plan_summary {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t partition_count;
  uint64_t operation_count;
  uint64_t size_bytes;
  int32_t applies_changes;
} axk_plan_summary;

typedef struct axk_progress_event {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t phase;
  uint64_t completed;
  uint64_t total;
  int32_t has_total;
  axk_string_view label;
} axk_progress_event;

typedef void (*axk_progress_callback)(void *user_data, uint32_t phase, uint64_t completed,
                                      uint64_t total, int has_total, axk_string_view label);
typedef void (*axk_progress_callback_v1)(void *user_data, const axk_progress_event *event);

AXK_C_API uint32_t axk_abi_version(void);
AXK_C_API uint32_t axk_abi_version_major(void);
AXK_C_API uint32_t axk_abi_version_minor(void);
AXK_C_API axk_status axk_context_create(axk_context **out_context);
AXK_C_API axk_status axk_context_destroy(axk_context **context);
AXK_C_API axk_status axk_context_cancel(axk_context *context);
AXK_C_API axk_status axk_context_reset_cancel(axk_context *context);
AXK_C_API axk_status axk_context_set_progress_callback(axk_context *context,
                                                       axk_progress_callback callback,
                                                       void *user_data);
AXK_C_API axk_status axk_context_set_progress_callback_v1(axk_context *context,
                                                          axk_progress_callback_v1 callback,
                                                          void *user_data);
AXK_C_API axk_string_view axk_context_last_error(const axk_context *context);
AXK_C_API axk_status axk_context_last_error_info(const axk_context *context,
                                                 axk_error_info *out_error);

AXK_C_API axk_status axk_image_open(axk_context *context, axk_string_view utf8_path,
                                    axk_image **out_image);
AXK_C_API axk_status axk_image_close(axk_image **image);
AXK_C_API axk_status axk_image_snapshot(const axk_image *image, axk_snapshot **out_snapshot);
AXK_C_API axk_status axk_snapshot_destroy(axk_snapshot **snapshot);
AXK_C_API axk_status axk_image_content_children(axk_context *context, const axk_image *image,
                                                axk_string_view parent_node_id, uint64_t offset,
                                                uint64_t limit, axk_node_result **out_result);
AXK_C_API axk_status axk_snapshot_content_children(axk_context *context,
                                                   const axk_snapshot *snapshot,
                                                   axk_string_view parent_node_id, uint64_t offset,
                                                   uint64_t limit,
                                                   axk_node_result **out_result);
AXK_C_API axk_status axk_image_objects(axk_context *context, const axk_image *image,
                                       uint64_t offset, uint64_t limit,
                                       axk_object_result **out_result);
AXK_C_API axk_status axk_snapshot_objects(axk_context *context, const axk_snapshot *snapshot,
                                          uint64_t offset, uint64_t limit,
                                          axk_object_result **out_result);
AXK_C_API axk_status axk_image_validation_summary(axk_context *context, const axk_image *image,
                                                  axk_validation_summary *out_summary);
AXK_C_API axk_status axk_image_waveform_preview(axk_context *context, const axk_image *image,
                                                axk_string_view object_key, uint64_t bin_count,
                                                axk_preview_result **out_result);
AXK_C_API axk_status axk_image_waveform_pcm(axk_context *context, const axk_image *image,
                                            axk_string_view object_key, axk_buffer **out_buffer);
AXK_C_API axk_status axk_image_export_audio(axk_context *context, const axk_image *image,
                                            axk_string_view utf8_output_directory, int overwrite,
                                            int include_sfz, uint64_t *out_written_file_count);

AXK_C_API axk_status axk_hds_build_plan_create(axk_context *context,
                                               axk_string_view utf8_manifest_path,
                                               axk_build_plan **out_plan);
AXK_C_API axk_status axk_build_plan_summary(const axk_build_plan *plan,
                                            axk_plan_summary *out_summary);
AXK_C_API axk_status axk_build_plan_apply(axk_context *context, axk_build_plan *plan,
                                          axk_string_view utf8_output_path,
                                          const axk_write_options *options);
AXK_C_API axk_status axk_build_plan_destroy(axk_build_plan **plan);
AXK_C_API axk_status axk_hds_create(axk_context *context, axk_string_view utf8_manifest_path,
                                    axk_string_view utf8_output_path, int overwrite,
                                    uint64_t *out_partition_count);

AXK_C_API axk_status axk_hds_transaction_create(axk_context *context,
                                                axk_string_view utf8_source_path,
                                                axk_string_view utf8_manifest_path,
                                                axk_transaction **out_transaction);
AXK_C_API axk_status axk_transaction_summary(const axk_transaction *transaction,
                                             axk_plan_summary *out_summary);
AXK_C_API axk_status axk_transaction_apply(axk_context *context, axk_transaction *transaction,
                                           axk_string_view utf8_output_path,
                                           const axk_write_options *options);
AXK_C_API axk_status axk_transaction_destroy(axk_transaction **transaction);
AXK_C_API axk_status axk_hds_alter(axk_context *context, axk_string_view utf8_source_path,
                                   axk_string_view utf8_manifest_path,
                                   axk_string_view utf8_output_path, uint64_t *out_operation_count,
                                   int *out_applied);

AXK_C_API uint64_t axk_node_result_total_count(const axk_node_result *result);
AXK_C_API uint64_t axk_node_result_count(const axk_node_result *result);
AXK_C_API axk_status axk_node_result_at(const axk_node_result *result, uint64_t index,
                                        axk_content_node *out_node);
AXK_C_API axk_status axk_node_result_destroy(axk_node_result **result);
AXK_C_API uint64_t axk_object_result_total_count(const axk_object_result *result);
AXK_C_API uint64_t axk_object_result_count(const axk_object_result *result);
AXK_C_API axk_status axk_object_result_at(const axk_object_result *result, uint64_t index,
                                          axk_object_info *out_object);
AXK_C_API axk_status axk_object_result_destroy(axk_object_result **result);
AXK_C_API uint64_t axk_preview_result_frame_count(const axk_preview_result *result);
AXK_C_API uint64_t axk_preview_result_count(const axk_preview_result *result);
AXK_C_API axk_status axk_preview_result_at(const axk_preview_result *result, uint64_t index,
                                           axk_preview_bin *out_bin);
AXK_C_API axk_status axk_preview_result_destroy(axk_preview_result **result);
AXK_C_API const uint8_t *axk_buffer_data(const axk_buffer *buffer);
AXK_C_API uint64_t axk_buffer_size(const axk_buffer *buffer);
AXK_C_API axk_status axk_buffer_destroy(axk_buffer **buffer);

#ifdef __cplusplus
}
#endif
