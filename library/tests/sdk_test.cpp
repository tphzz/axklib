#include "axklib/sdk.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#include <gtest/gtest.h>

namespace {

std::filesystem::path fixture_path() {
  return std::filesystem::path{AXK_SOURCE_ROOT} / "tests" / "fixtures" / "images" /
         "sampler-authored" / "HD00_512_single_sbnk_authored.hds";
}

void write_test_wave(const std::filesystem::path &path) {
  constexpr std::array<unsigned char, 48> wave{
      'R', 'I', 'F', 'F', 40,  0,   0,   0,   'W',  'A',  'V', 'E', 'f',  'm',  't', ' ',
      16,  0,   0,   0,   1,   0,   1,   0,   0x44, 0xac, 0,   0,   0x88, 0x58, 1,   0,
      2,   0,   16,  0,   'd', 'a', 't', 'a', 4,    0,    0,   0,   0,    0,    1,   0,
  };
  std::ofstream output{path, std::ios::binary};
  output.write(reinterpret_cast<const char *>(wave.data()),
               static_cast<std::streamsize>(wave.size()));
}

TEST(Sdk, PublicFacadeOwnsVersionResultAndMoveOnlySessions) {
  static_assert(!std::is_copy_constructible_v<axk::image>);
  static_assert(std::is_move_constructible_v<axk::image>);
  static_assert(!std::is_copy_constructible_v<axk::operation_context>);
  static_assert(sizeof(axk::image) <= sizeof(void *) * 2U);
  static_assert(sizeof(axk::snapshot) <= sizeof(void *) * 2U);
  static_assert(sizeof(axk::build_plan) <= sizeof(void *) * 2U);
  static_assert(sizeof(axk::transaction) <= sizeof(void *) * 2U);
  EXPECT_EQ(axk::sdk_version(), "0.1.0");

  axk::result<int> success{42};
  ASSERT_TRUE(success);
  EXPECT_EQ(*success, 42);

  axk::result<int> failure{axk::error{
      axk::error_code::invalid_argument, axk::error_category::internal, "bad input", {}}};
  ASSERT_FALSE(failure);
  EXPECT_EQ(failure.error().message, "bad input");
  EXPECT_NE(axk::render_error(failure.error()).find("bad input"), std::string::npos);
  EXPECT_THROW(failure.value(), axk::bad_result_access);
  EXPECT_THROW(success.error(), axk::bad_result_access);

  axk::result<void> void_success;
  EXPECT_TRUE(void_success);
  axk::result<void> void_failure{failure.error()};
  EXPECT_FALSE(void_failure);
  EXPECT_THROW(void_failure.value(), axk::bad_result_access);
}

TEST(Sdk, OpensPagesValidatesPreviewsAndOwnsPcm) {
  axk::operation_context context;
  auto opened = axk::image::open(fixture_path().string(), context);
  ASSERT_TRUE(opened);

  auto nodes = opened->content_children("", 0U, 32U, context);
  ASSERT_TRUE(nodes);
  EXPECT_FALSE(nodes->items.empty());
  EXPECT_EQ(nodes->total_count, nodes->items.size());

  auto objects = opened->objects(0U, 32U, context);
  ASSERT_TRUE(objects);
  ASSERT_FALSE(objects->items.empty());
  const auto waveform =
      std::find_if(objects->items.begin(), objects->items.end(),
                   [](const axk::object_info &item) { return item.type == "SMPL"; });
  ASSERT_NE(waveform, objects->items.end());

  auto validation = opened->validation(context);
  ASSERT_TRUE(validation);
  EXPECT_TRUE(validation->valid);
  EXPECT_GT(validation->object_count, 0U);

  auto relationships = opened->relationships(0U, 32U, context);
  ASSERT_TRUE(relationships);
  EXPECT_EQ(relationships->total_count, validation->relationship_count);

  auto issues = opened->validation_issues(0U, 32U, context);
  ASSERT_TRUE(issues);
  EXPECT_EQ(issues->total_count, validation->issue_count);

  auto preview = opened->preview(waveform->key, 8U, context);
  ASSERT_TRUE(preview);
  EXPECT_EQ(preview->bins.size(), 8U);

  auto pcm = opened->waveform_pcm(waveform->key, context);
  ASSERT_TRUE(pcm);
  EXPECT_FALSE(pcm->empty());

  auto snapshot = opened->make_snapshot();
  ASSERT_TRUE(snapshot);
  opened =
      axk::error{axk::error_code::invalid_argument, axk::error_category::internal, "released", {}};
  auto snapshot_objects = snapshot->objects(0U, 32U);
  ASSERT_TRUE(snapshot_objects);
  EXPECT_EQ(snapshot_objects->total_count, objects->total_count);
  auto snapshot_relationships = snapshot->relationships(0U, 32U);
  ASSERT_TRUE(snapshot_relationships);
  EXPECT_EQ(snapshot_relationships->total_count, relationships->total_count);
  auto snapshot_validation = snapshot->validation();
  ASSERT_TRUE(snapshot_validation);
  EXPECT_EQ(snapshot_validation->issue_count, validation->issue_count);
}

TEST(Sdk, CancellationIsAnOwnedFailure) {
  axk::operation_context context;
  context.cancel();
  auto opened = axk::image::open(fixture_path().string(), context);
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().code, axk::error_code::operation_cancelled);
  context.reset_cancel();
  EXPECT_TRUE(axk::image::open(fixture_path().string(), context));
}

