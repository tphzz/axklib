#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <stdexcept>

#include <gtest/gtest.h>

#include "axklib/c_api.h"
#include "axklib/catalog.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"
#include "axklib/sfs.hpp"
#include "axklib/utf8.hpp"

namespace {

axk_string_view view(const std::string &value) { return {value.data(), value.size()}; }

std::string string(axk_string_view value) { return {value.data, value.size}; }

struct ProgressCapture {
  std::uint64_t calls{};
  std::uint64_t last_completed{};
  std::string last_label;
};

struct ReentryCapture {
  axk_context *context{};
  axk_transaction *transaction{};
  axk_string_view output{};
  const axk_write_options *options{};
  axk_status status{AXK_STATUS_OK};
  std::uint64_t calls{};
};

void capture_progress(void *user_data, std::uint32_t, std::uint64_t completed, std::uint64_t, int,
                      axk_string_view label) {
  auto &capture = *static_cast<ProgressCapture *>(user_data);
  ++capture.calls;
  capture.last_completed = completed;
  capture.last_label = string(label);
}

void capture_progress_v1(void *user_data, const axk_progress_event *event) {
  auto &capture = *static_cast<ProgressCapture *>(user_data);
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(event->abi_version, AXK_ABI_VERSION);
  EXPECT_GE(event->struct_size, sizeof(axk_progress_event));
  ++capture.calls;
  capture.last_completed = event->completed;
  capture.last_label = string(event->label);
}

void attempt_reentry(void *user_data, const axk_progress_event *) {
  auto &capture = *static_cast<ReentryCapture *>(user_data);
  ++capture.calls;
  if (capture.transaction == nullptr)
    return;
  capture.status =
      axk_transaction_apply(capture.context, capture.transaction, capture.output, capture.options);
  throw std::runtime_error{"callback failure must be contained"};
}

static_assert(offsetof(axk_content_node, struct_size) == 0U);
static_assert(offsetof(axk_content_node, abi_version) == sizeof(std::uint32_t));
static_assert(offsetof(axk_error_info, struct_size) == 0U);
static_assert(offsetof(axk_error_info, abi_version) == sizeof(std::uint32_t));

} // namespace

TEST(CApi, RejectsNullAndReportsOwnedContextError) {
  EXPECT_EQ(axk_context_create(nullptr), AXK_STATUS_INVALID_ARGUMENT);
  axk_context *context{};
  ASSERT_EQ(axk_context_create(&context), AXK_STATUS_OK);
  axk_image *image{};
  const std::string missing{"missing-image.hds"};
  EXPECT_EQ(axk_image_open(context, view(missing), &image), AXK_STATUS_NOT_FOUND);
  EXPECT_EQ(image, nullptr);
  const auto message = string(axk_context_last_error(context));
  EXPECT_FALSE(message.empty());
  EXPECT_NE(message.find("missing-image.hds"), std::string::npos);
  axk_error_info info{};
  AXK_INIT_STRUCT(info);
  EXPECT_EQ(axk_context_last_error_info(context, &info), AXK_STATUS_OK);
  EXPECT_EQ(info.status, AXK_STATUS_NOT_FOUND);
  EXPECT_NE(string(info.message).find("missing-image.hds"), std::string::npos);
  EXPECT_EQ(axk_context_destroy(&context), AXK_STATUS_OK);
  EXPECT_EQ(axk_context_destroy(nullptr), AXK_STATUS_OK);
}

