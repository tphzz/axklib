#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app.hpp"
#include "commands/package_projection.hpp"
#include "commands/support.hpp"
#include "content_id.hpp"
#include "exit_status.hpp"
#include "local_operations.hpp"
#include "schema/operations_v1.hpp"
#include "schema/package_v1.hpp"

#include "axklib/alteration.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/write_operations.hpp"
#include "axklib/audio.hpp"
#include "axklib/package.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"
#include "axklib/wav_stream.hpp"

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
    std::ranges::transform(arguments, std::back_inserter(argv), [](auto &value) { return value.data(); });
    return axk::cli::run(static_cast<int>(argv.size()), argv.data());
}

std::string read_bytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

std::map<std::string, std::string> read_artifact_tree(const std::filesystem::path &root) {
    std::map<std::string, std::string> result;
    for (const auto &entry : std::filesystem::recursive_directory_iterator{root}) {
        if (entry.is_regular_file())
            result.emplace(entry.path().lexically_relative(root).generic_string(), read_bytes(entry.path()));
    }
    return result;
}

class CurrentPathGuard {
  public:
    explicit CurrentPathGuard(const std::filesystem::path &path) : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }
    ~CurrentPathGuard() { std::filesystem::current_path(original_); }

    CurrentPathGuard(const CurrentPathGuard &) = delete;
    CurrentPathGuard &operator=(const CurrentPathGuard &) = delete;

  private:
    std::filesystem::path original_;
};

} // namespace

