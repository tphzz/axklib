#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app.hpp"
#include "content_id.hpp"

#ifdef AXK_TEST_SHARED_SDK
#include "axklib/sdk.hpp"
#endif

namespace detail = axk::cli::detail;

namespace {

std::string normalize_newlines(std::string value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '\r' && index + 1U < value.size() && value[index + 1U] == '\n')
      continue;
    normalized.push_back(value[index]);
  }
  return normalized;
}

int run_cli(std::vector<std::string> arguments) {
  std::vector<char *> argv;
  argv.reserve(arguments.size());
  std::ranges::transform(arguments, std::back_inserter(argv),
                         [](auto &value) { return value.data(); });
  return axk::cli::run(static_cast<int>(argv.size()), argv.data());
}

std::string read_bytes(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{input}, {}};
}

} // namespace

TEST(ContentId, MatchesPublishedSha1VectorsAndStablePooledName) {
  const std::vector<std::byte> empty;
  EXPECT_EQ(detail::sha1_content_id(empty).digest_hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  EXPECT_EQ(detail::sha1_content_id(bytes).digest_hex, "a9993e364706816aba3e25717850c26c9cd0d89d");

  detail::PooledPathAllocator paths;
  const auto pooled = paths.allocate("file", "physical", "Sample", bytes);
  ASSERT_TRUE(pooled);
  EXPECT_EQ(pooled->filename(), "Sample__a9993e364706.wav");
}

TEST(ContentId, ReusesEqualContentAndRejectsInjectedShortPrefixCollision) {
  const auto fake = [](std::span<const std::byte> bytes) {
    const auto tail = bytes.front() == std::byte{1} ? std::string(28U, '0') : std::string(28U, '1');
    return detail::ContentId{"sha1", "aaaaaaaaaaaa" + tail};
  };
  detail::PooledPathAllocator paths{fake};
  const std::array first{std::byte{1}};
  const std::array second{std::byte{2}};
  const auto initial = paths.allocate("file", "physical", "Sample", first);
  ASSERT_TRUE(initial);
  const auto reused = paths.allocate("file", "physical", "Sample", first);
  ASSERT_TRUE(reused);
  EXPECT_EQ(*reused, *initial);
  const auto collision = paths.allocate("file", "physical", "Sample", second);
  ASSERT_FALSE(collision);
  EXPECT_NE(collision.error().message.find("distinct WAV contents"), std::string::npos);
}

TEST(Cli11Adapter, ReturnsParserExitCodesWithoutProcessExitMacros) {
  std::array help{const_cast<char *>("axklib"), const_cast<char *>("--help")};
  EXPECT_EQ(axk::cli::run(static_cast<int>(help.size()), help.data()), 0);

  std::array invalid{const_cast<char *>("axklib"), const_cast<char *>("--not-an-option")};
  EXPECT_NE(axk::cli::run(static_cast<int>(invalid.size()), invalid.data()), 0);
}

TEST(Cli11Adapter, RejectsInvalidUtf8AndRepeatedScalarOptions) {
  std::string invalid_value{"\xc3\x28", 2U};
  std::array invalid_utf8{const_cast<char *>("axklib"), invalid_value.data()};
  EXPECT_EQ(axk::cli::run(static_cast<int>(invalid_utf8.size()), invalid_utf8.data()), 2);

  std::array repeated{const_cast<char *>("axklib"),     const_cast<char *>("info"),
                      const_cast<char *>("--format"),   const_cast<char *>("json"),
                      const_cast<char *>("--format"),   const_cast<char *>("tree"),
                      const_cast<char *>("missing.hds")};
  EXPECT_NE(axk::cli::run(static_cast<int>(repeated.size()), repeated.data()), 0);
}

TEST(Cli11Adapter, ExposesMaintainedCommandInventoryAndHidesLegacyAliases) {
  std::array help{const_cast<char *>("axklib"), const_cast<char *>("--help")};
  testing::internal::CaptureStdout();
  EXPECT_EQ(axk::cli::run(static_cast<int>(help.size()), help.data()), 0);
  const auto output = testing::internal::GetCapturedStdout();
  for (const auto command : {"info", "inventory", "validate", "relationships", "coverage", "create",
                             "alter", "orphans", "objects", "corpus", "extract", "package"})
    EXPECT_NE(output.find(command), std::string::npos) << command;
  EXPECT_EQ(output.find("extract-wav"), std::string::npos);
  EXPECT_EQ(output.find("create-hds"), std::string::npos);

  const auto fixture =
      std::filesystem::path{AXK_SOURCE_ROOT} / "library/tests/fixtures/cli/help.txt";
  std::ifstream stream{fixture, std::ios::binary};
  ASSERT_TRUE(stream);
  const std::string expected{std::istreambuf_iterator<char>{stream}, {}};
  EXPECT_EQ(normalize_newlines(output), normalize_newlines(expected));
}

TEST(Cli11Adapter, ExposesCompletePortablePackageCommandFamily) {
  testing::internal::CaptureStdout();
  EXPECT_EQ(run_cli({"axklib", "package", "--help"}), 0);
  const auto output = testing::internal::GetCapturedStdout();
  for (const auto command : {"export", "inspect", "verify", "plan-import", "import"})
    EXPECT_NE(output.find(command), std::string::npos) << command;
}

TEST(Cli11Adapter, PortablePackageRoundTripPlansAndImportsAtomically) {
  const auto fixture = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images" /
                       "sampler-authored/HD00_512_single_sbnk_authored.hds";
  const auto root = std::filesystem::temp_directory_path() / "axklib-cli-package-round-trip";
  const auto package_stem = root / "sine-bank";
  const auto package = root / "sine-bank.axksbnk";
  const auto manifest = root / "target.json";
  const auto target = root / "target.hds";
  const auto imported = root / "imported.hds";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  ASSERT_TRUE(std::filesystem::create_directories(root));

  testing::internal::CaptureStdout();
  ASSERT_EQ(run_cli({"axklib", "package", "export", fixture.string(), "--root", "sbnk=sine wave",
                     "--partition", "0", "--group", "New Partition", "--volume", "New Volume", "-o",
                     package_stem.string(), "--format", "json"}),
            0);
  const auto export_output = testing::internal::GetCapturedStdout();
  EXPECT_NE(export_output.find("\"package_kind\":\"sbnk\""), std::string::npos);
  EXPECT_NE(export_output.find("\"required_extension\":\".axksbnk\""), std::string::npos);
  ASSERT_TRUE(std::filesystem::is_regular_file(package));

#ifdef AXK_TEST_SHARED_SDK
  const auto cli_export_json = nlohmann::json::parse(export_output);
  axk::operation_context sdk_context;
  axk::package_root_selector sdk_selector;
  sdk_selector.kind = axk::package_root_kind::sample_bank;
  sdk_selector.partition_index = 0U;
  sdk_selector.group_name = "New Partition";
  sdk_selector.volume_name = "New Volume";
  sdk_selector.object_name = "sine wave";
  const auto sdk_export = axk::portable_package::export_from(
      fixture.string(), {sdk_selector}, (root / "sdk-sine-bank").string(), {}, sdk_context);
  ASSERT_TRUE(sdk_export) << sdk_export.error().message;
  EXPECT_EQ(sdk_export->package_id, cli_export_json.at("package_id").get<std::string>());
  EXPECT_EQ(sdk_export->required_extension, ".axksbnk");
  EXPECT_EQ(read_bytes(sdk_export->output_path), read_bytes(package));
#endif

  for (const auto command : {"inspect", "verify"}) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(run_cli({"axklib", "package", command, package.string(), "--format", "json"}), 0);
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("\"valid\":true"), std::string::npos) << command;
    EXPECT_NE(output.find("\"package_kind\":\"sbnk\""), std::string::npos) << command;
    const auto expected_verification = std::string{"\"payloads_verified\":"} +
                                       (std::string_view{command} == "verify" ? "true" : "false");
    EXPECT_NE(output.find(expected_verification), std::string::npos) << command;
  }

  std::ofstream{manifest}
      << R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"Target","volumes":[{"name":"Imported","waveforms":[],"sample_banks":[]}]}]})";
  ASSERT_EQ(run_cli({"axklib", "create", "hds", manifest.string(), "-o", target.string()}), 0);
  ASSERT_TRUE(std::filesystem::is_regular_file(target));
  const std::string destination = R"({"package":0,"root":0,"partition":0,"volume":"Imported"})";

  testing::internal::CaptureStdout();
  ASSERT_EQ(run_cli({"axklib", "package", "plan-import", target.string(), package.string(),
                     "--destination", destination, "--format", "json"}),
            0);
  const auto plan_output = testing::internal::GetCapturedStdout();
  EXPECT_NE(plan_output.find("\"valid\":true"), std::string::npos);
  EXPECT_NE(plan_output.find("\"object_type\":\"SBNK\""), std::string::npos);
  EXPECT_NE(plan_output.find("\"object_type\":\"SMPL\""), std::string::npos);
  EXPECT_NE(plan_output.find("\"result\":null"), std::string::npos);

