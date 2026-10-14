#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "axklib/formats/tx16w/codec.hpp"

namespace axk {
class FatImage;

namespace tx16w {

enum class ParseNoticeDisposition : std::uint8_t { defaulted, omitted };
enum class ImportMode : std::uint8_t { hierarchy, wave_data_only };

struct ParseNotice {
    ParseNoticeDisposition disposition{ParseNoticeDisposition::omitted};
    std::string source_object;
    std::string source_parameter;
    std::string message;
};

struct SourceFile {
    std::string name;
    std::vector<std::byte> bytes;
    std::string source_member;
};

struct Inspection {
    Profile profile{Profile::yamaha_native};
    std::vector<NativeSetup> setups;
    std::vector<Wave> waves;
    std::vector<std::string> unsupported_files;
    std::vector<ParseNotice> notices;
};

[[nodiscard]] AXK_API Result<Inspection> inspect_files(std::span<const SourceFile> files,
                                                       const CancellationToken &cancellation = {});
[[nodiscard]] AXK_API Result<Inspection> inspect_disk(const FatImage &disk, const CancellationToken &cancellation = {});

} // namespace tx16w
} // namespace axk