TEST(ContentId, MatchesPublishedSha1VectorsAndStablePooledName) {
    const std::vector<std::byte> empty;
    EXPECT_EQ(detail::sha1_content_id(empty).digest_hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    EXPECT_EQ(detail::sha1_content_id(bytes).digest_hex, "a9993e364706816aba3e25717850c26c9cd0d89d");

    axk::Waveform waveform;
    waveform.format = {.channels = 1U, .sample_width_bytes = 1U, .sample_rate = 44'100U};
    waveform.frame_count = bytes.size();
    waveform.pcm.assign(bytes.begin(), bytes.end());
    const auto wav = axk::wav_bytes(waveform);
    ASSERT_TRUE(wav);
    const auto wav_id = detail::sha1_content_id(*wav);
    detail::PooledPathAllocator paths;
    const auto pooled =
        paths.allocate("file", "physical", "Sample", axk::audio_internal::WavSource::from_physical(waveform));
    ASSERT_TRUE(pooled);
    EXPECT_EQ(pooled->filename(), "Sample__" + wav_id.digest_hex.substr(0U, 12U) + ".wav");
}

TEST(CliPathExpansion, RejectsInputsThatCannotBeInspected) {
    const auto missing = std::filesystem::temp_directory_path() / "axklib-cli-missing-scan-root";
    std::error_code error;
    std::filesystem::remove_all(missing, error);

    const auto expanded = axk::cli::commands::expand_cli_paths({missing});
    ASSERT_FALSE(expanded);
    EXPECT_EQ(expanded.error().code, axk::ErrorCode::io_read_failed);
}

TEST(ContentId, ReusesEqualContentAndRejectsInjectedShortPrefixCollision) {
    const auto fake = [](const axk::audio_internal::WavSource &source) -> axk::Result<detail::ContentId> {
        const auto tail = source.physical->pcm.front() == std::byte{1} ? std::string(28U, '0') : std::string(28U, '1');
        return detail::ContentId{"sha1", "aaaaaaaaaaaa" + tail};
    };
    detail::PooledPathAllocator paths{fake};
    axk::Waveform first;
    first.format = {.channels = 1U, .sample_width_bytes = 1U, .sample_rate = 44'100U};
    first.frame_count = 1U;
    first.pcm = {std::byte{1}};
    auto second = first;
    second.pcm = {std::byte{2}};
    const auto initial =
        paths.allocate("file", "physical", "Sample", axk::audio_internal::WavSource::from_physical(first));
    ASSERT_TRUE(initial);
    const auto reused =
        paths.allocate("file", "physical", "Sample", axk::audio_internal::WavSource::from_physical(first));
    ASSERT_TRUE(reused);
    EXPECT_EQ(*reused, *initial);
    const auto collision =
        paths.allocate("file", "physical", "Sample", axk::audio_internal::WavSource::from_physical(second));
    ASSERT_FALSE(collision);
    EXPECT_NE(collision.error().message.find("distinct WAV contents"), std::string::npos);
}

TEST(Cli11Adapter, ReturnsParserExitCodesWithoutProcessExitMacros) {
    std::array help{const_cast<char *>("axklib"), const_cast<char *>("--help")};
    EXPECT_EQ(axk::cli::run(static_cast<int>(help.size()), help.data()), 0);

    std::array invalid{const_cast<char *>("axklib"), const_cast<char *>("--not-an-option")};
    EXPECT_NE(axk::cli::run(static_cast<int>(invalid.size()), invalid.data()), 0);
}

TEST(Cli11Adapter, PublicExitCategoriesRemainCentralizedAndComplete) {
    using axk::cli::ExitStatus;
    EXPECT_EQ(axk::cli::exit_code(ExitStatus::success), 0);
    EXPECT_EQ(axk::cli::exit_code(ExitStatus::operational_failure), 1);
    EXPECT_EQ(axk::cli::exit_code(ExitStatus::invalid_request), 2);
    EXPECT_EQ(axk::cli::exit_code(ExitStatus::diagnostics), 3);
    EXPECT_EQ(axk::cli::application_error_status("invalid_request"), ExitStatus::invalid_request);
    EXPECT_EQ(axk::cli::application_error_status("selector_not_found"), ExitStatus::invalid_request);
    EXPECT_EQ(axk::cli::application_error_status("package_extension_mismatch"), ExitStatus::invalid_request);
    EXPECT_EQ(axk::cli::application_error_status("write_plan_not_found"), ExitStatus::invalid_request);
    EXPECT_EQ(axk::cli::application_error_status("operation_cancelled"), ExitStatus::operational_failure);
    EXPECT_EQ(axk::cli::application_error_status("publication_failed"), ExitStatus::operational_failure);
    EXPECT_EQ(axk::cli::core_error_status(
                  axk::make_error(axk::ErrorCode::container_invalid_geometry, axk::ErrorCategory::container, "bad")),
              ExitStatus::invalid_request);
    EXPECT_EQ(axk::cli::core_error_status(
                  axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io, "unreadable")),
              ExitStatus::operational_failure);
    EXPECT_EQ(axk::cli::core_error_status(
                  axk::make_error(axk::ErrorCode::internal_invariant, axk::ErrorCategory::internal, "broken")),
              ExitStatus::operational_failure);
}

TEST(Cli11Adapter, MissingExtractionSelectorUsesTheDocumentedInvalidRequestStatus) {
    EXPECT_EQ(run_cli({"axklib", "extract", "wav", "--scope", "sbnk", "missing.hds", "-o", "out"}),
              axk::cli::exit_code(axk::cli::ExitStatus::invalid_request));
}

TEST(Cli11Adapter, ReportsSemanticAndSourceVersionsSeparately) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(run_cli({"axklib", "--version"}), 0);
    const auto output = normalize_newlines(testing::internal::GetCapturedStdout());
    const auto build = axk::current_build_info();
    const auto selected_ref = build.is_tagged_release ? build.git_tag : build.git_branch;
    const auto expected = std::string{"axklib "} + build.source_identity + "\nversion: " + std::string{axk::version()} +
                          "\npackage: " + build.package_basename + "\ngit: " + build.git_sha_short +
                          "\nref: " + selected_ref + "\nsource: " + (build.is_dirty ? "modified" : "clean") + "\n";
    EXPECT_EQ(output, expected);
}

TEST(Cli11Adapter, ShowsVersionAndSourceIdentityInDefaultBanner) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(run_cli({"axklib"}), 0);
    const auto output = normalize_newlines(testing::internal::GetCapturedStdout());
    const auto expected_prefix = std::string{"axklib "} + std::string{axk::version()} + " (" +
                                 axk::current_build_info().source_identity + ")\n\naxklib [OPTIONS] [SUBCOMMANDS]\n";
    EXPECT_EQ(output.substr(0U, expected_prefix.size()), expected_prefix);
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

TEST(Cli11Adapter, ExposesOnlyMaintainedCommandInventory) {
    std::array help{const_cast<char *>("axklib"), const_cast<char *>("--help")};
    testing::internal::CaptureStdout();
    EXPECT_EQ(axk::cli::run(static_cast<int>(help.size()), help.data()), 0);
    const auto output = testing::internal::GetCapturedStdout();
    for (const auto command : {"info", "inventory", "validate", "relationships", "coverage", "create", "alter",
                               "orphans", "objects", "corpus", "extract", "package"})
        EXPECT_NE(output.find(command), std::string::npos) << command;
    for (const auto command : {"create-hds", "object-json", "tree", "extract-wav", "export", "preview"}) {
        EXPECT_EQ(output.find(command), std::string::npos) << command;
        testing::internal::CaptureStderr();
        EXPECT_NE(run_cli({"axklib", command}), 0) << command;
        static_cast<void>(testing::internal::GetCapturedStderr());
    }

    const auto fixture = std::filesystem::path{AXK_SOURCE_ROOT} / "library/tests/fixtures/cli/help.txt";
    std::ifstream stream{fixture, std::ios::binary};
    ASSERT_TRUE(stream);
    std::string expected{std::istreambuf_iterator<char>{stream}, {}};
    const auto version_marker = expected.find("@AXK_VERSION@");
    ASSERT_NE(version_marker, std::string::npos);
    expected.replace(version_marker, std::string{"@AXK_VERSION@"}.size(), axk::version());
    const auto identity_marker = expected.find("@AXK_SOURCE_IDENTITY@");
    ASSERT_NE(identity_marker, std::string::npos);
    expected.replace(identity_marker, std::string{"@AXK_SOURCE_IDENTITY@"}.size(),
                     axk::current_build_info().source_identity);
    EXPECT_EQ(normalize_newlines(output), normalize_newlines(expected));
}

TEST(Cli11Adapter, ExposesCompletePortablePackageCommandFamily) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(run_cli({"axklib", "package", "--help"}), 0);
    const auto output = testing::internal::GetCapturedStdout();
    for (const auto command : {"export", "inspect", "verify", "plan-import", "import"})
        EXPECT_NE(output.find(command), std::string::npos) << command;
}

