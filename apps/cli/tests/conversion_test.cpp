#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app.hpp"
#include "axklib/writer.hpp"

namespace {
class ConversionCli : public testing::Test {
  protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / "axk-conversion-cli-test";
    std::string output;

    void SetUp() override {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "source");
        std::vector<char> bytes(35U);
        std::ranges::copy(std::string_view{"LM8953"}, bytes.begin());
        bytes[22U] = static_cast<char>(0xc9);
        bytes[23U] = bytes[24U] = 2;
        bytes[32U] = 0x7f;
        bytes[33U] = static_cast<char>(0xf0);
        bytes[34U] = static_cast<char>(0x80);
        {
            std::ofstream wave{root / "source" / "TONE.W01", std::ios::binary};
            wave.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Disk", {{"Samples", {}, {}, {}, {}}}}}};
        ASSERT_TRUE(axk::write_hds_image(manifest, root / "target.hds", false));
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    int run(std::vector<std::string> extra = {}, const std::string &confirmation = {}) {
        std::vector<std::string> arguments{"axklib",        "convert",
                                           "--profile",     "yamaha.tx16w-to-a-series",
                                           "--source",      (root / "source").string(),
                                           "--destination", (root / "target.hds").string(),
                                           "--selection",   R"({"partitionIndex":0,"volumeName":"Samples"})",
                                           "--options",     R"({"mode":"WAVE_DATA_ONLY"})"};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        std::vector<char *> argv;
        for (auto &value : arguments)
            argv.push_back(value.data());
        std::istringstream input{confirmation};
        const auto original = std::cin.rdbuf(input.rdbuf());
        testing::internal::CaptureStdout();
        testing::internal::CaptureStderr();
        const auto status = axk::cli::run(static_cast<int>(argv.size()), argv.data());
        output = testing::internal::GetCapturedStdout();
        static_cast<void>(testing::internal::GetCapturedStderr());
        std::cin.rdbuf(original);
        return status;
    }
    static std::string bytes(const std::filesystem::path &path) {
        std::ifstream file{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{file}, {}};
    }
};

TEST_F(ConversionCli, ReviewsAndPublishesOnlyAfterExactAcknowledgement) {
    const auto before = bytes(root / "target.hds");
    ASSERT_EQ(run(), 0);
    const auto report = nlohmann::json::parse(output);
    EXPECT_TRUE(report.at("executable"));
    EXPECT_FALSE(std::filesystem::exists(root / "output.hds"));
    std::string confirmation = "APPLY";
    for (const auto &issue : report.at("issues"))
        if (issue.at("impact") == "LOSS" || issue.at("impact") == "UNCERTAINTY")
            confirmation += ' ' + issue.at("id").get<std::string>();
    const std::vector<std::string> publish{"--output", (root / "output.hds").string()};
    EXPECT_NE(run(publish, "APPLY\n"), 0);
    EXPECT_FALSE(std::filesystem::exists(root / "output.hds"));
    EXPECT_NE(run({"--strict"}), 0);
    ASSERT_EQ(run(publish, confirmation + '\n'), 0);
    EXPECT_EQ(bytes(root / "target.hds"), before);
    const auto after = bytes(root / "output.hds");
    EXPECT_NE(after, before);
    EXPECT_NE(run(publish, confirmation + '\n'), 0);
    EXPECT_EQ(bytes(root / "output.hds"), after);
}
} // namespace
