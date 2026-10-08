#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/file_operations.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/path_reservations.hpp"
#include "axklib/application/validation_operations.hpp"
#include "axklib/audio.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/sample_storage.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

struct BitmapReportingCase {
    std::optional<axk::MediaImageFormat> media_format;
    axk::SampleStorageFormat storage_format;
};

class ProgramBitmapReporting : public testing::TestWithParam<BitmapReportingCase> {
  protected:
    std::filesystem::path root;
    std::unique_ptr<axk::app::Sandbox> sandbox;
    const std::string image_name{"source.img"};
    std::map<std::string, std::uint64_t> object_offsets;

    void SetUp() override {
        std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
        std::ranges::replace(name, '/', '-');
        for (std::size_t attempt = 0; attempt < 1024U; ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() /
                             ("axklib-bitmap-report-" + name + '-' + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root = std::move(candidate);
                break;
            }
            ASSERT_FALSE(error) << error.message();
        }
        ASSERT_FALSE(root.empty());
        std::filesystem::create_directory(root / "reports");
        axk::Waveform waveform;
        waveform.format = {1U, 2U, 44'100U};
        waveform.frame_count = 2U;
        waveform.pcm = {std::byte{}, std::byte{}, std::byte{1}, std::byte{}};
        const auto audio = root / "tone.wav";
        ASSERT_TRUE(axk::write_wav_atomic(audio, waveform));
        axk::VolumeSpec volume;
        volume.name = "importtest";
        volume.waveforms.push_back({"wave", "Wave", audio, 60U, {}});
        for (const auto *sample_name : {"909 HH", "DX Member"}) {
            axk::SampleSpec sample;
            sample.name = sample_name;
            sample.waveform_id = "wave";
            sample.storage_format = GetParam().storage_format;
            volume.samples.push_back(std::move(sample));
        }
        volume.sample_banks.push_back({"DX PER", {"DX Member"}, {}, GetParam().storage_format});
        volume.programs.push_back({8U, "909 HH", {{"SBNK", "909 HH", {}}}});
        volume.programs.push_back({21U, "DX METAL", {{"SBAC", "DX PER", {}}}});
        const auto path = root / image_name;
        if (GetParam().media_format) {
            axk::MediaBuildManifest manifest;
            manifest.schema_version = "1.0";
            manifest.format = *GetParam().media_format;
            manifest.authored_volume = volume;
            manifest.volume_name = volume.name;
            const auto written = axk::write_media_image(manifest, path);
            ASSERT_TRUE(written) << written.error().message;
        } else {
            const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
            const auto written = axk::write_hds_image(manifest, path);
            ASSERT_TRUE(written) << written.error().message;
        }
        std::vector<std::pair<std::uint64_t, std::uint32_t>> patches;
        {
            const auto media = axk::open_media(path);
            ASSERT_TRUE(media) << media.error().message;
            const auto objects = media->objects();
            ASSERT_TRUE(objects) << objects.error().message;
            for (const auto &object : *objects) {
                if (object.decoded.header.type != axk::ObjectType::sbac &&
                    object.decoded.header.type != axk::ObjectType::sbnk)
                    continue;
                auto absolute = object.data_offset;
                if (const auto *sfs = std::get_if<axk::Container>(&media->storage())) {
                    const auto &partition = sfs->partitions().front();
                    const auto record = std::ranges::find_if(partition.records, [&](const auto &entry) {
                        return entry.object_name == object.decoded.header.name &&
                               entry.object_type == object.decoded.header.raw_type;
                    });
                    ASSERT_NE(record, partition.records.end());
                    ASSERT_EQ(record->extents.size(), 1U);
                    absolute = (static_cast<std::uint64_t>(partition.start_sector) +
                                static_cast<std::uint64_t>(record->extents.front().cluster_offset) *
                                    partition.sectors_per_cluster) *
                               sfs->superblock().sector_size_bytes;
                }
                object_offsets.emplace(object.decoded.header.name, absolute);
                if (object.decoded.header.type == axk::ObjectType::sbac)
                    patches.emplace_back(absolute + 0x90U, 1U << 7U);
                if (object.decoded.header.type == axk::ObjectType::sbnk && object.decoded.header.name == "909 HH")
                    patches.emplace_back(absolute + 0xc0U, 1U << 4U);
            }
        }
        ASSERT_EQ(patches.size(), 2U);
        {
            std::fstream output{path, std::ios::binary | std::ios::in | std::ios::out};
            ASSERT_TRUE(output);
            for (const auto &[offset, word] : patches) {
                const std::array bytes{static_cast<char>(word >> 24U), static_cast<char>(word >> 16U),
                                       static_cast<char>(word >> 8U), static_cast<char>(word)};
                output.seekp(static_cast<std::streamoff>(offset));
                output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                ASSERT_TRUE(output);
            }
        }
        auto created = axk::app::Sandbox::create({{"workspace", "Workspace", root, true}});
        ASSERT_TRUE(created) << created.error().message;
        sandbox = std::make_unique<axk::app::Sandbox>(std::move(*created));
    }

    void TearDown() override {
        sandbox.reset();
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
        EXPECT_FALSE(error) << error.message();
    }

    axk::app::OperationContext context() const {
        return {
            .owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}};
    }

