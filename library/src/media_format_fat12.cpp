#include "axklib/writer.hpp"

#include "axklib/file_publication.hpp"
#include "axklib/floppy_catalog_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace axk {
namespace {

constexpr std::size_t sector_bytes = 512U;
constexpr std::size_t sectors_per_fat = 9U;
constexpr std::size_t first_fat_offset = sector_bytes;
constexpr std::size_t second_fat_offset = first_fat_offset + sectors_per_fat * sector_bytes;
constexpr std::size_t root_offset = (1U + 2U * sectors_per_fat) * sector_bytes;
constexpr std::size_t root_entry_bytes = 32U;
constexpr std::size_t data_offset = 0x4200U;
constexpr std::size_t catalog_first_cluster = 2U;
constexpr std::size_t catalog_last_cluster = 21U;

Error floppy_error(std::string message) {
    return make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported, std::move(message));
}

void write_le16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void write_le32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
}

void write_text(std::span<std::byte> bytes, std::size_t offset, std::string_view text) {
    std::ranges::transform(text, bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                           [](char value) { return static_cast<std::byte>(value); });
}

void write_fat12_entry(std::span<std::byte> fat, std::uint16_t cluster, std::uint16_t value) {
    const auto offset = static_cast<std::size_t>(cluster) + static_cast<std::size_t>(cluster) / 2U;
    if (cluster % 2U == 0U) {
        fat[offset] = static_cast<std::byte>(value & 0xffU);
        fat[offset + 1U] = (fat[offset + 1U] & std::byte{0xf0}) | static_cast<std::byte>((value >> 8U) & 0x0fU);
    } else {
        fat[offset] = (fat[offset] & std::byte{0x0f}) | static_cast<std::byte>((value << 4U) & 0xf0U);
        fat[offset + 1U] = static_cast<std::byte>((value >> 4U) & 0xffU);
    }
}

