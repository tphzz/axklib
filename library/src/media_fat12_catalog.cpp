#include "media_internal.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <ranges>
#include <set>

#include "axklib/floppy_catalog_internal.hpp"

namespace axk::detail {
namespace {

MediaValidationIssue issue(std::string code, std::string message, std::string_view path,
                           std::string recommended_next_check) {
    return {std::move(code), std::move(message), std::string{path}, "Yamaha YAMAHA.SYM catalog contract",
            std::move(recommended_next_check)};
}

std::optional<std::pair<std::string, std::uint16_t>> parse_disk_label(std::string_view label) {
    if (label.size() != 16U || std::isdigit(static_cast<unsigned char>(label[14])) == 0 ||
        std::isdigit(static_cast<unsigned char>(label[15])) == 0) {
        return std::nullopt;
    }
    const auto index = static_cast<std::uint16_t>((label[14] - '0') * 10 + (label[15] - '0'));
    if (index == 0U)
        return std::nullopt;
    return std::pair{std::string{label.substr(0U, 14U)}, index};
}

std::optional<std::string_view> logical_category(std::string_view path) {
    if (path.size() < 3U || path.front() != '\\')
        return std::nullopt;
    const auto separator = path.find('\\', 1U);
    if (separator == std::string_view::npos || separator == 1U || separator + 1U == path.size())
        return std::nullopt;
    return path.substr(0U, separator);
}

std::optional<ObjectType> category_type(std::string_view category) {
    if (category == R"(\PROG)")
        return ObjectType::prog;
    if (category == R"(\SBAC)")
        return ObjectType::sbac;
    if (category == R"(\SBNK)")
        return ObjectType::sbnk;
    if (category == R"(\SMPL)")
        return ObjectType::smpl;
    if (category == R"(\SEQU)")
        return ObjectType::sequ;
    if (category == R"(\PRF3)")
        return ObjectType::prf3;
    return std::nullopt;
}

} // namespace

FatCatalogInspection inspect_yamaha_floppy_catalog(const FatImage &image, const CancellationToken &cancellation) {
    FatCatalogInspection result;
    const auto yamaha = std::ranges::find_if(image.files(), [](const FatFile &file) {
        return file.path.find('/') == std::string::npos && is_yamaha_floppy_catalog_path(file.path);
    });
    if (yamaha == image.files().end()) {
        result.issues.push_back(issue("FLOPPY_CATALOG_MISSING", "FAT12 image has no root YAMAHA.SYM catalog", "\\",
                                      "Treat this as a standalone recovery source, not a trusted disk-set member"));
        return result;
    }
    const auto bytes = image.read_file(*yamaha, cancellation);
    if (!bytes) {
        result.issues.push_back(issue("FLOPPY_CATALOG_INVALID", "YAMAHA.SYM could not be read", "\\YAMAHA.SYM",
                                      "Repair or replace the catalog before attaching companion disks"));
        return result;
    }
    auto catalog = decode_yamaha_floppy_catalog(*bytes);
    if (!catalog) {
        result.issues.push_back(issue("FLOPPY_CATALOG_INVALID", catalog.error().message, "\\YAMAHA.SYM",
                                      "Treat this as a standalone recovery source, not a trusted disk-set member"));
        return result;
    }

    result.identity.label = catalog->disk_name;
    const auto parsed_label = parse_disk_label(catalog->disk_name);
    if (parsed_label) {
        result.identity.set_name = parsed_label->first;
        result.identity.index = parsed_label->second;
    } else {
        result.issues.push_back(
            issue("FLOPPY_SET_LABEL_INVALID",
                  std::format("Yamaha disk label '{}' has no exact two-digit set index", catalog->disk_name),
                  "\\YAMAHA.SYM", "Use explicit recovery; ordered disk-set attachment is disabled"));
    }

    std::map<std::uint16_t, std::vector<const FatFile *>> files_by_slot;
    for (const auto &file : image.files()) {
        if (&file == &*yamaha || file.path.find('/') != std::string::npos)
            continue;
        const auto slot = yamaha_floppy_filename_slot(file.name);
        if (slot)
            files_by_slot[*slot].push_back(&file);
    }

    std::set<std::uint16_t> catalog_slots;
    std::size_t continuation_markers{};
    std::size_t final_markers{};
    for (const auto &entry : catalog->files) {
        catalog_slots.insert(entry.slot);
        const bool standalone = entry.logical_path == R"(\A3000.SYM)";
        const bool continuation = entry.logical_path == R"(\A3000F.SYM)";
        const bool final = entry.logical_path == R"(\A3000E.SYM)";
        continuation_markers += continuation ? 1U : 0U;
        final_markers += final ? 1U : 0U;

        const auto physical = files_by_slot.find(entry.slot);
        if (physical == files_by_slot.end()) {
            result.issues.push_back(
                issue("FLOPPY_CATALOG_SLOT_MISSING",
                      std::format("YAMAHA.SYM slot {} ('{}') has no matching FAT file", entry.slot, entry.logical_path),
                      entry.logical_path, "Select the original disk image or use unverified recovery"));
            continue;
        }
        if (physical->second.size() != 1U) {
            result.issues.push_back(issue("FLOPPY_CATALOG_SLOT_DUPLICATE",
                                          std::format("YAMAHA.SYM slot {} maps to more than one FAT file", entry.slot),
                                          entry.logical_path, "Repair duplicate numeric FAT filename extensions"));
            continue;
        }
        const auto &file = *physical->second.front();
        if (standalone || continuation || final) {
            if (file.size != 0U) {
                result.issues.push_back(
                    issue("FLOPPY_SET_MARKER_INVALID",
                          std::format("Disk-set marker '{}' is not zero length", entry.logical_path),
                          entry.logical_path, "Use an exact Yamaha continuation marker"));
            }
            continue;
        }

        const auto category = logical_category(entry.logical_path);
        if (!category || !std::ranges::contains(catalog->categories, std::string{*category})) {
            result.issues.push_back(issue("FLOPPY_CATALOG_PATH_INVALID",
                                          std::format("Catalog path '{}' has no declared category", entry.logical_path),
                                          entry.logical_path, "Repair the logical path and category table"));
            continue;
        }
        const auto expected_type = category_type(*category);
        if (!expected_type)
            continue;
        const auto prefix = image.read_file_prefix(file, 0x42U, cancellation);
        const auto header =
            prefix ? decode_object_header(*prefix) : Result<ObjectHeader>{std::unexpected{prefix.error()}};
        if (!header || header->type != *expected_type) {
            result.issues.push_back(issue("FLOPPY_CATALOG_OBJECT_TYPE_MISMATCH",
                                          std::format("Catalog path '{}' disagrees with FAT file '{}''s object type",
                                                      entry.logical_path, file.path),
                                          entry.logical_path,
                                          "Use unverified recovery; do not join this disk to a trusted set"));
        }
    }

    for (const auto &[slot, files] : files_by_slot) {
        if (!catalog_slots.contains(slot)) {
            result.issues.push_back(
                issue("FLOPPY_CATALOG_FILE_UNLISTED",
                      std::format("FAT file '{}' uses uncataloged slot {}", files.front()->path, slot),
                      files.front()->path, "Use unverified recovery or repair YAMAHA.SYM"));
        }
    }

    if (continuation_markers > 0U && final_markers > 0U) {
        result.identity.marker = FloppySetMarker::invalid;
        result.issues.push_back(issue("FLOPPY_SET_MARKER_CONFLICT",
                                      "YAMAHA.SYM contains both continuation and final markers", "\\YAMAHA.SYM",
                                      "Use an image with exactly one disk-set marker"));
    } else if (continuation_markers == 1U) {
        result.identity.marker = FloppySetMarker::continuation;
    } else if (final_markers == 1U) {
        result.identity.marker = FloppySetMarker::final;
    } else if (continuation_markers > 1U || final_markers > 1U) {
        result.identity.marker = FloppySetMarker::invalid;
        result.issues.push_back(issue("FLOPPY_SET_MARKER_INVALID", "YAMAHA.SYM contains duplicate disk-set markers",
                                      "\\YAMAHA.SYM", "Use an image with exactly one disk-set marker"));
    }

    result.catalog = std::move(*catalog);
    result.identity.trusted_for_disk_set = parsed_label.has_value() &&
                                           result.identity.marker != FloppySetMarker::none &&
                                           result.identity.marker != FloppySetMarker::invalid && result.issues.empty();
    return result;
}

} // namespace axk::detail