TEST(CApi, VersionedStructuresRejectTruncationAndUnsupportedMajor) {
  EXPECT_EQ(axk_abi_version(), AXK_ABI_VERSION);
  EXPECT_EQ(axk_abi_version_major(), AXK_ABI_VERSION_MAJOR);
  EXPECT_EQ(axk_abi_version_minor(), AXK_ABI_VERSION_MINOR);
  axk_context *context{};
  ASSERT_EQ(axk_context_create(&context), AXK_STATUS_OK);
  axk_error_info info{};
  info.struct_size = sizeof(info) - 1U;
  info.abi_version = AXK_ABI_VERSION;
  EXPECT_EQ(axk_context_last_error_info(context, &info), AXK_STATUS_STRUCT_TOO_SMALL);
  info.struct_size = sizeof(info);
  info.abi_version = 2U << 16U;
  EXPECT_EQ(axk_context_last_error_info(context, &info), AXK_STATUS_UNSUPPORTED_ABI);
  axk_image *image{};
  EXPECT_EQ(axk_image_open(context, {}, &image), AXK_STATUS_INVALID_ARGUMENT);
  const std::string malformed_utf8{"\xc3\x28", 2U};
  EXPECT_EQ(axk_image_open(context, view(malformed_utf8), &image), AXK_STATUS_INVALID_ARGUMENT);
  const std::string embedded_nul{"image\0.hds", 10U};
  EXPECT_EQ(axk_image_open(context, view(embedded_nul), &image), AXK_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(axk_context_set_progress_callback_v1(context, capture_progress_v1, nullptr),
            AXK_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(axk_context_destroy(&context), AXK_STATUS_OK);
}

TEST(CApi, DebugRegistryRejectsStaleCopiedHandlesWithoutDereferencingThem) {
  axk_context *context{};
  ASSERT_EQ(axk_context_create(&context), AXK_STATUS_OK);
  axk_image *image{};
  const auto path = (std::filesystem::path{AXK_SOURCE_ROOT} /
                     "tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds")
                        .string();
  ASSERT_EQ(axk_image_open(context, view(path), &image), AXK_STATUS_OK);
  auto *stale = image;
  ASSERT_EQ(axk_image_close(&image), AXK_STATUS_OK);
  axk_node_result *result{};
  EXPECT_EQ(axk_image_content_children(context, stale, {}, 0U, 1U, &result),
            AXK_STATUS_INVALID_HANDLE);
  EXPECT_EQ(axk_image_close(&stale), AXK_STATUS_INVALID_HANDLE);
  EXPECT_EQ(stale, nullptr);
  auto *stale_context = context;
  ASSERT_EQ(axk_context_destroy(&context), AXK_STATUS_OK);
  EXPECT_EQ(axk_context_cancel(stale_context), AXK_STATUS_INVALID_HANDLE);
  EXPECT_EQ(axk_context_destroy(&stale_context), AXK_STATUS_INVALID_HANDLE);
}

TEST(CApi, CancellationAndPaginationHaveStableHandleLifetimes) {
  axk_context *context{};
  ASSERT_EQ(axk_context_create(&context), AXK_STATUS_OK);
  ASSERT_EQ(axk_context_cancel(context), AXK_STATUS_OK);
  axk_image *image{};
  const auto path = (std::filesystem::path{AXK_SOURCE_ROOT} /
                     "tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds")
                        .string();
  EXPECT_EQ(axk_image_open(context, view(path), &image), AXK_STATUS_CANCELLED);
  ASSERT_EQ(axk_context_reset_cancel(context), AXK_STATUS_OK);
  ASSERT_EQ(axk_image_open(context, view(path), &image), AXK_STATUS_OK);

  axk_node_result *roots{};
  ASSERT_EQ(axk_image_content_children(context, image, {}, 0, 1, &roots), AXK_STATUS_OK);
  EXPECT_EQ(axk_node_result_total_count(roots), 1U);
  axk_content_node root{};
  AXK_INIT_STRUCT(root);
  ASSERT_EQ(axk_node_result_at(roots, 0, &root), AXK_STATUS_OK);
  const auto root_id = string(root.id);
  EXPECT_EQ(string(root.type), "partition");
  EXPECT_EQ(axk_node_result_at(roots, 1, &root), AXK_STATUS_INVALID_ARGUMENT);
  ASSERT_EQ(axk_node_result_destroy(&roots), AXK_STATUS_OK);

  axk_node_result *volumes{};
  ASSERT_EQ(axk_image_content_children(context, image, view(root_id), 0, 1, &volumes),
            AXK_STATUS_OK);
  EXPECT_EQ(axk_node_result_count(volumes), 1U);
  ASSERT_EQ(axk_node_result_destroy(&volumes), AXK_STATUS_OK);
  ASSERT_EQ(axk_image_close(&image), AXK_STATUS_OK);

  ASSERT_EQ(axk_context_destroy(&context), AXK_STATUS_OK);
}

TEST(CApi, PreviewPcmBufferAndFileExportUseOwnedResults) {
  axk_context *context{};
  ASSERT_EQ(axk_context_create(&context), AXK_STATUS_OK);
  axk_image *image{};
  const auto path = (std::filesystem::path{AXK_SOURCE_ROOT} /
                     "tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds")
                        .string();
  ASSERT_EQ(axk_image_open(context, view(path), &image), AXK_STATUS_OK);
  const std::string key{"p0:sfs9"};
  axk_preview_result *preview{};
  ASSERT_EQ(axk_image_waveform_preview(context, image, view(key), 16, &preview), AXK_STATUS_OK);
  EXPECT_EQ(axk_preview_result_frame_count(preview), 132U);
  EXPECT_EQ(axk_preview_result_count(preview), 16U);
  axk_preview_bin bin{};
  AXK_INIT_STRUCT(bin);
  EXPECT_EQ(axk_preview_result_at(preview, 0, &bin), AXK_STATUS_OK);
  EXPECT_LE(bin.minimum, bin.maximum);
  ASSERT_EQ(axk_preview_result_destroy(&preview), AXK_STATUS_OK);

  axk_buffer *pcm{};
  ASSERT_EQ(axk_image_waveform_pcm(context, image, view(key), &pcm), AXK_STATUS_OK);
  EXPECT_NE(axk_buffer_data(pcm), nullptr);
  EXPECT_EQ(axk_buffer_size(pcm), 264U);
  ASSERT_EQ(axk_buffer_destroy(&pcm), AXK_STATUS_OK);

  const auto output = std::filesystem::temp_directory_path() / "axklib-c-api-export";
  std::error_code error;
  std::filesystem::remove_all(output, error);
  const auto output_text = output.string();
  std::uint64_t written{};
  ASSERT_EQ(axk_image_export_audio(context, image, view(output_text), 0, 1, &written),
            AXK_STATUS_OK);
  EXPECT_EQ(written, 16U);
  EXPECT_TRUE(std::filesystem::is_regular_file(
      output / "partition_00_New_Partition/New Volume/SMPL/sine wave.wav"));
  std::filesystem::remove_all(output, error);
  ASSERT_EQ(axk_image_close(&image), AXK_STATUS_OK);
  ASSERT_EQ(axk_context_destroy(&context), AXK_STATUS_OK);
}

TEST(CApi, SnapshotKeepsImmutableStateAndSupportsConcurrentPagination) {
  axk_context *context{};
  ASSERT_EQ(axk_context_create(&context), AXK_STATUS_OK);
  axk_image *image{};
  const auto path = (std::filesystem::path{AXK_SOURCE_ROOT} /
                     "tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds")
                        .string();
  ASSERT_EQ(axk_image_open(context, view(path), &image), AXK_STATUS_OK);
  axk_snapshot *snapshot{};
  ASSERT_EQ(axk_image_snapshot(image, &snapshot), AXK_STATUS_OK);
  ASSERT_EQ(axk_image_close(&image), AXK_STATUS_OK);

  const auto read_page = [&] {
    axk_context *thread_context{};
    EXPECT_EQ(axk_context_create(&thread_context), AXK_STATUS_OK);
    axk_object_result *objects{};
    EXPECT_EQ(axk_snapshot_objects(thread_context, snapshot, 0U, 64U, &objects), AXK_STATUS_OK);
    EXPECT_EQ(axk_object_result_total_count(objects), 17U);
    axk_object_info item{};
    AXK_INIT_STRUCT(item);
    EXPECT_EQ(axk_object_result_at(objects, 0U, &item), AXK_STATUS_OK);
    EXPECT_FALSE(string(item.object_key).empty());
    EXPECT_EQ(axk_object_result_destroy(&objects), AXK_STATUS_OK);
    EXPECT_EQ(axk_context_destroy(&thread_context), AXK_STATUS_OK);
  };
  std::thread first{read_page};
  std::thread second{read_page};
  first.join();
  second.join();
  axk_node_result *roots{};
  ASSERT_EQ(axk_snapshot_content_children(context, snapshot, {}, 0U, 4U, &roots), AXK_STATUS_OK);
  EXPECT_EQ(axk_node_result_total_count(roots), 1U);
  ASSERT_EQ(axk_node_result_destroy(&roots), AXK_STATUS_OK);
  EXPECT_EQ(axk_snapshot_destroy(&snapshot), AXK_STATUS_OK);
  EXPECT_EQ(axk_context_destroy(&context), AXK_STATUS_OK);
}

TEST(CApi, InventoryAndValidationMatchDirectCppCalls) {
  const auto path = std::filesystem::path{AXK_SOURCE_ROOT} /
                    "tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds";
  auto native = axk::open_image(path);
  ASSERT_TRUE(native.has_value());
  auto catalog = axk::build_object_catalog(*native);
  ASSERT_TRUE(catalog.has_value());
  const auto graph = axk::build_relationship_graph(*catalog);
  const auto validation = axk::validate_semantics(*native, *catalog, graph);

  axk_context *context{};
  axk_image *image{};
  ASSERT_EQ(axk_context_create(&context), AXK_STATUS_OK);
  const auto path_text = path.string();
  ASSERT_EQ(axk_image_open(context, view(path_text), &image), AXK_STATUS_OK);
  axk_object_result *objects{};
  ASSERT_EQ(axk_image_objects(context, image, 0U, 1024U, &objects), AXK_STATUS_OK);
  EXPECT_EQ(axk_object_result_total_count(objects), catalog->objects.size());
  axk_validation_summary summary{};
  AXK_INIT_STRUCT(summary);
  ASSERT_EQ(axk_image_validation_summary(context, image, &summary), AXK_STATUS_OK);
  EXPECT_EQ(summary.issue_count, validation.issues.size());
  EXPECT_EQ(summary.object_count, validation.coverage.object_count);
  EXPECT_EQ(summary.relationship_count, validation.coverage.relationship_count);
  EXPECT_EQ(summary.valid, validation.valid() ? 1 : 0);
  EXPECT_EQ(axk_object_result_destroy(&objects), AXK_STATUS_OK);
  EXPECT_EQ(axk_image_close(&image), AXK_STATUS_OK);
  EXPECT_EQ(axk_context_destroy(&context), AXK_STATUS_OK);
}

TEST(CApi, CreatesFreshImageWithoutExposingWriterInternals) {
  const auto root = std::filesystem::temp_directory_path() /
                    std::filesystem::path{u8"axklib-c-api-Größe-音"};
  const auto manifest = root / std::filesystem::path{u8"Aufbau-音.json"};
  const auto output = root / std::filesystem::path{u8"frisches-Abbild-音.hds"};
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  std::ofstream{manifest}
      << R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"hd1","volumes":[{"name":"Volume","waveforms":[],"sample_banks":[]}]}]})";
  axk_context *context{};
  ASSERT_EQ(axk_context_create(&context), AXK_STATUS_OK);
  const auto manifest_text = axk::text::path_to_utf8(manifest);
  const auto output_text = axk::text::path_to_utf8(output);
  std::uint64_t partitions{};
  ASSERT_EQ(axk_hds_create(context, view(manifest_text), view(output_text), 0, &partitions),
            AXK_STATUS_OK);
  EXPECT_EQ(partitions, 1U);
  EXPECT_EQ(std::filesystem::file_size(output), 1'048'576U);
  axk_image* image{};
  EXPECT_EQ(axk_image_open(context, view(output_text), &image), AXK_STATUS_OK);
  EXPECT_EQ(axk_image_close(&image), AXK_STATUS_OK);
  EXPECT_NE(axk_hds_create(context, view(manifest_text), view(output_text), 0, &partitions),
            AXK_STATUS_OK);
  EXPECT_EQ(axk_context_destroy(&context), AXK_STATUS_OK);
  std::filesystem::remove_all(root, error);
}