TEST(Cli11Adapter, HidesUnavailablePortablePackageChoices) {
    testing::internal::CaptureStdout();
    EXPECT_EQ(run_cli({"axklib", "package", "export", "--help"}), 0);
    const auto export_help = testing::internal::GetCapturedStdout();
    EXPECT_EQ(export_help.find("sequence"), std::string::npos);

    testing::internal::CaptureStdout();
    EXPECT_EQ(run_cli({"axklib", "package", "plan-import", "--help"}), 0);
    const auto import_help = testing::internal::GetCapturedStdout();
    EXPECT_EQ(import_help.find("--reuse-scope"), std::string::npos);

    const auto fixture = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images" /
                         "sampler-authored/HD00_512_single_sbnk_authored.hds";
    testing::internal::CaptureStderr();
    EXPECT_NE(run_cli({"axklib", "package", "export", fixture.string(), "--root", "sequence=001", "--partition", "0",
                       "--group", "New Partition", "--volume", "New Volume", "-o", "unused.axkseq"}),
              0);
    const auto error = testing::internal::GetCapturedStderr();
    EXPECT_NE(error.find("package root kind must be"), std::string::npos);
}

TEST(Cli11Adapter, UsesCanonicalSampleBankSampleAndWaveDataPackageSelectors) {
    const auto fixture = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images" /
                         "sampler-authored/HD00_512_single_sbnk_authored.hds";
    const auto root = std::filesystem::temp_directory_path() / "axklib-cli-package-terminology";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "package", "export", fixture.string(), "--root", "sample=sine wave", "--partition",
                       "0", "--volume", "New Volume", "-o", (root / "sample").string(), "--format", "json"}),
              0);
    const auto sample_output = nlohmann::json::parse(testing::internal::GetCapturedStdout());
    EXPECT_EQ(sample_output.at("package_kind"), "sbnk");

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "package", "export", fixture.string(), "--root", "wave-data=sine wave", "--partition",
                       "0", "--volume", "New Volume", "-o", (root / "wave-data").string(), "--format", "json"}),
              0);
    const auto wave_data_output = nlohmann::json::parse(testing::internal::GetCapturedStdout());
    EXPECT_EQ(wave_data_output.at("package_kind"), "smpl");

    testing::internal::CaptureStdout();
    ASSERT_EQ(
        run_cli({"axklib", "package", "export", fixture.string(), "--root", "sample-bank=New SmpBank", "--partition",
                 "0", "--volume", "New Volume", "-o", (root / "sample-bank").string(), "--format", "json"}),
        0);
    const auto sample_bank_output = nlohmann::json::parse(testing::internal::GetCapturedStdout());
    EXPECT_EQ(sample_bank_output.at("package_kind"), "sbac");

    testing::internal::CaptureStdout();
    ASSERT_EQ(
        run_cli({"axklib", "package", "export", fixture.string(), "--root", "bank-group=New SmpBank", "--partition",
                 "0", "--volume", "New Volume", "-o", (root / "bank-group").string(), "--format", "json"}),
        0);
    const auto legacy_sample_bank_output = nlohmann::json::parse(testing::internal::GetCapturedStdout());
    EXPECT_EQ(legacy_sample_bank_output.at("package_kind"), "sbac");

    std::filesystem::remove_all(root, error);
}

TEST(Cli11Adapter, WritesStarterBuildManifestsWithoutSilentReplacement) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-cli-manifest-template";
    const auto output = root / "nested" / "image.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "create", "manifest", "hds", "-o", output.string()}), 0);
    EXPECT_NE(testing::internal::GetCapturedStdout().find("kind=hds"), std::string::npos);
    ASSERT_TRUE(std::filesystem::is_regular_file(output));
    const auto parsed = nlohmann::json::parse(read_bytes(output));
    EXPECT_EQ(parsed.at("schema_version"), "1.1");
    EXPECT_EQ(parsed.at("size_bytes"), 536'870'912U);
    EXPECT_TRUE(parsed.at("partitions").at(0).at("volumes").empty());

    testing::internal::CaptureStderr();
    EXPECT_EQ(run_cli({"axklib", "create", "manifest", "iso", "-o", output.string()}), 2);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("refusing to replace"), std::string::npos);

    EXPECT_EQ(run_cli({"axklib", "create", "manifest", "iso", "-o", output.string(), "--overwrite"}), 0);
    const auto replaced = nlohmann::json::parse(read_bytes(output));
    EXPECT_EQ(replaced.at("format"), "iso9660");
    std::filesystem::remove_all(root, error);
}

TEST(Cli11Adapter, WritesStarterAlterationManifestWithoutSilentReplacement) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-cli-alter-manifest-template";
    const auto output = root / "transaction.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    ASSERT_EQ(run_cli({"axklib", "alter", "manifest", "-o", output.string()}), 0);
    ASSERT_TRUE(std::filesystem::is_regular_file(output));
    const auto parsed = axk::load_alteration_manifest(output);
    ASSERT_TRUE(parsed) << parsed.error().message;

    EXPECT_EQ(run_cli({"axklib", "alter", "manifest", "-o", output.string()}), 2);
    EXPECT_EQ(run_cli({"axklib", "alter", "manifest", "-o", output.string(), "--overwrite"}), 0);

    std::filesystem::remove_all(root, error);
}

