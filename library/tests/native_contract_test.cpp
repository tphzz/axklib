#include <cstdint>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

nlohmann::json read_contract(const std::string &filename) {
    const std::string path = std::string{AXK_SOURCE_ROOT} + "/library/tests/contracts/" + filename;
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error{"cannot open native contract: " + path};
    }
    return nlohmann::json::parse(input);
}

nlohmann::json read_repository_json(const std::string &filename) {
    const std::string path = std::string{AXK_SOURCE_ROOT} + '/' + filename;
    std::ifstream input{path};
    if (!input)
        throw std::runtime_error{"cannot open repository JSON: " + path};
    return nlohmann::json::parse(input);
}

} // namespace

TEST(NativeContracts, HasCanonicalInventoryAndUniqueEntries) {
    const auto baseline = read_contract("baseline.json");

    EXPECT_EQ(baseline.at("schema_version"), "2.0");

    const auto commands = baseline.at("commands").get<std::vector<std::string>>();
    const auto command_set = std::set<std::string>{commands.begin(), commands.end()};
    EXPECT_EQ(commands.size(), command_set.size());

    const auto profiles = baseline.at("writer_profiles").get<std::vector<std::string>>();
    const auto profile_set = std::set<std::string>{profiles.begin(), profiles.end()};
    EXPECT_EQ(profiles.size(), profile_set.size());
    EXPECT_TRUE(profile_set.contains("hds_geometry_v1"));
    EXPECT_TRUE(profile_set.contains("alteration_rename_v1"));
    EXPECT_TRUE(profile_set.contains("fat12_yamaha_1440k_v1"));
    EXPECT_TRUE(profile_set.contains("iso9660_yamaha_catalog_v1"));
}

TEST(NativeContracts, PinsTheBuildContractAndRetainedMediaCommands) {
    const auto baseline = read_contract("baseline.json");
    const auto build = baseline.at("build_contract");
    EXPECT_EQ(build.at("cmake_minimum"), "3.28");
    EXPECT_EQ(build.at("public_cpp_standard"), 17);
    EXPECT_EQ(build.at("implementation_cpp_standard"), 23);
    EXPECT_TRUE(build.at("warnings_as_errors").get<bool>());
    const auto quality_commands = build.at("quality_commands").get<std::vector<std::string>>();
    const std::set<std::string> quality_command_set{quality_commands.begin(), quality_commands.end()};
    EXPECT_EQ(quality_commands.size(), quality_command_set.size());

    const auto vcpkg = read_repository_json("vcpkg.json");
    EXPECT_EQ(build.at("vcpkg_builtin_baseline"), vcpkg.at("builtin-baseline"));

    const auto commands = baseline.at("commands").get<std::vector<std::string>>();
    const std::set<std::string> command_set{commands.begin(), commands.end()};
    EXPECT_TRUE(command_set.contains("create hds"));
    EXPECT_TRUE(command_set.contains("create floppy"));
    EXPECT_TRUE(command_set.contains("create iso"));
    EXPECT_TRUE(command_set.contains("create manifest"));
    EXPECT_TRUE(command_set.contains("alter manifest"));
    EXPECT_TRUE(command_set.contains("package export"));
    EXPECT_TRUE(command_set.contains("package plan-import"));
    EXPECT_TRUE(command_set.contains("package import"));
}

TEST(NativeContracts, UsesOnlyRelativeFixturePathsAndSha256Digests) {
    const auto baseline = read_contract("baseline.json");
    for (const auto &fixture : baseline.at("fixtures")) {
        const auto path = fixture.at("path").get<std::string>();
        const auto digest = fixture.at("sha256").get<std::string>();
        EXPECT_FALSE(path.empty());
        EXPECT_NE(path.front(), '/');
        EXPECT_EQ(digest.size(), 64U);
        EXPECT_GT(fixture.at("size_bytes").get<std::uint64_t>(), 0U);
    }
}

TEST(NativeContracts, SemanticContractsAreUniqueAndContainNoRegenerationData) {
    const auto document = read_contract("semantic-contracts.json");
    EXPECT_EQ(document.at("schema_version"), "2.0");
    std::set<std::string> identifiers;
    for (const auto &contract : document.at("contracts")) {
        EXPECT_TRUE(identifiers.insert(contract.at("id").get<std::string>()).second);
        EXPECT_FALSE(contract.contains("regeneration_command"));
        EXPECT_EQ(contract.at("input_sha256").get<std::string>().size(), 64U);
        EXPECT_EQ(contract.at("expected_semantic_sha256").get<std::string>().size(), 64U);
    }
}