    nlohmann::json request(std::string destination) const {
        return {{"sources", {{{"rootId", "workspace"}, {"relativePath", image_name}}}},
                {"destination", {{"rootId", "workspace"}, {"relativePath", std::move(destination)}}}};
    }

    void remove_wave_link(std::string_view name) {
        const auto offset = object_offsets.at(std::string{name});
        std::fstream output{root / image_name, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(output);
        constexpr std::array<char, 16> missing{'A', 'b', 's', 'e', 'n', 't'};
        constexpr std::array<char, 4> no_cache{};
        output.seekp(static_cast<std::streamoff>(offset + 0x78U));
        output.write(missing.data(), static_cast<std::streamsize>(missing.size()));
        output.seekp(static_cast<std::streamoff>(offset + 0xa0U));
        output.write(no_cache.data(), static_cast<std::streamsize>(no_cache.size()));
        ASSERT_TRUE(output);
    }
};

TEST_P(ProgramBitmapReporting, ReportsEachObjectMismatchOnceAndStrictPolicyFails) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_validation_operations(registry, *sandbox));
    for (const auto *policy : {"normal", "strict", "salvage-aware"}) {
        auto input = request(std::string{"reports/"} + policy);
        input["policy"] = policy;
        const auto result = registry.invoke("report.validate", input, context());
        ASSERT_TRUE(result) << result.error().message;
        EXPECT_EQ(result->at("failed"), std::string_view{policy} == "strict");
        std::ifstream issues_file{root / "reports" / policy / "validation_issues.json"};
        const auto issues = nlohmann::json::parse(issues_file);
        ASSERT_EQ(issues.size(), 2U) << issues.dump(2);
        for (const auto *code : {"REL_SBAC_PROGRAM_BITMAP_MISMATCH", "REL_SBNK_PROGRAM_BITMAP_MISMATCH"}) {
            const auto found =
                std::ranges::find_if(issues, [&](const auto &issue) { return issue.at("code") == code; });
            ASSERT_NE(found, issues.end());
            EXPECT_EQ(found->at("severity"), "warning");
            EXPECT_FALSE(found->at("object_key").template get<std::string>().empty());
            const auto message = found->at("message").template get<std::string>();
            const bool bank = std::string_view{code} == "REL_SBAC_PROGRAM_BITMAP_MISMATCH";
            EXPECT_NE(message.find(bank ? "DX PER" : "909 HH"), std::string::npos);
            EXPECT_NE(message.find(bank ? "008" : "005"), std::string::npos);
            EXPECT_NE(message.find(bank ? "021" : "008"), std::string::npos);
        }
    }
}