TEST(Cli11Adapter, WritesExactCanonicalManifestBytesFromSharedOperations) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_manifest_operations(registry));
    const axk::app::OperationContext context{
        .owner_id = "test", .request_id = "test", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto build = registry.invoke("create.manifest", {{"kind", "HDS"}}, context);
    ASSERT_TRUE(build) << build.error().message;
    const auto alteration = registry.invoke("alter.manifest", nlohmann::json::object(), context);
    ASSERT_TRUE(alteration) << alteration.error().message;

    const auto root = std::filesystem::temp_directory_path() / "axklib-cli-shared-manifest-template";
    const auto build_path = root / "build.json";
    const auto alteration_path = root / "alteration.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    ASSERT_EQ(run_cli({"axklib", "create", "manifest", "hds", "-o", build_path.string()}), 0);
    ASSERT_EQ(run_cli({"axklib", "alter", "manifest", "-o", alteration_path.string()}), 0);
    EXPECT_EQ(read_bytes(build_path), build->at("canonicalJson").get<std::string>());
    EXPECT_EQ(read_bytes(alteration_path), alteration->at("canonicalJson").get<std::string>());

    std::filesystem::remove_all(root, error);
}

TEST(Cli11Adapter, PortablePackageRoundTripPlansAndImportsAtomically) {
    const auto fixture = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images" /
                         "sampler-authored/HD00_512_single_sbnk_authored.hds";
    const auto root =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / "axklib-cli-package-round-trip";
    const auto package_stem = root / "sine-sample";
    const auto package = root / "sine-sample.axksbnk";
    const auto manifest = root / "target.json";
    const auto target = root / "target.hds";
    const auto imported = root / "imported.hds";
    const auto expected_imported = root / "expected-imported.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_TRUE(std::filesystem::create_directories(root));

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "package", "export", fixture.string(), "--root", "sbnk=sine wave", "--partition", "0",
                       "--group", "New Partition", "--volume", "New Volume", "-o", package_stem.string(), "--format",
                       "json"}),
              0);
    const auto export_output = testing::internal::GetCapturedStdout();
    EXPECT_NE(export_output.find("\"package_kind\":\"sbnk\""), std::string::npos);
    EXPECT_NE(export_output.find("\"required_extension\":\".axksbnk\""), std::string::npos);
    ASSERT_TRUE(std::filesystem::is_regular_file(package));
    auto direct_export = axk::open_portable_package(package);
    ASSERT_TRUE(direct_export) << direct_export.error().message;
    auto expected_export = axk::cli::schema::package_v1::serialize(
        axk::cli::schema::package_v1::project_package(package, *direct_export), false);
    ASSERT_TRUE(expected_export) << expected_export.error().message;
    EXPECT_EQ(export_output, *expected_export + '\n');

#ifdef AXK_TEST_SHARED_SDK
    const auto cli_export_json = nlohmann::json::parse(export_output);
    axk::operation_context sdk_context;
    axk::package_root_selector sdk_selector;
    sdk_selector.kind = axk::package_root_kind::sample;
    sdk_selector.partition_index = 0U;
    sdk_selector.group_name = "New Partition";
    sdk_selector.volume_name = "New Volume";
    sdk_selector.object_name = "sine wave";
    const auto sdk_export = axk::portable_package::export_from(fixture.string(), {sdk_selector},
                                                               (root / "sdk-sine-sample").string(), {}, sdk_context);
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
        const auto expected_verification =
            std::string{"\"payloads_verified\":"} + (std::string_view{command} == "verify" ? "true" : "false");
        EXPECT_NE(output.find(expected_verification), std::string::npos) << command;

        auto direct = std::string_view{command} == "verify" ? axk::open_portable_package(package)
                                                            : axk::inspect_portable_package(package);
        ASSERT_TRUE(direct) << direct.error().message;
        if (std::string_view{command} == "verify") {
            ASSERT_TRUE(axk::verify_portable_package(*direct));
        }
        auto expected = axk::cli::schema::package_v1::serialize(
            axk::cli::schema::package_v1::project_package(package, *direct), false);
        ASSERT_TRUE(expected) << expected.error().message;
        EXPECT_EQ(output, *expected + '\n') << command;
    }

    std::ofstream{manifest}
        << R"({"schema_version":"1.1","size_bytes":1048576,"partitions":[{"name":"Target","volumes":[{"name":"Imported","waveforms":[],"samples":[]}]}]})";
    ASSERT_EQ(run_cli({"axklib", "create", "hds", manifest.string(), "-o", target.string()}), 0);
    ASSERT_TRUE(std::filesystem::is_regular_file(target));
    const std::string destination = R"({"package":0,"root":0,"partition":0,"volume":"Imported"})";

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "package", "plan-import", target.string(), package.string(), "--destination",
                       destination, "--format", "json"}),
              0);
    const auto plan_output = testing::internal::GetCapturedStdout();
    EXPECT_NE(plan_output.find("\"valid\":true"), std::string::npos);
    EXPECT_NE(plan_output.find("\"object_type\":\"SBNK\""), std::string::npos);
    EXPECT_NE(plan_output.find("\"object_type\":\"SMPL\""), std::string::npos);
    EXPECT_NE(plan_output.find("\"result\":null"), std::string::npos);

    axk::PackageImportRequest direct_request;
    axk::PackageRootDestination direct_destination;
    direct_destination.package_index = 0U;
    direct_destination.root_index = 0U;
    direct_destination.partition_index = 0U;
    direct_destination.volume_name = "Imported";
    direct_request.root_destinations.push_back(std::move(direct_destination));
    const std::array direct_packages{*direct_export};
    auto direct_plan = axk::plan_package_import(target, direct_packages, direct_request);
    ASSERT_TRUE(direct_plan) << direct_plan.error().message;
    auto expected_plan = axk::cli::schema::package_v1::serialize(
        axk::cli::schema::package_v1::project_plan(target, {package}, *direct_plan), false);
    ASSERT_TRUE(expected_plan) << expected_plan.error().message;
    EXPECT_EQ(plan_output, *expected_plan + '\n');