TEST(Sdk, ImmutableSnapshotSupportsConcurrentPagination) {
  axk::operation_context context;
  auto opened = axk::image::open(fixture_path().string(), context);
  ASSERT_TRUE(opened);
  auto snapshot = opened->make_snapshot();
  ASSERT_TRUE(snapshot);
  std::atomic<bool> valid{true};
  const auto read = [&]() {
    for (std::uint64_t offset = 0; offset < 17U; ++offset) {
      auto page = snapshot->objects(offset, 1U);
      if (!page || page->total_count != 17U)
        valid.store(false);
    }
  };
  std::thread first{read};
  std::thread second{read};
  first.join();
  second.join();
  EXPECT_TRUE(valid.load());
}

class TestProgressSink final : public axk::progress_sink {
public:
  void report(const axk::progress_event &event) noexcept override {
    ++calls;
    last_label = event.label;
  }

  std::uint64_t calls{};
  std::string last_label;
};

class ThrowingProgressSink final : public axk::progress_sink {
public:
  void report(const axk::progress_event &) override { throw std::runtime_error{"callback"}; }
};

TEST(Sdk, BuildAndTransactionPlansApplyThroughTheFacade) {
  const auto root = std::filesystem::temp_directory_path() / "axklib-sdk-plans";
  const auto build_manifest = root / "build.json";
  const auto alteration_manifest = root / "alteration.json";
  const auto source = root / "source.hds";
  const auto output = root / "output.hds";
  std::error_code filesystem_error;
  std::filesystem::remove_all(root, filesystem_error);
  std::filesystem::create_directories(root);
  std::ofstream{build_manifest}
      << R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"hd1","volumes":[{"name":"Keep","waveforms":[],"sample_banks":[]},{"name":"Delete","waveforms":[],"sample_banks":[]}]}]})";
  std::ofstream{alteration_manifest}
      << R"({"schema_version":"1.0","operations":[{"id":"delete","type":"delete_volume","partition_index":0,"volume_name":"Delete"}]})";

  axk::operation_context context;
  TestProgressSink progress;
  context.set_progress_sink(&progress);
  auto build = axk::build_plan::from_manifest(build_manifest.string(), context);
  ASSERT_TRUE(build);
  EXPECT_EQ(build->summary().partition_count, 1U);
  EXPECT_EQ(build->summary().size_bytes, 1'048'576U);
  ASSERT_TRUE(build->apply(source.string(), {}, context));
  EXPECT_TRUE(std::filesystem::exists(source));

  auto alteration =
      axk::transaction::from_manifest(source.string(), alteration_manifest.string(), context);
  ASSERT_TRUE(alteration);
  EXPECT_EQ(alteration->summary().operation_count, 1U);
  ASSERT_TRUE(alteration->apply(output.string(), {}, context));
  EXPECT_TRUE(std::filesystem::exists(output));
  EXPECT_GT(progress.calls, 0U);

  std::filesystem::remove_all(root, filesystem_error);
}

TEST(Sdk, MediaBuildPlanCreatesAFat12Image) {
  const auto root = std::filesystem::temp_directory_path() / "axklib-sdk-media-plan";
  const auto manifest = root / "build.json";
  const auto audio = root / "tone.wav";
  const auto output = root / "output.ima";
  std::error_code filesystem_error;
  std::filesystem::remove_all(root, filesystem_error);
  std::filesystem::create_directories(root);
  write_test_wave(audio);
  std::ofstream{manifest}
      << R"({"schema_version":"1.0","format":"fat12_floppy","authored_volume":{"name":"Volume","waveforms":[{"id":"tone","name":"Tone","path":"tone.wav","root_key":60}],"sample_banks":[{"name":"Tone Bank","waveform_id":"tone","root_key":60,"key_low":0,"key_high":127}]}})";

  axk::operation_context context;
  auto plan = axk::build_plan::from_manifest(manifest.string(), context);
  ASSERT_TRUE(plan) << plan.error().message;
  EXPECT_EQ(plan->summary().size_bytes, 1'474'560U);
  ASSERT_TRUE(plan->apply(output.string(), {}, context));
  EXPECT_EQ(std::filesystem::file_size(output), 1'474'560U);

  std::filesystem::remove_all(root, filesystem_error);
}

TEST(Sdk, ProgressCallbackFailureDoesNotCrossTheFacade) {
  const auto root = std::filesystem::temp_directory_path() / "axklib-sdk-callback";
  const auto manifest = root / "build.json";
  const auto output = root / "output.hds";
  std::error_code filesystem_error;
  std::filesystem::remove_all(root, filesystem_error);
  std::filesystem::create_directories(root);
  std::ofstream{manifest}
      << R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"hd1","volumes":[{"name":"Volume","waveforms":[],"sample_banks":[]}]}]})";
  axk::operation_context context;
  ThrowingProgressSink sink;
  context.set_progress_sink(&sink);
  auto plan = axk::build_plan::from_manifest(manifest.string(), context);
  ASSERT_TRUE(plan);
  EXPECT_TRUE(plan->apply(output.string(), {}, context));
  context.set_progress_sink(nullptr);
  std::filesystem::remove_all(root, filesystem_error);
}

} // namespace