TEST_P(ProgramBitmapReporting, WritesGenericCrossCheckRowsAndSchemaForBothObjectTypes) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox));
    const auto result = registry.invoke("report.relationships", request("reports/relationships"), context());
    ASSERT_TRUE(result) << result.error().message;
    const auto directory = root / "reports" / "relationships";
    EXPECT_TRUE(std::filesystem::is_regular_file(directory / "_schemas/current_program_bitmap_crosscheck.schema.json"));
    EXPECT_FALSE(std::filesystem::exists(directory / "current_sbnk_program_bitmap_crosscheck.json"));
    std::ifstream rows_file{directory / "current_program_bitmap_crosscheck.json"};
    const auto rows = nlohmann::json::parse(rows_file);
    ASSERT_EQ(rows.size(), 3U);
    const auto bank = std::ranges::find_if(rows, [](const auto &row) { return row.at("object_type") == "SBAC"; });
    ASSERT_NE(bank, rows.end());
    EXPECT_EQ(bank->at("object_name"), "DX PER");
    EXPECT_EQ(bank->at("program_bitmap_offset"), 0x90U);
    EXPECT_EQ(bank->at("linked_programs_001_032_bitmap"), 128U);
    EXPECT_EQ(bank->at("bitmap_programs"), "008");
    EXPECT_EQ(bank->at("direct_prog_assignment_programs"), "021");
    EXPECT_EQ(bank->at("match_status"), "mismatch");
    EXPECT_FALSE(bank->contains("sbnk_object_key"));
    const auto sample = std::ranges::find_if(rows, [](const auto &row) { return row.at("object_name") == "909 HH"; });
    ASSERT_NE(sample, rows.end());
    EXPECT_EQ(sample->at("object_type"), "SBNK");
    EXPECT_EQ(sample->at("program_bitmap_offset"), 0xc0U);
    EXPECT_EQ(sample->at("bitmap_programs"), "005");
    EXPECT_EQ(sample->at("direct_prog_assignment_programs"), "008");
}

TEST_P(ProgramBitmapReporting, CorpusIncludesMediaWarningsAndHonorsStrictPolicyWithoutWaveSmoke) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox));
    for (const auto *policy : {"normal", "strict", "salvage-aware"}) {
        auto input = request(std::string{"reports/corpus-"} + policy);
        input["policy"] = policy;
        input["skipWaveSmoke"] = true;
        const auto result = registry.invoke("corpus.audit", input, context());
        ASSERT_TRUE(result) << result.error().message;
        EXPECT_EQ(result->at("validationIssueCount"), 2U);
        EXPECT_EQ(result->at("validationFailed"), std::string_view{policy} == "strict");
        EXPECT_EQ(result->at("waveSmokeDecoded"), 0U);
        std::ifstream file{root / "reports" / (std::string{"corpus-"} + policy) / "validation_issues.csv"};
        ASSERT_TRUE(file);
        const std::string issues{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        for (const auto *code : {"REL_SBAC_PROGRAM_BITMAP_MISMATCH", "REL_SBNK_PROGRAM_BITMAP_MISMATCH"}) {
            const auto first = issues.find(code);
            ASSERT_NE(first, std::string::npos);
            EXPECT_EQ(issues.find(code, first + 1U), std::string::npos);
        }
    }
}

