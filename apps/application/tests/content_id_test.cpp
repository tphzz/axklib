#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "content_id.hpp"

#include "axklib/audio.hpp"

TEST(ContentId, MatchesPublishedSha1VectorsAndStablePooledName) {
    const std::vector<std::byte> empty;
    EXPECT_EQ(axk::app::sha1_content_id(empty).digest_hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    const std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    EXPECT_EQ(axk::app::sha1_content_id(bytes).digest_hex, "a9993e364706816aba3e25717850c26c9cd0d89d");

    axk::Waveform waveform;
    waveform.format = {.channels = 1U, .sample_width_bytes = 1U, .sample_rate = 44'100U};
    waveform.frame_count = bytes.size();
    waveform.pcm.assign(bytes.begin(), bytes.end());
    const auto wav = axk::wav_bytes(waveform);
    ASSERT_TRUE(wav);
    const auto wav_id = axk::app::sha1_content_id(*wav);
    axk::app::PooledPathAllocator paths;
    const auto pooled =
        paths.allocate("file", "physical", "Sample", axk::audio_internal::WavSource::from_physical(waveform));
    ASSERT_TRUE(pooled);
    EXPECT_EQ(pooled->filename(), "Sample__" + wav_id.digest_hex.substr(0U, 12U) + ".wav");
}

TEST(ContentId, ReusesEqualContentAndRejectsInjectedShortPrefixCollision) {
    const auto fake = [](const axk::audio_internal::WavSource &source) -> axk::Result<axk::app::ContentId> {
        const auto tail = source.physical->pcm.front() == std::byte{1} ? std::string(28U, '0') : std::string(28U, '1');
        return axk::app::ContentId{"sha1", "aaaaaaaaaaaa" + tail};
    };
    axk::app::PooledPathAllocator paths{fake};
    axk::Waveform first;
    first.format = {.channels = 1U, .sample_width_bytes = 1U, .sample_rate = 44'100U};
    first.frame_count = 1U;
    first.pcm = {std::byte{1}};
    auto second = first;
    second.pcm = {std::byte{2}};
    const auto initial =
        paths.allocate("file", "physical", "Sample", axk::audio_internal::WavSource::from_physical(first));
    ASSERT_TRUE(initial);
    const auto reused =
        paths.allocate("file", "physical", "Sample", axk::audio_internal::WavSource::from_physical(first));
    ASSERT_TRUE(reused);
    EXPECT_EQ(*reused, *initial);
    const auto collision =
        paths.allocate("file", "physical", "Sample", axk::audio_internal::WavSource::from_physical(second));
    ASSERT_FALSE(collision);
    EXPECT_NE(collision.error().message.find("distinct WAV contents"), std::string::npos);
}