#ifdef AXK_TEST_SHARED_SDK
    const auto cli_plan_json = nlohmann::json::parse(plan_output);
    axk::package_import_request sdk_request;
    axk::package_root_destination sdk_destination;
    sdk_destination.package_index = 0U;
    sdk_destination.root_index = 0U;
    sdk_destination.partition_index = 0U;
    sdk_destination.volume_name = "Imported";
    sdk_request.root_destinations.push_back(std::move(sdk_destination));
    auto sdk_plan = axk::package_import_plan::create(target.string(), {package.string()}, sdk_request, sdk_context);
    ASSERT_TRUE(sdk_plan) << sdk_plan.error().message;
    const auto sdk_plan_summary = sdk_plan->summary();
    ASSERT_TRUE(sdk_plan_summary) << sdk_plan_summary.error().message;
    EXPECT_TRUE(sdk_plan_summary->valid);
    EXPECT_EQ(sdk_plan_summary->plan_id, cli_plan_json.at("plan_id").get<std::string>());
    EXPECT_EQ(sdk_plan_summary->target_snapshot_id, cli_plan_json.at("target_snapshot_id").get<std::string>());
    EXPECT_EQ(sdk_plan_summary->object_count, cli_plan_json.at("objects").size());
#endif

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "package", "import", target.string(), package.string(), "--destination", destination,
                       "-o", imported.string(), "--format", "json"}),
              0);
    const auto import_output = testing::internal::GetCapturedStdout();
    EXPECT_NE(import_output.find("\"applied\":true"), std::string::npos);
    ASSERT_TRUE(std::filesystem::is_regular_file(imported));
    auto direct_report = axk::apply_package_import(target, direct_packages, *direct_plan, expected_imported);
    ASSERT_TRUE(direct_report) << direct_report.error().message;
    EXPECT_EQ(read_bytes(imported), read_bytes(expected_imported));
    direct_report->output_path = imported;
    auto expected_import = axk::cli::schema::package_v1::serialize(
        axk::cli::schema::package_v1::project_plan(target, {package}, *direct_plan, *direct_report), false);
    ASSERT_TRUE(expected_import) << expected_import.error().message;
    EXPECT_EQ(import_output, *expected_import + '\n');
    const auto original = read_bytes(imported);

    testing::internal::CaptureStderr();
    EXPECT_NE(run_cli({"axklib", "package", "import", target.string(), package.string(), "--destination", destination,
                       "-o", imported.string()}),
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

    ASSERT_EQ(run_cli({"axklib", "package", "export", fixture.string(), "--root", "sbnk=sine wave", "--partition", "0",
                       "--group", "New Partition", "--volume", "New Volume", "-o", package.string()}),
              0);
    std::ofstream{manifest}
        << R"({"schema_version":"1.1","size_bytes":1048576,"partitions":[{"name":"Target","volumes":[{"name":"Imported","waveforms":[],"samples":[]}]}]})";
    ASSERT_EQ(run_cli({"axklib", "create", "hds", manifest.string(), "-o", target.string()}), 0);

    testing::internal::CaptureStderr();
    EXPECT_EQ(
        run_cli({"axklib", "package", "plan-import", target.string(), package.string(), "--destination", "not-json"}),
        2);
    EXPECT_NE(testing::internal::GetCapturedStderr().find("destination JSON"), std::string::npos);

    testing::internal::CaptureStdout();
    EXPECT_EQ(run_cli({"axklib", "package", "import", target.string(), package.string(), "--destination",
                       R"({"package":0,"root":0,"partition":0,"volume":"Missing"})", "-o", output.string(), "--format",
                       "json"}),
              3);
    const auto conflict = testing::internal::GetCapturedStdout();
    EXPECT_NE(conflict.find("\"valid\":false"), std::string::npos);
    EXPECT_NE(conflict.find("SFS_DESTINATION_MISSING"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));

    std::filesystem::remove_all(root, error);
}

TEST(Cli11Adapter, ExposesHdsFloppyAndIsoCreationProfiles) {
    std::array help{const_cast<char *>("axklib"), const_cast<char *>("create"), const_cast<char *>("--help")};
    testing::internal::CaptureStdout();
    EXPECT_EQ(axk::cli::run(static_cast<int>(help.size()), help.data()), 0);
    const auto output = testing::internal::GetCapturedStdout();
    for (const auto command : {"hds", "floppy", "iso"})
        EXPECT_NE(output.find(command), std::string::npos) << command;
}

TEST(Cli11Adapter, CreateHdsInvokesSharedPlanAndMatchesDirectWriter) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-cli-create-hds-parity-test";
    const auto manifest_path = root / "manifest.json";
    const auto direct_path = root / "direct.hds";
    const auto cli_path = root / "cli.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_TRUE(std::filesystem::create_directory(root, error));
    const auto manifest_text =
        R"({"schema_version":"1.1","size_bytes":1048576,"partitions":[{"name":"Parity","volumes":[{"name":"Empty","waveforms":[],"samples":[]}]}]})";
    std::ofstream{manifest_path, std::ios::binary} << manifest_text;
    const auto manifest = axk::parse_hds_build_manifest(manifest_text);
    ASSERT_TRUE(manifest) << axk::render_error(manifest.error());
    const auto direct = axk::write_hds_image(*manifest, direct_path, false);
    ASSERT_TRUE(direct) << axk::render_error(direct.error());

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "create", "hds", manifest_path.string(), "-o", cli_path.string()}), 0);
    const auto output = testing::internal::GetCapturedStdout();
    std::ostringstream expected;
    expected << "image=" << axk::text::path_to_utf8(cli_path) << " size_bytes=" << direct->size_bytes
             << " partitions=" << direct->partitions.size()
             << " objects=0 unused_tail_sectors=" << direct->unused_tail_sectors << '\n';
    for (const auto &partition : direct->partitions) {
        expected << "partition=" << static_cast<unsigned int>(partition.geometry.index) << " name='" << partition.name
                 << "' start_sector=" << partition.geometry.start_sector
                 << " sector_count=" << partition.geometry.filesystem_sector_count
                 << " cluster_count=" << partition.geometry.cluster_count
                 << " free_kib=" << partition.sampler_visible_free_kib << '\n';
    }
    EXPECT_EQ(output, expected.str());
    EXPECT_EQ(read_bytes(cli_path), read_bytes(direct_path));
    std::filesystem::remove_all(root, error);
}