TEST_P(ProgramBitmapReporting, ActiveMissingWaveDataConvergesAcrossReportCorpusAndSession) {
    remove_wave_link("909 HH");
    remove_wave_link("DX Member");
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_validation_operations(registry, *sandbox));
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox));
    const auto standard = registry.invoke("report.validate", request("reports/missing"), context());
    ASSERT_TRUE(standard) << standard.error().message;
    EXPECT_TRUE(standard->at("failed").get<bool>());
    std::ifstream standard_file{root / "reports" / "missing" / "validation_issues.json"};
    const auto issues = nlohmann::json::parse(standard_file);
    ASSERT_EQ(issues.size(), 3U) << issues.dump(2);
    const auto active = std::ranges::find_if(
        issues, [](const auto &issue) { return issue.at("code") == "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING"; });
    ASSERT_NE(active, issues.end());
    EXPECT_EQ(active->at("severity"), "error");
    EXPECT_TRUE(active->at("message").template get<std::string>().starts_with(
        "2 Sample-to-Wave-Data link(s) across 2 Sample(s)"));
    for (const auto *policy : {"normal", "strict", "salvage-aware"}) {
        auto input = request(std::string{"reports/missing-corpus-"} + policy);
        input["policy"] = policy;
        input["skipWaveSmoke"] = true;
        const auto result = registry.invoke("corpus.audit", input, context());
        ASSERT_TRUE(result) << result.error().message;
        EXPECT_EQ(result->at("validationIssueCount"), issues.size());
        EXPECT_TRUE(result->at("validationFailed").get<bool>());
    }
    axk::app::PathReservationCoordinator reservations;
    axk::app::ImageSessionManager sessions{
        *sandbox, 4U, 100U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", image_name}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto session_issues = sessions.validation_issues(opened->image_id, "owner", 100U);
    ASSERT_TRUE(session_issues);
    EXPECT_EQ(session_issues->items.size(), issues.size());
    for (const auto &issue : session_issues->items) {
        EXPECT_TRUE(issue.object_id);
        const auto found = std::ranges::find_if(issues, [&](const auto &row) { return row.at("code") == issue.code; });
        ASSERT_NE(found, issues.end());
        EXPECT_EQ(found->at("message").template get<std::string>(), issue.message);
        EXPECT_EQ(found->at("severity").template get<std::string>(), issue.severity);
    }
}

TEST_P(ProgramBitmapReporting, ExposesObjectBoundMismatchWarningsWhenOpeningEachContainer) {
    axk::app::PathReservationCoordinator reservations;
    axk::app::ImageSessionManager sessions{
        *sandbox, 4U, 100U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", image_name}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    const auto issues = sessions.validation_issues(opened->image_id, "owner", 100U);
    ASSERT_TRUE(issues) << issues.error().message;
    ASSERT_EQ(issues->items.size(), 2U);
    for (const auto &issue : issues->items) {
        EXPECT_EQ(issue.severity, "warning");
        EXPECT_TRUE(issue.object_id);
        EXPECT_TRUE(issue.code == "REL_SBAC_PROGRAM_BITMAP_MISMATCH" ||
                    issue.code == "REL_SBNK_PROGRAM_BITMAP_MISMATCH");
    }
    if (!GetParam().media_format) {
        const auto admitted = sessions.begin_mutation(opened->image_id, "owner", opened->revision);
        EXPECT_TRUE(admitted) << admitted.error().message;
        if (admitted)
            sessions.abort_mutation(opened->image_id, "owner", opened->revision);
    }
}

constexpr std::array cases{
    BitmapReportingCase{std::nullopt, axk::SampleStorageFormat::a3000_188},
    BitmapReportingCase{std::nullopt, axk::SampleStorageFormat::a4000_a5000_224},
    BitmapReportingCase{axk::MediaImageFormat::fat12_floppy, axk::SampleStorageFormat::a3000_188},
    BitmapReportingCase{axk::MediaImageFormat::fat12_floppy, axk::SampleStorageFormat::a4000_a5000_224},
    BitmapReportingCase{axk::MediaImageFormat::iso9660, axk::SampleStorageFormat::a3000_188},
    BitmapReportingCase{axk::MediaImageFormat::iso9660, axk::SampleStorageFormat::a4000_a5000_224},
};
INSTANTIATE_TEST_SUITE_P(AllMediaAndBankFormats, ProgramBitmapReporting, testing::ValuesIn(cases));

} // namespace