#ifdef AXK_TEST_SHARED_SDK
  const auto cli_plan_json = nlohmann::json::parse(plan_output);
  axk::package_import_request sdk_request;
  axk::package_root_destination sdk_destination;
  sdk_destination.package_index = 0U;
  sdk_destination.root_index = 0U;
  sdk_destination.partition_index = 0U;
  sdk_destination.volume_name = "Imported";
  sdk_request.root_destinations.push_back(std::move(sdk_destination));
  auto sdk_plan = axk::package_import_plan::create(target.string(), {package.string()}, sdk_request,
                                                   sdk_context);
  ASSERT_TRUE(sdk_plan) << sdk_plan.error().message;
  const auto sdk_plan_summary = sdk_plan->summary();
  ASSERT_TRUE(sdk_plan_summary) << sdk_plan_summary.error().message;
  EXPECT_TRUE(sdk_plan_summary->valid);
  EXPECT_EQ(sdk_plan_summary->plan_id, cli_plan_json.at("plan_id").get<std::string>());
  EXPECT_EQ(sdk_plan_summary->target_snapshot_id,
            cli_plan_json.at("target_snapshot_id").get<std::string>());
  EXPECT_EQ(sdk_plan_summary->object_count, cli_plan_json.at("objects").size());
#endif

  testing::internal::CaptureStdout();
  ASSERT_EQ(run_cli({"axklib", "package", "import", target.string(), package.string(),
                     "--destination", destination, "-o", imported.string(), "--format", "json"}),
            0);
  const auto import_output = testing::internal::GetCapturedStdout();
  EXPECT_NE(import_output.find("\"applied\":true"), std::string::npos);
  ASSERT_TRUE(std::filesystem::is_regular_file(imported));
  const auto original = read_bytes(imported);

  testing::internal::CaptureStderr();
  EXPECT_NE(run_cli({"axklib", "package", "import", target.string(), package.string(),
                     "--destination", destination, "-o", imported.string()}),
            0);
  EXPECT_FALSE(testing::internal::GetCapturedStderr().empty());
  EXPECT_EQ(read_bytes(imported), original);

  std::filesystem::remove_all(root, error);
}