TEST(CApi, BuildPlanIsVersionedAndSingleThreadOwned) {
  const auto root = std::filesystem::temp_directory_path() / "axklib-c-api-build-plan";
  const auto manifest = root / "manifest.json";
  const auto output = root / "fresh.hds";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  std::ofstream{manifest}
      << R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"hd1","volumes":[{"name":"Volume","waveforms":[],"sample_banks":[]}]}]})";
  axk_context *context{};
  ASSERT_EQ(axk_context_create(&context), AXK_STATUS_OK);
  axk_build_plan *plan{};
  const auto manifest_text = manifest.string();
  ASSERT_EQ(axk_hds_build_plan_create(context, view(manifest_text), &plan), AXK_STATUS_OK);
  axk_plan_summary summary{};
  AXK_INIT_STRUCT(summary);
  ASSERT_EQ(axk_build_plan_summary(plan, &summary), AXK_STATUS_OK);
  EXPECT_EQ(summary.partition_count, 1U);
  EXPECT_EQ(summary.size_bytes, 1'048'576U);
  axk_write_options options{};
  AXK_INIT_STRUCT(options);
  const auto output_text = output.string();
  axk_status thread_status{AXK_STATUS_OK};
  std::thread wrong_thread{[&] {
    thread_status = axk_build_plan_apply(context, plan, view(output_text), &options);
  }};
  wrong_thread.join();
  EXPECT_EQ(thread_status, AXK_STATUS_WRONG_THREAD);
  ASSERT_EQ(axk_build_plan_apply(context, plan, view(output_text), &options), AXK_STATUS_OK);
  EXPECT_TRUE(std::filesystem::exists(output));
  EXPECT_EQ(axk_build_plan_destroy(&plan), AXK_STATUS_OK);
  EXPECT_EQ(axk_context_destroy(&context), AXK_STATUS_OK);
  std::filesystem::remove_all(root, error);
}

