#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/io.hpp"
#include "axklib/utf8.hpp"

namespace {

class TemporaryFile {
 public:
  explicit TemporaryFile(std::string_view stem) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            (std::string{stem} + "-" + std::to_string(suffix) + ".bin");
  }

  ~TemporaryFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile& operator=(const TemporaryFile&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST(Cancellation, SharedTokenObservesCancellation) {
  axk::CancellationSource source;
  const auto first = source.token();
  const auto second = source.token();

  EXPECT_FALSE(first.is_cancelled());
  EXPECT_TRUE(first.check());
  source.cancel();
  EXPECT_TRUE(first.is_cancelled());
  EXPECT_TRUE(second.is_cancelled());
  ASSERT_FALSE(second.check());
  EXPECT_EQ(second.check().error().code, axk::ErrorCode::operation_cancelled);
}

TEST(MemoryReader, ReadsExactlyAndRejectsShortOrOverflowingRanges) {
  axk::MemoryReader reader{
      {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}}};
  std::array<std::byte, 2> output{};

  ASSERT_TRUE(reader.read_exact_at(1, output));
  EXPECT_EQ(output[0], std::byte{0x02});
  EXPECT_EQ(output[1], std::byte{0x03});
  EXPECT_EQ(reader.size(), 4U);

  const auto short_read = reader.read_exact_at(3, output);
  ASSERT_FALSE(short_read);
  EXPECT_EQ(short_read.error().code, axk::ErrorCode::io_short_read);

  const auto overflow = reader.read_exact_at(std::numeric_limits<std::uint64_t>::max(), output);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().code, axk::ErrorCode::integer_overflow);
}

TEST(FileReader, ReportsMissingFilesWithSourceContext) {
  const auto missing = std::filesystem::temp_directory_path() / "axklib-missing-reader-file";
  std::error_code ignored;
  std::filesystem::remove(missing, ignored);
  const auto result = axk::FileReader::open(missing);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::io_open_failed);
  ASSERT_TRUE(result.error().context.source_path.has_value());
  EXPECT_EQ(*result.error().context.source_path, axk::text::path_to_utf8(missing));
}

TEST(FileReader, ReadsBeyondTwoGiBWithoutAllocatingTheWholeFile) {
  TemporaryFile file{"axklib-sparse-reader"};
  constexpr std::uint64_t offset = (std::uint64_t{2} * 1024U * 1024U * 1024U) + 17U;
  {
    std::ofstream output{file.path(), std::ios::binary};
    ASSERT_TRUE(output);
    output.seekp(static_cast<std::streamoff>(offset));
    output.put(static_cast<char>(0x5a));
  }

  const auto reader = axk::FileReader::open(file.path());
  ASSERT_TRUE(reader);
  EXPECT_EQ((*reader)->size(), offset + 1U);
  std::array<std::byte, 1> byte{};
  ASSERT_TRUE((*reader)->read_exact_at(offset, byte));
  EXPECT_EQ(byte[0], std::byte{0x5a});
  EXPECT_FALSE((*reader)->read_exact_at(offset + 1U, byte));

  axk::CancellationSource cancellation;
  cancellation.cancel();
  const auto cancelled = (*reader)->read_exact_at(offset, byte, cancellation.token());
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
}
