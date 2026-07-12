#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "app.hpp"
#include "content_id.hpp"

namespace detail = axk::cli::detail;

TEST(ContentId, MatchesPublishedSha1VectorsAndStablePooledName) {
  const std::vector<std::byte> empty;
  EXPECT_EQ(detail::sha1_content_id(empty).digest_hex,
            "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  EXPECT_EQ(detail::sha1_content_id(bytes).digest_hex,
            "a9993e364706816aba3e25717850c26c9cd0d89d");

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

  std::array repeated{const_cast<char *>("axklib"), const_cast<char *>("info"),
                      const_cast<char *>("--format"), const_cast<char *>("json"),
                      const_cast<char *>("--format"), const_cast<char *>("tree"),
                      const_cast<char *>("missing.hds")};
  EXPECT_NE(axk::cli::run(static_cast<int>(repeated.size()), repeated.data()), 0);
}

TEST(Cli11Adapter, ExposesMaintainedCommandInventoryAndHidesLegacyAliases) {
  std::array help{const_cast<char *>("axklib"), const_cast<char *>("--help")};
  testing::internal::CaptureStdout();
  EXPECT_EQ(axk::cli::run(static_cast<int>(help.size()), help.data()), 0);
  const auto output = testing::internal::GetCapturedStdout();
  for (const auto command : {"info", "inventory", "validate", "relationships", "coverage",
                             "create", "alter", "orphans", "objects", "corpus", "extract"})
    EXPECT_NE(output.find(command), std::string::npos) << command;
  EXPECT_EQ(output.find("extract-wav"), std::string::npos);
  EXPECT_EQ(output.find("create-hds"), std::string::npos);

  const auto fixture = std::filesystem::path{AXK_SOURCE_ROOT} / "cpp/tests/fixtures/cli/help.txt";
  std::ifstream stream{fixture, std::ios::binary};
  ASSERT_TRUE(stream);
  const std::string expected{std::istreambuf_iterator<char>{stream}, {}};
  EXPECT_EQ(output, expected);
}

TEST(CliArchitecture, KeepsCli11AndJsonAtTheirOwnedBoundaries) {
  const auto root = std::filesystem::path{AXK_SOURCE_ROOT} / "cpp/cli";
  const auto read = [](const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{stream}, {}};
  };
  const auto app = read(root / "app.cpp");
  EXPECT_LE(static_cast<std::size_t>(std::ranges::count(app, '\n')), 400U);
  EXPECT_NE(app.find("#include <CLI/CLI.hpp>"), std::string::npos);
  EXPECT_EQ(app.find("nlohmann"), std::string::npos);
  EXPECT_EQ(app.find("sha1"), std::string::npos);

  for (const auto& entry : std::filesystem::directory_iterator{root / "commands"}) {
    if (entry.path().extension() != ".cpp")
      continue;
    const auto source = read(entry.path());
    EXPECT_EQ(source.find("CLI/CLI.hpp"), std::string::npos) << entry.path();
    EXPECT_EQ(source.find("\"schema_version\""), std::string::npos) << entry.path();
  }
  for (const auto& entry : std::filesystem::directory_iterator{root / "schema"}) {
    if (entry.path().extension() == ".hpp") {
      EXPECT_EQ(read(entry.path()).find("nlohmann"), std::string::npos) << entry.path();
    }
  }
}
