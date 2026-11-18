#include "axklib/writer_internal.hpp"

#include "axklib/file_publication.hpp"
#include "axklib/filesystems/fat/floppy_writer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t root_directory_entries = 224U;
constexpr std::size_t directory_attribute_offset = 0x0bU;
constexpr std::string_view single_marker_logical_path{"\\A3000.SYM"};
constexpr std::string_view single_marker_physical_path{"A3000_SY.001"};
constexpr std::size_t boot_jump_offset = 0x00;
constexpr std::size_t oem_name_offset = 0x03;
constexpr std::size_t reserved_boot_offset = 0x25;
constexpr std::size_t extended_boot_signature_offset = 0x26;
constexpr std::size_t volume_label_offset = 0x2b;
constexpr std::size_t filesystem_name_offset = 0x36;

void apply_yamaha_floppy_profile(std::span<std::byte> bytes) {
    constexpr std::array<std::byte, 3> jump{std::byte{0xeb}, std::byte{0x58}, std::byte{0x90}};
    constexpr std::string_view oem_name{"WINIMAGE"};
    constexpr std::string_view filesystem_name{"FAT12   "};

    std::ranges::copy(jump, bytes.begin() + boot_jump_offset);
    std::ranges::transform(oem_name, bytes.begin() + oem_name_offset,
                           [](char character) { return static_cast<std::byte>(character); });
    bytes[reserved_boot_offset] = std::byte{0};
    bytes[extended_boot_signature_offset] = std::byte{0x29};
    std::fill_n(bytes.begin() + volume_label_offset, 11, std::byte{' '});
    std::ranges::transform(filesystem_name, bytes.begin() + filesystem_name_offset,
                           [](char character) { return static_cast<std::byte>(character); });
}

} // namespace

namespace axk::detail {

namespace {

bool is_single_marker_physical_path(std::string_view path) { return path == single_marker_physical_path; }

Result<void> validate_yamaha_fat12_profile(std::span<const std::byte> bytes, const YamahaFloppyCatalog &expected,
                                           const CancellationToken &cancellation) {
    auto reader = std::make_shared<MemoryReader>(std::vector<std::byte>{bytes.begin(), bytes.end()});
    auto fat = FatImage::open(std::move(reader), "generated Yamaha FAT12 image", cancellation);
    if (!fat)
        return std::unexpected{fat.error()};
    for (const auto &file : fat->files()) {
        if (file.directory_offset + directory_attribute_offset >= bytes.size() ||
            bytes[static_cast<std::size_t>(file.directory_offset) + directory_attribute_offset] != std::byte{}) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "generated Yamaha FAT12 root entry has nonzero attributes")};
        }
    }

    const auto yamaha = std::ranges::find(fat->files(), std::string{"YAMAHA.SYM"}, &FatFile::path);
    if (yamaha == fat->files().end()) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "generated Yamaha FAT12 image is missing YAMAHA.SYM")};
    }
    auto actual_bytes = fat->read_file(*yamaha, cancellation);
    if (!actual_bytes)
        return std::unexpected{actual_bytes.error()};
    auto actual = decode_yamaha_floppy_catalog(*actual_bytes);
    if (!actual)
        return std::unexpected{actual.error()};
    auto expected_bytes = encode_yamaha_floppy_catalog(expected.disk_name, expected.files, expected.categories);
    if (!expected_bytes)
        return std::unexpected{expected_bytes.error()};
    if (*actual_bytes != *expected_bytes) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "generated Yamaha FAT12 catalog differs from its write plan")};
    }

    for (const auto &entry : expected.files) {
        const auto separator = entry.logical_path.find_last_of('\\');
        if (separator == std::string::npos || separator + 1U == entry.logical_path.size()) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "generated Yamaha FAT12 catalog path is invalid")};
        }
        auto filename = yamaha_floppy_physical_filename(entry.logical_path.substr(separator + 1U), entry.slot);
        if (!filename)
            return std::unexpected{filename.error()};
        const auto physical = std::ranges::find(fat->files(), *filename, &FatFile::path);
        if (physical == fat->files().end()) {
            return std::unexpected{
                make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                           std::format("generated Yamaha FAT12 catalog path '{}' has no physical file '{}'",
                                       entry.logical_path, *filename))};
        }
        if ((entry.logical_path == single_marker_logical_path || entry.logical_path == "\\A3000F.SYM" ||
             entry.logical_path == "\\A3000E.SYM") &&
            physical->size != 0U) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "generated Yamaha FAT12 marker file is not empty")};
        }
    }
    return {};
}

} // namespace

