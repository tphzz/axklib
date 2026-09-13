#include <cstddef>
#include <initializer_list>

#include <gtest/gtest.h>

#include "axklib/system_file.hpp"

// This executable links only the core library: parameter inspection must not
// acquire a dependency on the audio import and writer implementation.
TEST(CoreReader, SystemRegisteredParametersDoNotRequireTheAudioLibrary) {
    for (const auto kind : {axk::SystemFileKind::a3000_system, axk::SystemFileKind::a4000_a5000_system2}) {
        const auto native = kind == axk::SystemFileKind::a3000_system;
        axk::DecodedSystemFile file;
        file.kind = kind;
        file.system_bulk_bytes.resize(native ? 0x348U : 0xfe0U);
        file.system_bulk_bytes[(native ? 0x1c8U : 0x3ecU) + 0x6eU] = std::byte{91};
        const auto sample = axk::decode_system_registered_sample(file);
        ASSERT_TRUE(sample);
        EXPECT_EQ(sample->parameters.level, 91);
        EXPECT_TRUE(axk::decode_system_registered_program(file));
        EXPECT_TRUE(axk::decode_system_recording(file));
    }
}