TEST(Cli11Adapter, CreateFloppyAndIsoInvokeSharedPlanAndMatchDirectWriter) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-cli-create-media-parity-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_TRUE(std::filesystem::create_directory(root, error));

    axk::Waveform waveform;
    waveform.format = {.channels = 1U, .sample_width_bytes = 2U, .sample_rate = 44'100U};
    waveform.frame_count = 4U;
    waveform.pcm = {std::byte{0},    std::byte{0},    std::byte{1}, std::byte{0},
                    std::byte{0xff}, std::byte{0xff}, std::byte{0}, std::byte{0}};
    const auto wav = axk::wav_bytes(waveform);
    ASSERT_TRUE(wav) << axk::render_error(wav.error());
    const auto tone_path = root / "tone.wav";
    std::ofstream tone{tone_path, std::ios::binary};
    tone.write(reinterpret_cast<const char *>(wav->data()), static_cast<std::streamsize>(wav->size()));
    ASSERT_TRUE(tone);
    tone.close();

    const std::array cases{
        nlohmann::json{
            {"kind", "floppy"},
            {"format", "fat12_floppy"},
            {"manifest",
             {{"schema_version", "1.1"},
              {"format", "fat12_floppy"},
              {"authored_volume",
               {{"name", "FAT ROOT"},
                {"waveforms",
                 {{{"id", "tone"}, {"name", "Authored Tone"}, {"path", tone_path.string()}, {"root_key", 60}}}},
                {"samples",
                 {{{"name", "Authored Tone"},
                   {"waveform_id", "tone"},
                   {"root_key", 60},
                   {"key_low", 60},
                   {"key_high", 60},
                   {"level", 100}}}}}}}}},
        nlohmann::json{{"kind", "iso"},
                       {"format", "iso9660"},
                       {"manifest",
                        {{"schema_version", "1.1"},
                         {"format", "iso9660"},
                         {"iso",
                          {{"volume_id", "AXK_AUDIO"},
                           {"raw_group", "46DEF120"},
                           {"group_name", "NEW GROUP"},
                           {"raw_volume", "F001"},
                           {"volume_name", "NEW VOLUME"}}},
                         {"authored_volume",
                          {{"name", "NEW VOLUME"},
                           {"waveforms", nlohmann::json::array()},
                           {"samples", nlohmann::json::array()}}}}}}};

    for (const auto &test_case : cases) {
        const auto kind = test_case.at("kind").get<std::string>();
        const auto manifest_path = root / (kind + ".json");
        const auto direct_path = root / (kind + "-direct." + (kind == "floppy" ? "ima" : "iso"));
        const auto cli_path = root / (kind + "-cli." + (kind == "floppy" ? "ima" : "iso"));
        std::ofstream{manifest_path, std::ios::binary} << test_case.at("manifest").dump(2) << '\n';
        const auto manifest = axk::load_media_build_manifest(manifest_path);
        ASSERT_TRUE(manifest) << kind << ": " << axk::render_error(manifest.error());
        const auto direct = axk::write_media_image(*manifest, direct_path, false);
        ASSERT_TRUE(direct) << kind << ": " << axk::render_error(direct.error());

        testing::internal::CaptureStdout();
        ASSERT_EQ(run_cli({"axklib", "create", kind, manifest_path.string(), "-o", cli_path.string()}), 0) << kind;
        const auto output = testing::internal::GetCapturedStdout();
        const auto expected = std::format(
            "image={} format={} size_bytes={} objects={}\n", axk::text::path_to_utf8(cli_path),
            test_case.at("format").get_ref<const std::string &>(), direct->size_bytes, direct->object_count);
        EXPECT_EQ(output, expected) << kind;
        EXPECT_EQ(read_bytes(cli_path), read_bytes(direct_path)) << kind;
    }
    std::filesystem::remove_all(root, error);
}