TEST(CApi, TransactionHandlePlansAppliesAndReportsV1Progress) {
  const auto root = std::filesystem::temp_directory_path() / "axklib-c-api-transaction-v1";
  const auto build = root / "build.json";
  const auto manifest = root / "transaction.json";
  const auto source = root / "source.hds";
  const auto output = root / "output.hds";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  std::ofstream{build}
      << R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"hd1","volumes":[{"name":"Keep","waveforms":[],"sample_banks":[]},{"name":"Delete","waveforms":[],"sample_banks":[]}]}]})";
  std::ofstream{manifest}
      << R"({"schema_version":"1.0","operations":[{"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"Delete"}]})";
  axk_context *context{};
  ASSERT_EQ(axk_context_create(&context), AXK_STATUS_OK);
  ReentryCapture progress;
  progress.context = context;
  ASSERT_EQ(axk_context_set_progress_callback_v1(context, attempt_reentry, &progress),
            AXK_STATUS_OK);
  const auto build_text = build.string();
  const auto source_text = source.string();
  std::uint64_t partitions{};
  ASSERT_EQ(axk_hds_create(context, view(build_text), view(source_text), 0, &partitions),
            AXK_STATUS_OK);
  EXPECT_EQ(axk_context_set_progress_callback_v1(context, attempt_reentry, &progress),
            AXK_STATUS_INVALID_ARGUMENT);
  axk_transaction *transaction{};
  const auto manifest_text = manifest.string();
  ASSERT_EQ(axk_hds_transaction_create(context, view(source_text), view(manifest_text), &transaction),
            AXK_STATUS_OK);
  axk_plan_summary summary{};
  AXK_INIT_STRUCT(summary);
  ASSERT_EQ(axk_transaction_summary(transaction, &summary), AXK_STATUS_OK);
  EXPECT_EQ(summary.operation_count, 1U);
  axk_write_options options{};
  AXK_INIT_STRUCT(options);
  const auto output_text = output.string();
  progress.transaction = transaction;
  progress.output = view(output_text);
  progress.options = &options;
  ASSERT_EQ(axk_transaction_apply(context, transaction, view(output_text), &options),
            AXK_STATUS_OK);
  EXPECT_TRUE(std::filesystem::exists(output));
  EXPECT_GT(progress.calls, 0U);
  EXPECT_EQ(progress.status, AXK_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(axk_transaction_destroy(&transaction), AXK_STATUS_OK);
  EXPECT_EQ(axk_context_destroy(&context), AXK_STATUS_OK);
  std::filesystem::remove_all(root, error);
}

