#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "axklib/media.hpp"

namespace axk {
struct Su700Song {
    std::string name;
    std::vector<std::string> samples;
};
struct Su700Control {
    std::vector<Su700Song> songs;
};
enum class Su700ImportStatus : std::uint8_t { unrelated, complete, unsupported };
enum class Su700ImportRole : std::uint8_t { control, song, sample, extra };
struct Su700ImportFile {
    std::string source_path;
    std::vector<std::string> destination_path;
    std::uint32_t size_bytes{};
    Su700ImportRole role{Su700ImportRole::extra};
};
struct Su700FloppyInspection {
    Su700ImportStatus status{Su700ImportStatus::unrelated};
    std::string issue;
    std::string suggested_volume_name;
    std::size_t song_count{};
    std::size_t sample_count{};
    std::vector<Su700ImportFile> files;
};

// Names retain the eight-byte basename, including spaces before the extension.
[[nodiscard]] AXK_API Result<Su700Control> decode_su700_control(std::span<const std::byte> bytes);
[[nodiscard]] AXK_API Result<Su700FloppyInspection> inspect_su700_floppy(const FatImage &image,
                                                                         const CancellationToken &cancellation = {});
} // namespace axk