TEST(Cli11Adapter, AlterHdsDryRunAndApplyInvokeSharedInspectionAndDirectAlteration) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-cli-alter-hds-parity-test";
    const auto source_path = root / "source.hds";
    const auto manifest_path = root / "alteration.json";
    const auto direct_path = root / "direct.hds";
    const auto cli_path = root / "cli.hds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_TRUE(std::filesystem::create_directory(root, error));

    const auto build_text =
        R"({"schema_version":"1.1","size_bytes":4194304,"partitions":[{"name":"Parity","volumes":[{"name":"Keep","waveforms":[],"samples":[]},{"name":"Remove","waveforms":[],"samples":[]}]}]})";
    const auto build = axk::parse_hds_build_manifest(build_text);
    ASSERT_TRUE(build) << axk::render_error(build.error());
    ASSERT_TRUE(axk::write_hds_image(*build, source_path, false));
    const auto alteration_text =
        R"({"schema_version":"1.1","operations":[{"id":"remove","type":"delete_volume","partition_index":0,"volume_name":"Remove"}]})";
    std::ofstream{manifest_path, std::ios::binary} << alteration_text;
    const auto manifest = axk::parse_alteration_manifest(alteration_text);
    ASSERT_TRUE(manifest) << axk::render_error(manifest.error());
    const auto direct_dry = axk::inspect_hds_alteration(source_path, *manifest);
    ASSERT_TRUE(direct_dry) << axk::render_error(direct_dry.error());

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "alter", "hds", source_path.string(), manifest_path.string()}), 0);
    const auto cli_dry = nlohmann::json::parse(testing::internal::GetCapturedStdout());
    const auto expected_dry = nlohmann::json::parse(*axk::cli::schema::operations_v1::serialize(
        axk::cli::schema::operations_v1::project_alteration(*direct_dry), false));
    EXPECT_EQ(cli_dry, expected_dry);

    const auto direct = axk::alter_hds(source_path, *manifest, direct_path);
    ASSERT_TRUE(direct) << axk::render_error(direct.error());
    testing::internal::CaptureStdout();
    ASSERT_EQ(
        run_cli({"axklib", "alter", "hds", source_path.string(), manifest_path.string(), "-o", cli_path.string()}), 0);
    auto cli_result = nlohmann::json::parse(testing::internal::GetCapturedStdout());
    auto expected = nlohmann::json::parse(*axk::cli::schema::operations_v1::serialize(
        axk::cli::schema::operations_v1::project_alteration(*direct), false));
    expected["output_path"] = axk::text::path_to_utf8(cli_path);
    EXPECT_EQ(cli_result, expected);
    EXPECT_EQ(read_bytes(cli_path), read_bytes(direct_path));
    std::filesystem::remove_all(root, error);
}

TEST(Cli11Adapter, ExtractSfzWritesAudioInstrumentsAndVolumeGraph) {
    const CurrentPathGuard current_path{AXK_SOURCE_ROOT};
    const auto baseline = nlohmann::json::parse(
        read_bytes(std::filesystem::path{AXK_SOURCE_ROOT} / "library/tests/fixtures/cli/extract_sfz_hashes.json"));
    const auto fixture = std::filesystem::path{baseline.at("source").get<std::string>()};
    const auto root = std::filesystem::temp_directory_path() / "axklib-cli-extract-integration-test";
    const auto output = root / "cli";
    const auto service_output = root / "service";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_TRUE(std::filesystem::create_directory(root, error));
    std::vector<std::string> arguments{"axklib", "extract", "sfz", "file", fixture.string(), "-o", output.string()};
    std::vector<char *> argv;
    std::ranges::transform(arguments, std::back_inserter(argv), [](auto &value) { return value.data(); });

    testing::internal::CaptureStdout();
    ASSERT_EQ(axk::cli::run(static_cast<int>(argv.size()), argv.data()), 0);
    EXPECT_EQ(testing::internal::GetCapturedStdout(),
              "wave_data=2 written_files=23 selection_graphs=1 sfz_files=20 decode_errors=0 load_errors=0\n");

    const std::array runtime_paths{fixture, service_output};
    auto runtime = axk::cli::LocalOperationRuntime::create(runtime_paths);
    ASSERT_TRUE(runtime) << runtime.error().message;
    auto source = (*runtime)->file_ref(fixture);
    ASSERT_TRUE(source) << source.error().message;
    auto destination = (*runtime)->directory_ref(service_output);
    ASSERT_TRUE(destination) << destination.error().message;
    auto result = (*runtime)->invoke(
        "extract.sfz",
        {{"sources", {{{"rootId", source->root_id}, {"relativePath", source->relative_path}}}},
         {"destination", {{"rootId", destination->root_id}, {"relativePath", destination->relative_path}}},
         {"scope", "file"},
         {"stereo", "auto"},
         {"overwrite", false},
         {"strict", false}});
    ASSERT_TRUE(result) << result.error().message;

    const auto &summary = baseline.at("summary");
    EXPECT_EQ(result->at("waveformCount"), summary.at("waveform_count"));
    EXPECT_EQ(result->at("writtenFileCount"), summary.at("written_file_count"));
    EXPECT_EQ(result->at("selectionGraphCount"), summary.at("selection_graph_count"));
    EXPECT_EQ(result->at("sfzFileCount"), summary.at("sfz_file_count"));
    EXPECT_EQ(result->at("decodeErrorCount"), summary.at("decode_error_count"));
    EXPECT_EQ(result->at("loadErrorCount"), summary.at("load_error_count"));
    ASSERT_EQ(result->at("artifacts").size(), baseline.at("artifacts").size());
    for (std::size_t index = 0; index < baseline.at("artifacts").size(); ++index) {
        EXPECT_EQ(result->at("artifacts").at(index).at("relativePath"),
                  baseline.at("artifacts").at(index).at("relative_path"));
        EXPECT_EQ(result->at("artifacts").at(index).at("sha256"), baseline.at("artifacts").at(index).at("sha256"));
    }
    EXPECT_EQ(read_artifact_tree(output), read_artifact_tree(service_output));
    std::filesystem::remove_all(root, error);
}

