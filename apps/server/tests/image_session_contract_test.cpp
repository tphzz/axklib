#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/filesystem.hpp"
#include "axklib/application/image_session_contracts.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/server/contract.hpp"
#include "axklib/writer_internal.hpp"

namespace {

TEST(ImageSessionContract, SystemContextsRequireStorageRevisionAndAllStoredParts) {
    const axk::server::OpenApiValidator validator;
    nlohmann::json parts = nlohmann::json::array();
    for (int index = 0; index < 32; ++index) {
        const std::string port = index < 16 ? "A" : "B";
        const auto channel = index % 16 + 1;
        const auto label = port + (channel < 10 ? "0" : "") + std::to_string(channel);
        parts.push_back({{"partNumber", index + 1},
                         {"partLabel", label},
                         {"midi", {{"port", port}, {"channel", channel}, {"display", label}}},
                         {"programNumber", index + 1},
                         {"master", index == 31}});
    }
    nlohmann::json context{{"fileKind", "SYSTEM2"},
                           {"availability", "AVAILABLE"},
                           {"storageRevision", 0},
                           {"savedProgramMode", "MULTI"},
                           {"basicReceive", {{"port", "B"}, {"channel", 16}, {"display", "B16"}}},
                           {"omni", false},
                           {"programChangeEnabled", true},
                           {"parts", parts}};
    for (const auto revision : std::array{0, 1}) {
        context["storageRevision"] = revision;
        EXPECT_TRUE(validator.validate("SystemProgramContext", context)) << context.dump();
    }
    context["storageRevision"] = 2;
    EXPECT_FALSE(validator.validate("SystemProgramContext", context));
    context["storageRevision"] = 0;
    context["model"] = "A4000";
    EXPECT_FALSE(validator.validate("SystemProgramContext", context));
    context.erase("model");
    context["parts"].erase(context["parts"].begin() + 16, context["parts"].end());
    EXPECT_FALSE(validator.validate("SystemProgramContext", context));
    context["parts"] = parts;
    context.erase("storageRevision");
    EXPECT_FALSE(validator.validate("SystemProgramContext", context));
}

TEST(ImageSessionContract, AcceptsEverySerializedFloppyMarker) {
    const axk::server::OpenApiValidator validator;
    axk::app::ImageSessionSummary summary;
    summary.image_id = "image-test";
    summary.revision = 1U;
    summary.source = {.root_id = "workspace", .relative_path = "ordinary.ima", .kind = axk::app::ImageSourceKind::file};
    summary.format = "fat12_floppy";
    summary.floppy_set = axk::app::ImageFloppySetSummary{
        .status = axk::app::ImageFloppySetStatus::single,
        .set_label = "ORDINARY",
        .members = {{.index = 1U, .label = "ORDINARY", .marker = "ORDINARY"}},
        .next_required_index = {},
    };
    for (const auto marker : std::array{"NONE", "ORDINARY", "CONTINUATION", "FINAL", "INVALID"}) {
        SCOPED_TRACE(marker);
        summary.floppy_set->members.front().marker = marker;
        const auto serialized = axk::app::image_session_summary_json(summary);
        const auto wire = validator.wire_value("ImageSession", serialized);
        EXPECT_EQ(wire.at("floppySet").at("members").at(0).at("marker"), marker);
        EXPECT_TRUE(validator.validate("ImageSession", wire)) << wire.dump();
    }
    for (const auto marker : std::array{"ordinary", "UNKNOWN"}) {
        summary.floppy_set->members.front().marker = marker;
        EXPECT_FALSE(validator.validate("ImageSession", axk::app::image_session_summary_json(summary)));
    }
}

TEST(ImageSessionContract, RepresentsUnnumberedAndTwoDigitCatalogMembers) {
    const axk::server::OpenApiValidator validator;
    for (const auto index : std::array{0, 1, 32, 33, 99}) {
        const nlohmann::json member{{"index", index}, {"label", "DJ TSUYOSHI     "}, {"marker", "ORDINARY"}};
        EXPECT_TRUE(validator.validate("ImageFloppySetMember", member)) << member.dump();
    }
    for (const auto index : std::array{-1, 100}) {
        const nlohmann::json member{{"index", index}, {"label", "INVALID"}, {"marker", "ORDINARY"}};
        EXPECT_FALSE(validator.validate("ImageFloppySetMember", member));
    }
}

TEST(ImageSessionContract, KeepsCatalogOrdinalRangeSeparateFromAttachmentLimit) {
    const axk::server::OpenApiValidator validator;
    const nlohmann::json member{{"index", 99}, {"label", "SET           99"}, {"marker", "CONTINUATION"}};
    nlohmann::json set{{"status", "INCOMPLETE"},
                       {"setLabel", "SET"},
                       {"members", nlohmann::json::array({member})},
                       {"nextRequiredIndex", 100}};
    EXPECT_TRUE(validator.validate("ImageFloppySet", set));
    for (const auto next : std::array{0, 101}) {
        set["nextRequiredIndex"] = next;
        EXPECT_FALSE(validator.validate("ImageFloppySet", set));
    }
    set["nextRequiredIndex"] = nullptr;
    for (int index = 1; index < 33; ++index)
        set["members"].push_back(member);
    EXPECT_FALSE(validator.validate("ImageFloppySet", set));
}

class FloppyOpenContract : public ::testing::Test {
  protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() /
                     ("axklib-floppy-open-contract-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(directory_));
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    std::filesystem::path directory_;
};

TEST_F(FloppyOpenContract, OpensUnnumberedOrdinaryFloppyThroughTheSerializedSessionContract) {
    axk::detail::PreparedMediaImage image;
    const auto filename = axk::detail::yamaha_floppy_physical_filename("A3000.SYM", 1U);
    ASSERT_TRUE(filename) << filename.error().message;
    image.retained_files.push_back({*filename, {}});
    image.floppy_catalog = axk::YamahaFloppyCatalog{"DJ TSUYOSHI", {{1U, R"(\A3000.SYM)"}}, {R"(\OTHERS)"}};
    const auto bytes = axk::detail::build_fat12_image(image, {});
    ASSERT_TRUE(bytes) << bytes.error().message;
    {
        std::ofstream output{directory_ / "ordinary.ima", std::ios::binary};
        output.write(reinterpret_cast<const char *>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        ASSERT_TRUE(output);
    }
    auto sandbox = axk::app::Sandbox::create({{"fixture", "Fixture", directory_, false}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    axk::app::ImageSessionManager sessions{*sandbox};
    const auto opened = sessions.open({"fixture", "ordinary.ima"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    ASSERT_TRUE(opened->floppy_set);
    EXPECT_EQ(opened->floppy_set->status, axk::app::ImageFloppySetStatus::single);
    ASSERT_EQ(opened->floppy_set->members.size(), 1U);
    EXPECT_EQ(opened->floppy_set->members.front().marker, "ORDINARY");
    EXPECT_EQ(opened->floppy_set->members.front().index, 0U);
    const axk::server::OpenApiValidator validator;
    const auto wire = validator.wire_value("ImageSession", axk::app::image_session_summary_json(*opened));
    EXPECT_TRUE(validator.validate("ImageSession", wire)) << wire.dump();
}

} // namespace
