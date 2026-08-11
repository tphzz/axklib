#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/media.hpp"

namespace axk::detail {

inline constexpr std::size_t yamaha_floppy_catalog_bytes = 257U * 38U;

[[nodiscard]] Result<std::vector<std::byte>>
encode_yamaha_floppy_catalog(std::string_view disk_name, std::span<const YamahaFloppyCatalogEntry> files,
                             std::span<const std::string> categories);
[[nodiscard]] Result<YamahaFloppyCatalog> decode_yamaha_floppy_catalog(std::span<const std::byte> bytes);
[[nodiscard]] bool is_yamaha_floppy_catalog_path(std::string_view path);
[[nodiscard]] Result<std::uint16_t> yamaha_floppy_filename_slot(std::string_view filename);

} // namespace axk::detail