Result<std::vector<std::string>> plan_fat12_object_filenames(const PreparedMediaImage &image) {
    std::set<std::string> filenames{"YAMAHA.SYM"};
    if (!image.floppy_catalog)
        filenames.insert(std::string{single_marker_physical_path});
    for (const auto &retained : image.retained_files) {
        if (is_yamaha_floppy_catalog_path(retained.path))
            continue;
        if (!image.floppy_catalog && is_single_marker_physical_path(retained.path))
            continue;
        if (retained.path.empty() || retained.path.size() > 12U ||
            retained.path.find_first_of("/\\") != std::string::npos || !filenames.insert(retained.path).second) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "retained FAT12 files must be unique root-level 8.3 names")};
        }
    }

    std::vector<std::string> result;
    result.reserve(image.objects.size());
    for (std::size_t index = 0U; index < image.objects.size(); ++index) {
        const auto &object = image.objects[index];
        if (!object.fat_filename.empty()) {
            if (object.fat_filename.size() > 12U || object.fat_filename.find_first_of("/\\") != std::string::npos ||
                !filenames.insert(object.fat_filename).second) {
                return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                                  "prepared FAT12 object filenames must be unique 8.3 names")};
            }
            result.push_back(object.fat_filename);
            continue;
        }
        if (index >= 222U) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "Yamaha floppy catalogs support at most 222 generated objects")};
        }
        const auto slot = static_cast<std::uint16_t>(index + 2U);
        auto filename = *yamaha_floppy_physical_filename(object.name, slot);
        if (!filenames.insert(filename).second) {
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "FAT12 object filename space is exhausted")};
        }
        result.push_back(std::move(filename));
    }
    return result;
}

Result<std::vector<std::byte>> build_fat12_image(const PreparedMediaImage &image,
                                                 const CancellationToken &cancellation) {
    const auto retained_count =
        static_cast<std::size_t>(std::ranges::count_if(image.retained_files, [&image](const PreparedMediaFile &file) {
            return !is_yamaha_floppy_catalog_path(file.path) &&
                   (image.floppy_catalog || !is_single_marker_physical_path(file.path));
        }));
    const auto single_marker_count = image.floppy_catalog ? 0U : 1U;
    if (image.objects.size() + retained_count + single_marker_count + 1U > root_directory_entries) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "FAT12 floppy profile supports at "
                                          "most 224 root-directory objects")};
    }
    auto object_filenames = plan_fat12_object_filenames(image);
    if (!object_filenames)
        return std::unexpected{object_filenames.error()};
    YamahaFloppyCatalog catalog;
    if (image.floppy_catalog) {
        catalog = *image.floppy_catalog;
    } else {
        for (const auto &retained : image.retained_files) {
            if (!is_yamaha_floppy_catalog_path(retained.path))
                continue;
            if (auto retained_catalog = decode_yamaha_floppy_catalog(retained.payload); retained_catalog)
                catalog.disk_name = std::move(retained_catalog->disk_name);
            break;
        }
        if (catalog.disk_name.empty()) {
            auto disk_name = yamaha_floppy_disk_name(image.manifest.volume_name, 1U);
            if (!disk_name)
                return std::unexpected{disk_name.error()};
            catalog.disk_name = std::move(*disk_name);
        }
        catalog.categories = yamaha_floppy_categories(image.objects);
        catalog.files.reserve(image.objects.size() + 1U);
        catalog.files.push_back({1U, std::string{single_marker_logical_path}});
        for (std::size_t index = 0U; index < image.objects.size(); ++index) {
            const auto slot = yamaha_floppy_filename_slot((*object_filenames)[index]);
            if (!slot) {
                return std::unexpected{slot.error()};
            }
            auto path = yamaha_floppy_object_path(image.objects[index].type, image.objects[index].name);
            if (!path)
                return std::unexpected{path.error()};
            catalog.files.push_back({*slot, std::move(*path)});
        }
    }
    auto catalog_bytes = encode_yamaha_floppy_catalog(catalog.disk_name, catalog.files, catalog.categories);
    if (!catalog_bytes)
        return std::unexpected{catalog_bytes.error()};

    std::vector<filesystems::FloppyFile> files;
    files.reserve(single_marker_count + retained_count + image.objects.size() + 1U);
    if (!image.floppy_catalog)
        files.push_back({std::string{single_marker_physical_path}, std::span<const std::byte>{}});
    for (const auto &retained : image.retained_files) {
        if (!is_yamaha_floppy_catalog_path(retained.path) &&
            (image.floppy_catalog || !is_single_marker_physical_path(retained.path))) {
            files.push_back({retained.path, std::span{retained.payload}});
        }
    }
    for (std::size_t i = 0; i < image.objects.size(); ++i) {
        const auto &reader = image.objects[i].payload;
        if (!reader)
            return std::unexpected(make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                              "FAT12 object payload reader is missing"));
        files.push_back({(*object_filenames)[i], std::cref(*reader)});
    }
    std::ranges::sort(files, {}, &filesystems::FloppyFile::name);
    files.push_back({"YAMAHA.SYM", std::span{*catalog_bytes}});
    auto disk = filesystems::build_floppy(filesystems::FloppyLayout::hd_1440k, files, cancellation);
    if (!disk)
        return std::unexpected(disk.error());
    apply_yamaha_floppy_profile(*disk);
    if (auto validated = validate_yamaha_fat12_profile(*disk, catalog, cancellation); !validated)
        return std::unexpected{validated.error()};

    return disk;
}

Result<void> write_fat12_image(const PreparedMediaImage &image, TemporaryPublication &publication,
                               const CancellationToken &cancellation) {
    auto bytes = build_fat12_image(image, cancellation);
    if (!bytes)
        return std::unexpected{bytes.error()};
    if (auto resized = publication.resize(bytes->size()); !resized)
        return resized;
    return publication.write_at(0U, *bytes);
}

} // namespace axk::detail