void write_boot_sector(std::span<std::byte> image, FloppyFormatMode mode) {
    write_text(image, 0x00U, std::string_view{"\xeb\x44\x90", 3U});
    write_text(image, 0x03U, "YAMAHA  ");
    write_le16(image, 0x0bU, 512U);
    image[0x0dU] = std::byte{1};
    write_le16(image, 0x0eU, 1U);
    image[0x10U] = std::byte{2};
    write_le16(image, 0x11U, 224U);
    write_le16(image, 0x13U, 2'880U);
    image[0x15U] = std::byte{0xf0};
    write_le16(image, 0x16U, 9U);
    write_le16(image, 0x18U, 18U);
    write_le16(image, 0x1aU, 2U);
    write_le32(image, 0x1cU, 0U);
    write_le32(image, 0x20U, 0U);
    if (mode == FloppyFormatMode::quick)
        std::fill_n(image.begin() + 0x2bU, 11U, std::byte{0x20});
}

void write_fats(std::span<std::byte> image) {
    auto first = image.subspan(first_fat_offset, sectors_per_fat * sector_bytes);
    first[0] = std::byte{0xf0};
    first[1] = std::byte{0xff};
    first[2] = std::byte{0xff};
    for (std::uint16_t cluster = catalog_first_cluster; cluster < catalog_last_cluster; ++cluster)
        write_fat12_entry(first, cluster, static_cast<std::uint16_t>(cluster + 1U));
    write_fat12_entry(first, catalog_last_cluster, 0x0fffU);
    std::ranges::copy(first, image.begin() + static_cast<std::ptrdiff_t>(second_fat_offset));
}

void write_root_directory(std::span<std::byte> image, FloppyFormatMode mode) {
    auto label = image.subspan(root_offset, root_entry_bytes);
    std::fill_n(label.begin(), 11U, std::byte{0x20});
    label[0x0bU] = mode == FloppyFormatMode::quick ? std::byte{0x28} : std::byte{0x08};
    if (mode == FloppyFormatMode::full)
        write_le16(label, 0x16U, 0x2000U);

    auto catalog = image.subspan(root_offset + root_entry_bytes, root_entry_bytes);
    write_text(catalog, 0U, "YAMAHA  SYM");
    catalog[0x0bU] = std::byte{0x20};
    write_le16(catalog, 0x1aU, catalog_first_cluster);
    write_le32(catalog, 0x1cU, static_cast<std::uint32_t>(detail::yamaha_floppy_catalog_bytes));

    auto marker = image.subspan(root_offset + 2U * root_entry_bytes, root_entry_bytes);
    write_text(marker, 0U, "A3000_SY002");
}

Result<std::vector<std::byte>> build_formatted_floppy(FloppyFormatMode mode) {
    if (mode != FloppyFormatMode::quick && mode != FloppyFormatMode::full)
        return std::unexpected{floppy_error("unknown floppy format mode")};
    const auto fill = mode == FloppyFormatMode::quick ? std::byte{} : std::byte{0xe5};
    std::vector<std::byte> image(formatted_floppy_size_bytes, fill);
    std::fill(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(data_offset), std::byte{});
    write_boot_sector(image, mode);
    write_fats(image);
    write_root_directory(image, mode);

    const std::array files{YamahaFloppyCatalogEntry{2U, "\\A3000.SYM"}};
    const std::array categories{std::string{"\\OTHERS"}};
    auto catalog = detail::encode_yamaha_floppy_catalog(std::string(16U, ' '), files, categories);
    if (!catalog)
        return std::unexpected{catalog.error()};
    std::ranges::copy(*catalog, image.begin() + static_cast<std::ptrdiff_t>(data_offset));
    return image;
}

Result<void> validate_formatted_floppy(const std::filesystem::path &path, const CancellationToken &cancellation) {
    auto image = FatImage::open(path, cancellation);
    if (!image)
        return std::unexpected{image.error()};
    const auto &geometry = image->geometry();
    if (geometry.bytes_per_sector != sector_bytes || geometry.sectors_per_cluster != 1U || geometry.fat_count != 2U ||
        geometry.root_entry_count != 224U || geometry.total_sectors != 2'880U || geometry.data_offset != data_offset)
        return std::unexpected{floppy_error("formatted Yamaha floppy failed geometry validation")};
    const auto catalog_file = std::ranges::find(image->files(), "YAMAHA.SYM", &FatFile::path);
    const auto marker_file = std::ranges::find(image->files(), "A3000_SY.002", &FatFile::path);
    if (catalog_file == image->files().end() || catalog_file->first_cluster != catalog_first_cluster ||
        catalog_file->size != detail::yamaha_floppy_catalog_bytes || marker_file == image->files().end() ||
        marker_file->first_cluster != 0U || marker_file->size != 0U)
        return std::unexpected{floppy_error("formatted Yamaha floppy failed root-directory validation")};
    const auto &catalog = image->yamaha_catalog();
    if (!catalog || catalog->disk_name != std::string(16U, ' ') || catalog->files.size() != 1U ||
        catalog->files[0].slot != 2U || catalog->files[0].logical_path != "\\A3000.SYM" ||
        catalog->categories != std::vector<std::string>{"\\OTHERS"} || !image->validation_issues().empty())
        return std::unexpected{floppy_error("formatted Yamaha floppy failed catalog validation")};
    auto objects = image->objects(64U * 1024U * 1024U, cancellation);
    if (!objects)
        return std::unexpected{objects.error()};
    if (!objects->empty())
        return std::unexpected{floppy_error("formatted Yamaha floppy unexpectedly contains sampler objects")};
    return {};
}

} // namespace

Result<FloppyCreationPlan> plan_floppy_creation(FloppyFormatMode mode, const CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (mode != FloppyFormatMode::quick && mode != FloppyFormatMode::full)
        return std::unexpected{floppy_error("unknown floppy format mode")};
    return FloppyCreationPlan{mode, formatted_floppy_size_bytes, 0U};
}

Result<WrittenMediaImage> write_formatted_floppy_image(const FloppyCreationPlan &plan,
                                                       const std::filesystem::path &output_path, bool overwrite,
                                                       const CancellationToken &cancellation) {
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    if (plan.size_bytes != formatted_floppy_size_bytes || plan.object_count != 0U)
        return std::unexpected{floppy_error("floppy creation plan does not match the formatted-media profile")};
    auto image = build_formatted_floppy(plan.mode);
    if (!image)
        return std::unexpected{image.error()};
    if (!overwrite && std::filesystem::exists(output_path))
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "formatted floppy output already exists")};
    std::error_code filesystem_error;
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error)
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "could not create formatted floppy output directory")};
    auto publication = detail::TemporaryPublication::create(output_path);
    if (!publication)
        return std::unexpected{publication.error()};
    if (auto appended = publication->append(*image); !appended)
        return std::unexpected{appended.error()};
    if (auto flushed = publication->flush(); !flushed)
        return std::unexpected{flushed.error()};
    if (auto validated = validate_formatted_floppy(publication->path(), cancellation); !validated)
        return std::unexpected{validated.error()};
    if (const auto checked = cancellation.check(); !checked)
        return std::unexpected{checked.error()};
    const auto mode = overwrite ? detail::PublicationMode::replace_existing : detail::PublicationMode::create_only;
    auto published = publication->publish(mode);
    if (!published)
        return std::unexpected{published.error()};
    return WrittenMediaImage{output_path, MediaImageFormat::fat12_floppy, formatted_floppy_size_bytes,
                             0U,          std::move(*published),          MediaConversionArtifactKind::image,
                             1U};
}

} // namespace axk