TEST(Cli11Adapter, PortablePackageRejectsBadSelectorsMappingsAndConflictsCleanly) {
  const auto fixture = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images" /
                       "sampler-authored/HD00_512_single_sbnk_authored.hds";
  const auto root = std::filesystem::temp_directory_path() / "axklib-cli-package-errors";
  const auto package = root / "sine.axksbnk";
  const auto target = root / "target.hds";
  const auto manifest = root / "target.json";
  const auto output = root / "must-not-exist.hds";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  ASSERT_TRUE(std::filesystem::create_directories(root));

  testing::internal::CaptureStderr();
  EXPECT_EQ(run_cli({"axklib", "package", "export", fixture.string(), "--root", "nonsense=x", "-o",
                     (root / "bad").string()}),
            2);
  EXPECT_NE(testing::internal::GetCapturedStderr().find("root kind"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(root / "bad"));

  ASSERT_EQ(run_cli({"axklib", "package", "export", fixture.string(), "--root", "sbnk=sine wave",
                     "--partition", "0", "--group", "New Partition", "--volume", "New Volume", "-o",
                     package.string()}),
            0);
  std::ofstream{manifest}
      << R"({"schema_version":"1.0","size_bytes":1048576,"partitions":[{"name":"Target","volumes":[{"name":"Imported","waveforms":[],"sample_banks":[]}]}]})";
  ASSERT_EQ(run_cli({"axklib", "create", "hds", manifest.string(), "-o", target.string()}), 0);

  testing::internal::CaptureStderr();
  EXPECT_EQ(run_cli({"axklib", "package", "plan-import", target.string(), package.string(),
                     "--destination", "not-json"}),
            2);
  EXPECT_NE(testing::internal::GetCapturedStderr().find("destination JSON"), std::string::npos);

  testing::internal::CaptureStdout();
  EXPECT_EQ(run_cli({"axklib", "package", "import", target.string(), package.string(),
                     "--destination", R"({"package":0,"root":0,"partition":0,"volume":"Missing"})",
                     "-o", output.string(), "--format", "json"}),
            3);
  const auto conflict = testing::internal::GetCapturedStdout();
  EXPECT_NE(conflict.find("\"valid\":false"), std::string::npos);
  EXPECT_NE(conflict.find("SFS_DESTINATION_MISSING"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(output));

  std::filesystem::remove_all(root, error);
}

TEST(Cli11Adapter, ExposesHdsFloppyAndIsoCreationProfiles) {
  std::array help{const_cast<char *>("axklib"), const_cast<char *>("create"),
                  const_cast<char *>("--help")};
  testing::internal::CaptureStdout();
  EXPECT_EQ(axk::cli::run(static_cast<int>(help.size()), help.data()), 0);
  const auto output = testing::internal::GetCapturedStdout();
  for (const auto command : {"hds", "floppy", "iso"})
    EXPECT_NE(output.find(command), std::string::npos) << command;
}

TEST(Cli11Adapter, ExtractSfzWritesAudioInstrumentsAndVolumeGraph) {
  const auto fixture = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images" /
                       "sampler-authored/HD00_512_multi_sbnk_authored.hds";
  const auto output =
      std::filesystem::temp_directory_path() / "axklib-cli-extract-integration-test";
  std::error_code error;
  std::filesystem::remove_all(output, error);
  std::vector<std::string> arguments{"axklib",         "extract", "sfz",          "file",
                                     fixture.string(), "-o",      output.string()};
  std::vector<char *> argv;
  std::ranges::transform(arguments, std::back_inserter(argv),
                         [](auto &value) { return value.data(); });

  ASSERT_EQ(axk::cli::run(static_cast<int>(argv.size()), argv.data()), 0);
  std::size_t wav_count{};
  std::size_t sfz_count{};
  std::size_t graph_count{};
  for (const auto &entry : std::filesystem::recursive_directory_iterator{output}) {
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() == ".wav")
      ++wav_count;
    else if (entry.path().extension() == ".sfz")
      ++sfz_count;
    else if (entry.path().filename() == "volume.axklib.json")
      ++graph_count;
    EXPECT_FALSE(entry.path().filename().string().ends_with(".tmp"));
  }
  EXPECT_EQ(wav_count, 2U);
  EXPECT_EQ(sfz_count, 20U);
  EXPECT_EQ(graph_count, 1U);
  std::filesystem::remove_all(output, error);
}

TEST(CliArchitecture, KeepsCli11AndJsonAtTheirOwnedBoundaries) {
  const auto root = std::filesystem::path{AXK_SOURCE_ROOT} / "apps/cli";
  const auto read = [](const std::filesystem::path &path) {
    std::ifstream stream{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{stream}, {}};
  };
  const auto app = read(root / "app.cpp");
  EXPECT_LE(static_cast<std::size_t>(std::ranges::count(app, '\n')), 420U);
  EXPECT_NE(app.find("#include <CLI/CLI.hpp>"), std::string::npos);
  EXPECT_EQ(app.find("nlohmann"), std::string::npos);
  EXPECT_EQ(app.find("sha1"), std::string::npos);

  for (const auto &entry : std::filesystem::directory_iterator{root / "commands"}) {
    if (entry.path().extension() != ".cpp")
      continue;
    const auto source = read(entry.path());
    EXPECT_EQ(source.find("CLI/CLI.hpp"), std::string::npos) << entry.path();
    EXPECT_EQ(source.find("\"schema_version\""), std::string::npos) << entry.path();
  }
  for (const auto &entry : std::filesystem::directory_iterator{root / "schema"}) {
    if (entry.path().extension() == ".hpp") {
      EXPECT_EQ(read(entry.path()).find("nlohmann"), std::string::npos) << entry.path();
    }
  }
}
