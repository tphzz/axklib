#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/media.hpp"

namespace axk::detail {

inline constexpr std::uint64_t iso_sector_size = 2048;
inline constexpr std::uint64_t iso_pvd_sector = 16;
inline constexpr std::string_view object_magic = "FSFSDEV3SPLX";

struct IsoMenuLabels {
    std::vector<std::pair<std::string, std::string>> groups;
    std::vector<std::pair<std::string, std::string>> volumes;
    std::vector<MediaValidationIssue> validation_issues;
};

[[nodiscard]] Error media_error(ErrorCode code, std::string message, std::string_view source = {},
                                std::optional<std::uint64_t> offset = std::nullopt);
[[nodiscard]] Result<std::vector<std::byte>> read_bytes(const RandomAccessReader &reader, std::uint64_t offset,
                                                        std::size_t size, const CancellationToken &cancellation);
[[nodiscard]] std::uint16_t le16(std::span<const std::byte> bytes, std::size_t offset);
[[nodiscard]] std::uint32_t le32(std::span<const std::byte> bytes, std::size_t offset);
[[nodiscard]] std::uint16_t be16(std::span<const std::byte> bytes, std::size_t offset);
[[nodiscard]] std::uint32_t be32(std::span<const std::byte> bytes, std::size_t offset);
[[nodiscard]] std::string clean_ascii(std::span<const std::byte> bytes);
[[nodiscard]] bool object_prefix(std::span<const std::byte> bytes);
[[nodiscard]] std::string upper_ascii(std::string value);
[[nodiscard]] bool unsafe_component(std::string_view value);

[[nodiscard]] Result<IsoMenuLabels> read_yamaha_iso_menu_labels(const IsoImage &image,
                                                                const CancellationToken &cancellation);

} // namespace axk::detail