TEST(CApi, PlansAndAppliesAlterationWithContextCancellationContract) {
  const auto root = std::filesystem::temp_directory_path() / "axklib-c-api-alteration";
  const auto build_manifest = root / "build.json";
  const auto transaction = root / "transaction.json";
  const auto source = root / "source.hds";
  const auto output = root / "output.hds";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  std::ofstream{build_manifest}
      << R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"hd1","volumes":[{"name":"Keep","waveforms":[],"sample_banks":[]},{"name":"Delete","waveforms":[],"sample_banks":[]}]}]})";
  std::ofstream{transaction}
      << R"({"schema_version":"1.0","operations":[{"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"Delete"}]})";
  axk_context *context{};
  ASSERT_EQ(axk_context_create(&context), AXK_STATUS_OK);
  ProgressCapture progress;
  ASSERT_EQ(axk_context_set_progress_callback(context, capture_progress, &progress), AXK_STATUS_OK);
  const auto build_text = build_manifest.string();
  const auto transaction_text = transaction.string();
  const auto source_text = source.string();
  const auto output_text = output.string();
  std::uint64_t partitions{};
  ASSERT_EQ(axk_hds_create(context, view(build_text), view(source_text), 0, &partitions),
            AXK_STATUS_OK);
  std::uint64_t operations{};
  int applied{-1};
  ASSERT_EQ(
      axk_hds_alter(context, view(source_text), view(transaction_text), {}, &operations, &applied),
      AXK_STATUS_OK);
  EXPECT_EQ(operations, 1U);
  EXPECT_EQ(applied, 0);
  EXPECT_GT(progress.calls, 0U);
  EXPECT_EQ(progress.last_completed, 1U);
  EXPECT_EQ(progress.last_label, "delete_volume");
  EXPECT_FALSE(std::filesystem::exists(output));
  ASSERT_EQ(axk_context_cancel(context), AXK_STATUS_OK);
  EXPECT_EQ(axk_hds_alter(context, view(source_text), view(transaction_text), view(output_text),
                          &operations, &applied),
            AXK_STATUS_CANCELLED);
  EXPECT_FALSE(std::filesystem::exists(output));
  ASSERT_EQ(axk_context_reset_cancel(context), AXK_STATUS_OK);
  ASSERT_EQ(axk_hds_alter(context, view(source_text), view(transaction_text), view(output_text),
                          &operations, &applied),
            AXK_STATUS_OK);
  EXPECT_EQ(applied, 1);
  EXPECT_TRUE(std::filesystem::exists(output));
  EXPECT_EQ(axk_context_destroy(&context), AXK_STATUS_OK);
  std::filesystem::remove_all(root, error);
}
