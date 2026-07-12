#include <array>
#include <string>

#include <gtest/gtest.h>

#include "app.hpp"

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
}
