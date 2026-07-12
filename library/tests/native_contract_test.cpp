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

} // namespace

TEST(NativeContracts, HasCanonicalInventoryAndUniqueEntries) {
  const auto baseline = read_contract("baseline.json");

  EXPECT_EQ(baseline.at("schema_version"), "2.0");
  EXPECT_FALSE(baseline.contains("oracle"));

  const auto commands = baseline.at("commands").get<std::vector<std::string>>();
  const auto command_set = std::set<std::string>{commands.begin(), commands.end()};
  EXPECT_EQ(commands.size(), command_set.size());

  const auto profiles = baseline.at("writer_profiles").get<std::vector<std::string>>();
  const auto profile_set = std::set<std::string>{profiles.begin(), profiles.end()};
  EXPECT_EQ(profiles.size(), profile_set.size());
  EXPECT_TRUE(profile_set.contains("hds_geometry_v1"));
  EXPECT_TRUE(profile_set.contains("alteration_rename_v1"));
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

TEST(NativeContracts, SemanticContractsAreUniqueAndContainNoPrivateRegenerationData) {
  const auto document = read_contract("semantic-contracts.json");
  EXPECT_EQ(document.at("schema_version"), "2.0");
  EXPECT_FALSE(document.contains("oracle_commit"));
  std::set<std::string> identifiers;
  for (const auto &contract : document.at("contracts")) {
    EXPECT_TRUE(identifiers.insert(contract.at("id").get<std::string>()).second);
    EXPECT_FALSE(contract.contains("regeneration_command"));
    EXPECT_EQ(contract.at("input_sha256").get<std::string>().size(), 64U);
    EXPECT_EQ(contract.at("expected_semantic_sha256").get<std::string>().size(), 64U);
  }
}
