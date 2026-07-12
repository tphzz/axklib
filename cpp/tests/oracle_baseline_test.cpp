#include <cstdint>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

nlohmann::json read_baseline() {
  const std::string path = std::string{AXK_SOURCE_ROOT} + "/oracle/baseline.json";
  std::ifstream input{path};
  if (!input) {
    throw std::runtime_error{"cannot open oracle baseline: " + path};
  }
  return nlohmann::json::parse(input);
}

}  // namespace

TEST(OracleBaseline, HasCanonicalIdentityAndUniqueContracts) {
  const auto baseline = read_baseline();

  EXPECT_EQ(baseline.at("schema_version"), "1.0");
  EXPECT_EQ(baseline.at("oracle").at("implementation"), "python");
  EXPECT_EQ(baseline.at("oracle").at("git_commit").get<std::string>().size(), 40U);
  EXPECT_GT(baseline.at("oracle").at("test_count").get<int>(), 0);

  const auto commands = baseline.at("commands").get<std::vector<std::string>>();
  const auto command_set = std::set<std::string>{commands.begin(), commands.end()};
  EXPECT_EQ(commands.size(), command_set.size());

  const auto profiles = baseline.at("writer_profiles").get<std::vector<std::string>>();
  const auto profile_set = std::set<std::string>{profiles.begin(), profiles.end()};
  EXPECT_EQ(profiles.size(), profile_set.size());
  EXPECT_TRUE(profile_set.contains("hds_geometry_v1"));
  EXPECT_TRUE(profile_set.contains("alteration_rename_v1"));
}

TEST(OracleBaseline, UsesOnlyRelativeFixturePathsAndSha256Digests) {
  const auto baseline = read_baseline();
  for (const auto& fixture : baseline.at("fixtures")) {
    const auto path = fixture.at("path").get<std::string>();
    const auto digest = fixture.at("sha256").get<std::string>();
    EXPECT_FALSE(path.empty());
    EXPECT_NE(path.front(), '/');
    EXPECT_EQ(digest.size(), 64U);
    EXPECT_GT(fixture.at("size_bytes").get<std::uint64_t>(), 0U);
  }
}
