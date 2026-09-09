#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "filesystem_inputs.hpp"

namespace {
class InputReader final : public axk::RandomAccessReader {
  public:
    std::uint64_t length{3U * 1024U * 1024U + 1U};
    mutable std::size_t reads{};
    axk::CancellationSource *cancel_after_read{};
    std::uint64_t size() const noexcept override { return length; }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        EXPECT_LE(destination.size(), 1024U * 1024U);
        EXPECT_LE(offset + destination.size(), length);
        std::ranges::fill(destination, std::byte{0x21});
        ++reads;
        if (cancel_after_read)
            cancel_after_read->cancel();
        return {};
    }
};

TEST(FilesystemInputs, FingerprintsInBoundedChunksAndChecksIdentityAroundReading) {
    auto reader = std::make_shared<InputReader>();
    std::size_t validations{};
    axk::app::filesystem_inputs::OpenedInput input{reader, "source-revision", {}, [&]() -> axk::app::Result<void> {
                                                       ++validations;
                                                       return {};
                                                   }};
    const auto snapshot = input.snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(reader->reads, 4U);
    EXPECT_EQ(validations, 2U);
    EXPECT_EQ(snapshot->at("sizeBytes"), reader->length);
    EXPECT_EQ(snapshot->at("sha256").get<std::string>().size(), 64U);
    EXPECT_TRUE(input.verify(*snapshot));
    auto changed = *snapshot;
    changed["sha256"] = std::string(64U, '0');
    const auto rejected = input.verify(changed);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "filesystem_input_changed");
}

TEST(FilesystemInputs, CancelsDuringHashingAndBeforeEmptyFileInspection) {
    axk::CancellationSource cancellation;
    auto reader = std::make_shared<InputReader>();
    reader->cancel_after_read = &cancellation;
    axk::app::filesystem_inputs::OpenedInput input{
        reader, "revision", {}, []() -> axk::app::Result<void> { return {}; }};
    const auto result = input.snapshot(cancellation.token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "operation_cancelled");
    EXPECT_EQ(reader->reads, 1U);
    reader->length = 0U;
    EXPECT_FALSE(input.snapshot(cancellation.token()));
}

TEST(FilesystemInputs, RejectsOversizedInputsBeforeReadingAndMalformedSnapshots) {
    auto reader = std::make_shared<InputReader>();
    axk::app::filesystem_inputs::OpenedInput input{
        reader, "revision", {}, []() -> axk::app::Result<void> { return {}; }};
    reader->length = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
    const auto too_large = input.snapshot();
    ASSERT_FALSE(too_large);
    EXPECT_EQ(too_large.error().code, "filesystem_input_too_large");
    EXPECT_EQ(reader->reads, 0U);
    reader->length = 0U;
    const nlohmann::json valid{{"revision", "revision"}, {"sha256", std::string(64U, 'a')}, {"sizeBytes", 0U}};
    std::vector<nlohmann::json> malformed{nullptr, nlohmann::json::object()};
    for (const auto &field : {"revision", "sha256", "sizeBytes"}) {
        auto missing = valid;
        missing.erase(field);
        malformed.push_back(std::move(missing));
    }
    for (const auto &size : {nlohmann::json(-1), nlohmann::json(0.5), nlohmann::json("0")}) {
        auto invalid = valid;
        invalid["sizeBytes"] = size;
        malformed.push_back(std::move(invalid));
    }
    auto invalid_hash = valid;
    invalid_hash["sha256"] = std::string(64U, 'X');
    malformed.push_back(invalid_hash);
    for (const auto &snapshot : malformed) {
        const auto result = input.verify(snapshot);
        ASSERT_FALSE(result) << snapshot.dump();
        EXPECT_EQ(result.error().code, "invalid_request");
    }
    EXPECT_EQ(reader->reads, 0U);
}
} // namespace
