#include "axklib/writer_internal.hpp"

#include "axklib/floppy_catalog_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <ranges>
#include <set>

namespace axk::detail {
namespace {

constexpr std::size_t catalog_record_bytes = 38U;
constexpr std::size_t catalog_file_records = 224U;
constexpr std::size_t catalog_category_records = 32U;
constexpr std::size_t catalog_record_count = 1U + catalog_file_records + catalog_category_records;
constexpr std::size_t catalog_bytes = catalog_record_count * catalog_record_bytes;
constexpr std::size_t maximum_record_text_bytes = catalog_record_bytes - 2U;

Error catalog_error(std::string message) {
    return make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported, std::move(message));
}

Result<void> write_record(std::span<std::byte> bytes, std::size_t record, std::string_view text) {
    if (record >= catalog_record_count || text.empty() || text.size() > maximum_record_text_bytes)
        return std::unexpected{catalog_error("Yamaha floppy catalog record text is invalid")};
    const auto offset = record * catalog_record_bytes;
    bytes[offset] = std::byte{};
    std::ranges::transform(text, bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1U),
                           [](char value) { return static_cast<std::byte>(value); });
    return {};
}

Result<std::string> read_record(std::span<const std::byte> bytes, std::size_t record) {
    const auto payload = bytes.subspan(record * catalog_record_bytes + 1U, catalog_record_bytes - 1U);
    const auto end = std::ranges::find(payload, std::byte{});
    if (end == payload.end())
        return std::unexpected{catalog_error("Yamaha floppy catalog record is not NUL terminated")};
    return std::string{reinterpret_cast<const char *>(payload.data()), static_cast<std::size_t>(end - payload.begin())};
}

std::string_view object_category(ObjectType type) {
    switch (type) {
    case ObjectType::prog:
        return "PROG";
    case ObjectType::sbac:
        return "SBAC";
    case ObjectType::sbnk:
        return "SBNK";
    case ObjectType::smpl:
        return "SMPL";
    case ObjectType::sequ:
        return "SEQU";
    case ObjectType::prf3:
        return "PRF3";
    default:
        return {};
    }
}

std::string object_stem(std::string_view value) {
    std::string result;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || character == '_')
            result.push_back(static_cast<char>(std::toupper(byte)));
        if (result.size() == 8U)
            break;
    }
    return result.empty() ? "OBJECT" : result;
}

} // namespace

bool is_yamaha_floppy_catalog_path(std::string_view path) {
    return path.size() == 10U && std::ranges::equal(path, std::string_view{"YAMAHA.SYM"}, {}, [](char value) {
               return static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
           });
}

Result<std::vector<std::byte>> encode_yamaha_floppy_catalog(std::string_view disk_name,
                                                            std::span<const YamahaFloppyCatalogEntry> files,
                                                            std::span<const std::string> categories) {
    if (disk_name.empty() || disk_name.size() > maximum_record_text_bytes || categories.size() > 32U)
        return std::unexpected{catalog_error("Yamaha floppy catalog label or category count is invalid")};

    std::vector<std::byte> result(catalog_bytes);
    for (std::size_t record = 1U; record < catalog_record_count; ++record)
        result[record * catalog_record_bytes] = std::byte{0xff};
    if (auto written = write_record(result, 0U, disk_name); !written)
        return std::unexpected{written.error()};

    std::set<std::uint16_t> slots;
    for (const auto &file : files) {
        if (file.slot >= catalog_file_records || !slots.insert(file.slot).second)
            return std::unexpected{catalog_error("Yamaha floppy catalog file slot is duplicate or out of range")};
        if (auto written = write_record(result, static_cast<std::size_t>(file.slot) + 1U, file.logical_path);
            !written) {
            return std::unexpected{written.error()};
        }
    }

    std::set<std::string, std::less<>> unique_categories;
    for (std::size_t index = 0U; index < categories.size(); ++index) {
        if (!unique_categories.insert(categories[index]).second)
            return std::unexpected{catalog_error("Yamaha floppy catalog category is duplicate")};
        if (auto written = write_record(result, 1U + catalog_file_records + index, categories[index]); !written)
            return std::unexpected{written.error()};
    }
    return result;
}