TEST(Cli11Adapter, InfoUsesCanonicalServiceDataForEveryRenderingMode) {
    const CurrentPathGuard current_path{AXK_SOURCE_ROOT};
    const auto fixture =
        std::filesystem::path{"tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds"};

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "info", fixture.string(), "--format", "summary"}), 0);
    EXPECT_EQ(testing::internal::GetCapturedStdout(),
              fixture.string() + "\tsfs\tobjects=17 SBAC=1 SBNK=8 SMPL=8\trecovery=-\n");

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "info", fixture.string(), "--format", "tree"}), 0);
    const auto tree = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(tree.starts_with(fixture.string() + " [sfs]\n`-- partition 0: New Partition [PARTITION] (1)\n"));
    EXPECT_NE(tree.find("Sample Banks/Samples (SBAC/SBNK) [CATEGORY]"), std::string::npos);
    EXPECT_NE(tree.find("B New SmpBank [SAMPLE BANK (SBAC)]"), std::string::npos);
    EXPECT_NE(tree.find("_NewSample [SAMPLE (SBNK)]"), std::string::npos);
    EXPECT_NE(tree.find("Wave Data (SMPL) [CATEGORY]"), std::string::npos);
    EXPECT_NE(tree.find("SMP 252511 [WAVE DATA (SMPL)]"), std::string::npos);

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "info", fixture.string(), "--format", "paths"}), 0);
    const auto paths = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(paths.starts_with("source_path\tscope\tpath\tdisplay_name\tobject_type\tobject_key\n"));
    EXPECT_NE(paths.find("\tsbac\tpartition_00_New_Partition/New Volume/Sample Banks and Samples/B New SmpBank\tB New "
                         "SmpBank\tSBAC\tp0:sfs23\n"),
              std::string::npos);

    testing::internal::CaptureStdout();
    ASSERT_EQ(run_cli({"axklib", "info", fixture.string(), "--format", "json"}), 0);
    const auto json = nlohmann::json::parse(testing::internal::GetCapturedStdout());
    ASSERT_EQ(json.at("trees").size(), 1U);
    EXPECT_EQ(json.at("trees").front().at("source_path"), fixture.string());
    EXPECT_EQ(json.at("trees").front().at("roots").front().at("selector_path"), "partition_00_New_Partition");
    EXPECT_TRUE(json.at("load_errors").empty());
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
    const auto analysis = read(root / "commands" / "analysis.cpp");
    const auto info_begin = analysis.find("int run_info_request");
    ASSERT_NE(info_begin, std::string::npos);
    const auto info_handler = analysis.substr(info_begin);
    EXPECT_NE(info_handler.find("LocalOperationRuntime::create"), std::string::npos);
    EXPECT_EQ(info_handler.find("load_cli_paths(request.paths"), std::string::npos);
    const auto coverage_begin = analysis.find("int run_coverage_request");
    const auto info_projection_begin = analysis.find("info_node_output");
    ASSERT_NE(coverage_begin, std::string::npos);
    ASSERT_NE(info_projection_begin, std::string::npos);
    const auto coverage_handler = analysis.substr(coverage_begin, info_projection_begin - coverage_begin);
    EXPECT_NE(coverage_handler.find("LocalOperationRuntime::create"), std::string::npos);
    EXPECT_EQ(coverage_handler.find("load_cli_paths(request.paths"), std::string::npos);
    const auto relationships_handler_begin = analysis.find("int run_relationships_request");
    ASSERT_NE(relationships_handler_begin, std::string::npos);
    const auto relationships_handler =
        analysis.substr(relationships_handler_begin, coverage_begin - relationships_handler_begin);
    EXPECT_NE(relationships_handler.find("LocalOperationRuntime::create"), std::string::npos);
    EXPECT_EQ(relationships_handler.find("load_cli_paths(request.paths"), std::string::npos);
    const auto reports = read(root / "commands" / "reports.cpp");
    const std::array report_handlers{
        std::pair{"run_objects_request", "run_inventory_request"},
        std::pair{"run_inventory_request", "run_orphans_request"},
        std::pair{"run_orphans_request", "run_validate_request"},
        std::pair{"run_validate_request", "run_corpus_audit_request"},
        std::pair{"run_corpus_audit_request", "} // namespace axk::cli::commands"},
    };
    for (const auto &[name, next_name] : report_handlers) {
        const auto begin = reports.find(std::string{"int "} + name);
        const std::string_view next{next_name};
        auto next_marker = std::string{next};
        if (!next.starts_with('}'))
            next_marker.insert(0U, "int ");
        const auto end = reports.find(next_marker, begin);
        ASSERT_NE(begin, std::string::npos) << name;
        ASSERT_NE(end, std::string::npos) << name;
        const auto handler = reports.substr(begin, end - begin);
        EXPECT_NE(handler.find("LocalOperationRuntime::create"), std::string::npos) << name;
    }
    for (const std::string_view forbidden :
         {"load_cli_paths", "load_semantic_snapshot", "build_media_inventory", "build_relationship_graph",
          "validate_semantics", "decode_waveform", "write_report_", "allocation_mismatch_rows", "relationship_rows",
          "_legacy"}) {
        EXPECT_EQ(reports.find(forbidden), std::string::npos) << forbidden;
    }
    for (const auto &entry : std::filesystem::directory_iterator{root / "schema"}) {
        if (entry.path().extension() == ".hpp") {
            EXPECT_EQ(read(entry.path()).find("nlohmann"), std::string::npos) << entry.path();
        }
    }
}
