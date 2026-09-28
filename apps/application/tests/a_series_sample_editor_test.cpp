#include <algorithm>
#include <cstddef>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "a_series_sample_editor.hpp"
#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/object.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {

class SampleEditorStereoLoops : public testing::Test {
  protected:
    std::vector<std::byte> payload;

    void SetUp() override {
        axk::SampleSpec sample;
        sample.name = "Rim";
        sample.storage_format = axk::SampleStorageFormat::a3000_188;
        sample.parameters.root_key = 50U;
        sample.parameters.loop_mode = axk::AudioSamplerLoopMode::forward_one_shot;
        const auto prepared =
            axk::detail::prepare_sbnk_payload(sample, {"Left", 0x100U, 48'000U, 15'000U},
                                              axk::detail::PreparedWaveformMember{"Right", 0x200U, 48'000U, 15'000U});
        ASSERT_TRUE(prepared) << prepared.error().message;
        payload = *prepared;
        axk::ByteWriter writer{payload};
        ASSERT_TRUE(writer.write_be32(0xf8U, 0U));
        ASSERT_TRUE(writer.write_be32(0xfcU, 900U));
        ASSERT_TRUE(writer.write_be32(0x100U, 0U));
        ASSERT_TRUE(writer.write_be32(0x104U, 0U));
        ASSERT_TRUE(writer.write_be32(0x160U, 0U));
    }

    nlohmann::json editing() const {
        const auto decoded = axk::decode_object(payload);
        EXPECT_TRUE(decoded);
        if (!decoded)
            return nullptr;
        axk::ObjectSnapshot snapshot;
        snapshot.object = *decoded;
        snapshot.placement = axk::ObjectPlacement{};
        snapshot.placement->volume_name = "Volume";
        return axk::app::detail::a_series_sample_editor(
            snapshot, payload, true, nlohmann::json::array({{{"frames", 15'000U}}, {{"frames", 15'000U}}}));
    }
};

TEST_F(SampleEditorStereoLoops, DormantUnequalOffsetsDoNotBlockSharedLoopEditing) {
    const auto result = editing();
    ASSERT_FALSE(result.is_null());
    EXPECT_TRUE(result.at("editable"));
    EXPECT_TRUE(result.at("canEditPlayback"));
    const auto &blocked = result.at("blockedParameters");
    EXPECT_EQ(std::ranges::find(blocked, "loop_start_frame"), blocked.end());
    EXPECT_EQ(std::ranges::find(blocked, "loop_length_frames"), blocked.end());
}

TEST_F(SampleEditorStereoLoops, ActiveUnequalLoopsRemainProtected) {
    payload[0xe5U] = std::byte{1};
    ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x100U, 100U));
    ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0x104U, 100U));
    const auto result = editing();
    ASSERT_FALSE(result.is_null());
    const auto &blocked = result.at("blockedParameters");
    EXPECT_NE(std::ranges::find(blocked, "loop_start_frame"), blocked.end());
    EXPECT_NE(std::ranges::find(blocked, "loop_length_frames"), blocked.end());
}

TEST_F(SampleEditorStereoLoops, EmptyLoopsWithUnequalPlaybackRemainProtected) {
    ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0xf4U, 14'000U));
    const auto result = editing();
    ASSERT_FALSE(result.is_null());
    EXPECT_FALSE(result.at("canEditPlayback"));
    const auto &blocked = result.at("blockedParameters");
    EXPECT_NE(std::ranges::find(blocked, "loop_start_frame"), blocked.end());
    EXPECT_NE(std::ranges::find(blocked, "loop_length_frames"), blocked.end());
}

TEST_F(SampleEditorStereoLoops, EmptyLoopsWithPlaybackBeyondStoredAudioRemainProtected) {
    ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0xf0U, 15'001U));
    ASSERT_TRUE(axk::ByteWriter{payload}.write_be32(0xf4U, 15'001U));
    const auto result = editing();
    ASSERT_FALSE(result.is_null());
    const auto &blocked = result.at("blockedParameters");
    EXPECT_NE(std::ranges::find(blocked, "loop_start_frame"), blocked.end());
    EXPECT_NE(std::ranges::find(blocked, "loop_length_frames"), blocked.end());
}

} // namespace