Result<YamahaFloppyCatalog> decode_yamaha_floppy_catalog(std::span<const std::byte> bytes) {
    if (bytes.size() != catalog_bytes || bytes[0] != std::byte{})
        return std::unexpected{catalog_error("Yamaha floppy catalog size or disk-label record is invalid")};
    auto disk_name = read_record(bytes, 0U);
    if (!disk_name || disk_name->empty())
        return std::unexpected{catalog_error("Yamaha floppy catalog disk label is empty")};

    YamahaFloppyCatalog result;
    result.disk_name = std::move(*disk_name);
    for (std::size_t slot = 0U; slot < catalog_file_records; ++slot) {
        const auto record = slot + 1U;
        const auto state = bytes[record * catalog_record_bytes];
        if (state == std::byte{0xff})
            continue;
        if (state != std::byte{})
            return std::unexpected{catalog_error("Yamaha floppy catalog file record state is invalid")};
        auto path = read_record(bytes, record);
        if (!path || path->empty())
            return std::unexpected{catalog_error("Yamaha floppy catalog file path is invalid")};
        result.files.push_back({static_cast<std::uint16_t>(slot), std::move(*path)});
    }
    for (std::size_t index = 0U; index < catalog_category_records; ++index) {
        const auto record = 1U + catalog_file_records + index;
        const auto state = bytes[record * catalog_record_bytes];
        if (state == std::byte{0xff})
            continue;
        if (state != std::byte{})
            return std::unexpected{catalog_error("Yamaha floppy catalog category record state is invalid")};
        auto path = read_record(bytes, record);
        if (!path || path->empty())
            return std::unexpected{catalog_error("Yamaha floppy catalog category path is invalid")};
        result.categories.push_back(std::move(*path));
    }
    return result;
}

Result<std::string> yamaha_floppy_disk_name(std::string_view volume_name, std::size_t disk_index) {
    if (disk_index == 0U || disk_index > 99U)
        return std::unexpected{catalog_error("Yamaha floppy disk index is unsupported")};
    std::string base;
    for (const auto character : volume_name) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0) {
            base.push_back(static_cast<char>(std::toupper(byte)));
        } else if (character == ' ' || character == '_') {
            base.push_back(character);
        } else {
            base.push_back('_');
        }
        if (base.size() == 14U)
            break;
    }
    if (base.empty())
        base = "AXKLIB";
    base.resize(14U, ' ');
    return base + std::format("{:02}", disk_index);
}

std::vector<std::string> yamaha_floppy_categories(std::span<const PreparedMediaObject> objects) {
    std::vector<std::string> result{"\\OTHERS"};
    constexpr std::array types{ObjectType::prog, ObjectType::sbac, ObjectType::sbnk,
                               ObjectType::smpl, ObjectType::sequ, ObjectType::prf3};
    for (const auto type : types) {
        if (std::ranges::contains(objects, type, &PreparedMediaObject::type))
            result.push_back("\\" + std::string{object_category(type)});
    }
    return result;
}

Result<std::string> yamaha_floppy_object_path(ObjectType type, std::string_view name,
                                              std::optional<std::size_t> disk_index) {
    const auto category = object_category(type);
    if (category.empty() || name.empty() || name.size() > 16U ||
        (disk_index && (*disk_index == 0U || *disk_index > 99U)))
        return std::unexpected{catalog_error("Yamaha floppy object path input is unsupported")};
    std::string padded{name};
    padded.resize(16U, ' ');
    auto result = std::format("\\{}\\{}", category, padded);
    if (disk_index)
        result += std::format("{:02}", *disk_index);
    return result;
}

Result<std::string> yamaha_floppy_physical_filename(std::string_view logical_name, std::uint16_t slot) {
    if (logical_name.empty() || slot >= catalog_file_records)
        return std::unexpected{catalog_error("Yamaha floppy physical filename input is unsupported")};
    std::string stem;
    if (logical_name == "A3000F.SYM" || logical_name == "A3000E.SYM") {
        stem = std::string{logical_name.substr(0U, 6U)} + "_S";
    } else {
        stem = object_stem(logical_name);
    }
    return std::format("{}.{:03}", stem, slot);
}

Result<std::uint16_t> yamaha_floppy_filename_slot(std::string_view filename) {
    if (filename.size() < 4U || filename[filename.size() - 4U] != '.')
        return std::unexpected{catalog_error("Yamaha floppy physical filename has no numeric slot")};
    std::uint16_t result{};
    for (const auto character : filename.substr(filename.size() - 3U)) {
        if (character < '0' || character > '9')
            return std::unexpected{catalog_error("Yamaha floppy physical filename has no numeric slot")};
        result = static_cast<std::uint16_t>(result * 10U + static_cast<unsigned int>(character - '0'));
    }
    if (result >= catalog_file_records)
        return std::unexpected{catalog_error("Yamaha floppy physical filename slot is out of range")};
    return result;
}

} // namespace axk::detail
