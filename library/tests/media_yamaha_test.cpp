#include "media_test_fixtures.hpp"

TEST(StandaloneObject, UsesSharedDecoderAndRejectsArbitraryFiles) {
    auto object = axk::StandaloneObject::open(std::make_shared<axk::MemoryReader>(smpl_object()), "sample.obj");
    ASSERT_TRUE(object);
    EXPECT_EQ(object->object().decoded.header.name, "TEST");

    std::vector<std::byte> arbitrary(64);
    auto invalid =
        axk::StandaloneObject::open(std::make_shared<axk::MemoryReader>(std::move(arbitrary)), "invalid.bin");
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, axk::ErrorCode::container_unrecognized);
}

TEST(MediaPaths, SanitizesTraversalAndPlatformReservedCharacters) {
    EXPECT_EQ(axk::sanitize_path_component("../unsafe/name", "fallback"), "unsafe_name");
    EXPECT_EQ(axk::sanitize_path_component("..", "fallback"), "fallback");
    EXPECT_EQ(axk::sanitize_path_component("A:B*C?", "fallback"), "A_B_C");
    EXPECT_EQ(axk::sanitize_path_component("  Partition      A  ", "fallback"), "Partition A");
    EXPECT_EQ(axk::sanitize_path_component("A___B*", "fallback"), "A_B (2)");
}

TEST(MediaPaths, DisambiguatesDuplicateDisplayedIsoVolumesByRawIdentifier) {
    auto decoded = axk::decode_object(smpl_object());
    ASSERT_TRUE(decoded);
    axk::MediaObject first{"first",
                           "GROUP/F001/F000",
                           "scope",
                           "GROUP",
                           "F001",
                           {"ORGANS", axk::LabelStatus::confirmed, "menu"},
                           {"Or11 Argent", axk::LabelStatus::confirmed, "menu"},
                           0,
                           1,
                           *decoded,
                           {},
                           {}};
    axk::MediaObject second = first;
    second.key = "second";
    second.logical_path = "GROUP/F002/F000";
    second.raw_volume = "F002";
    const std::array objects{first, second};
    const auto paths = axk::structured_object_paths(objects);
    ASSERT_EQ(paths.size(), 2U);
    EXPECT_EQ(paths[0].relative_path.generic_string(), "ORGANS/Or11 Argent (F001)/SMPL/TEST");
    EXPECT_EQ(paths[1].relative_path.generic_string(), "ORGANS/Or11 Argent (F002)/SMPL/TEST");
}
