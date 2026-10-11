#include "detail.hpp"
#include "rawdisk/scsi2sd/xml.hpp"
#include "scsi2sd_common.hpp"

#include <expat.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rawdisk::scsi2sd::xml {
namespace {
using rawdisk::detail::fail;
constexpr std::size_t kMaxXmlBytes = 256U * 1024U;
constexpr std::size_t kMaxTargets = 8;
using Fields = std::map<std::string, std::string, std::less<>>;

template <class T> std::optional<T> number(std::string_view s) {
    const std::string t = rawdisk::detail::trim(s);
    std::string_view v = t;
    int base = 10;
    if (v.starts_with("0x") || v.starts_with("0X")) {
        v.remove_prefix(2);
        base = 16;
    }
    if (v.empty())
        return std::nullopt;
    std::uint64_t x = 0;
    const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), x, base);
    if (ec != std::errc{} || p != v.data() + v.size() || x > std::numeric_limits<T>::max())
        return std::nullopt;
    return static_cast<T>(x);
}

template <class T> bool read_number(const Fields &fields, std::string_view tag, T &out, bool required) {
    const auto at = fields.find(tag);
    if (at == fields.end())
        return !required;
    const auto value = number<T>(at->second);
    if (!value)
        return false;
    out = *value;
    return true;
}

Result<Target> decode_target(const Fields &fields, std::uint8_t id, std::size_t index) {
    Target t;
    t.index = index;
    t.scsi_id = id;
    const auto enabled = fields.find("enabled");
    if (enabled == fields.end() || (enabled->second != "true" && enabled->second != "false"))
        return fail(Errc::layout_invalid);
    t.enabled = enabled->second == "true";
    if (!read_number(fields, "sdSectorStart", t.sd_sector_start, t.enabled) ||
        !read_number(fields, "scsiSectors", t.scsi_sectors, t.enabled) ||
        !read_number(fields, "bytesPerSector", t.bytes_per_sector, t.enabled) ||
        !read_number(fields, "deviceType", t.device_type, false) ||
        !read_number(fields, "deviceTypeModifier", t.device_type_modifier, false) ||
        !read_number(fields, "sectorsPerTrack", t.sectors_per_track, false) ||
        !read_number(fields, "headsPerCylinder", t.heads_per_cylinder, false) || t.bytes_per_sector > 8192U ||
        t.sectors_per_track > 255U || t.heads_per_cylinder > 255U ||
        (t.enabled && (t.scsi_sectors == 0U || t.bytes_per_sector == 0U)))
        return fail(Errc::layout_invalid);
    for (const auto &[tag, destination] :
         {std::pair{"vendor", &t.vendor}, {"prodId", &t.product}, {"revision", &t.revision}, {"serial", &t.serial}})
        if (const auto at = fields.find(tag); at != fields.end())
            *destination = at->second;
    return t;
}

// SAX callbacks retain only direct board/target fields. No entity loader is installed.
struct Document {
    XML_Parser parser{};
    Config config;
    Fields fields;
    std::set<std::uint8_t> enabled_ids;
    std::string section, field, text;
    std::uint8_t id{};
    unsigned depth{};
    bool board_seen{}, failed{};
    std::exception_ptr exception;

    void reject() {
        failed = true;
        XML_StopParser(parser, XML_FALSE);
    }
    template <class F> void callback(F action) noexcept {
        try {
            if (!failed)
                action();
        } catch (...) {
            exception = std::current_exception();
            reject();
        }
    }
    void start(std::string_view name, const XML_Char **attributes) {
        ++depth;
        if (depth > 32U || (name == "SCSI2SD" && depth != 1U) ||
            ((name == "SCSITarget" || name == "BoardConfig" || name == "S2S_BoardCfg") && depth != 2U)) {
            reject();
            return;
        }
        if (depth == 1U) {
            if (name != "SCSI2SD" || *attributes)
                reject();
        } else if (depth == 2U) {
            section = name;
            fields.clear();
            if (name == "SCSITarget") {
                if (config.targets.size() >= kMaxTargets || !attributes[0] || std::string_view{attributes[0]} != "id" ||
                    attributes[2]) {
                    reject();
                    return;
                }
                const auto value = number<std::uint8_t>(attributes[1]);
                if (!value || *value > 7U)
                    reject();
                else
                    id = *value;
            } else if (name == "BoardConfig" || name == "S2S_BoardCfg") {
                if (board_seen || *attributes)
                    reject();
                board_seen = true;
            }
        } else if (section == "SCSITarget" || section == "BoardConfig" || section == "S2S_BoardCfg") {
            if (depth != 3U || *attributes || fields.contains(name)) {
                reject();
                return;
            }
            field = name;
            text.clear();
        }
    }
    void end() {
        if (depth == 3U && !field.empty()) {
            auto value = rawdisk::detail::trim(text);
            fields.emplace(field, value);
            if (section != "SCSITarget")
                config.board.emplace_back(field, std::move(value));
            field.clear();
        } else if (depth == 2U && section == "SCSITarget") {
            auto target = decode_target(fields, id, config.targets.size());
            if (!target || (target->enabled && !enabled_ids.insert(id).second))
                reject();
            else
                config.targets.push_back(std::move(*target));
        }
        --depth;
    }
};

Result<Config> parse_impl(std::string_view xml) {
    if (xml.size() > kMaxXmlBytes)
        return fail(Errc::layout_invalid);
    const std::unique_ptr<XML_ParserStruct, decltype(&XML_ParserFree)> parser{XML_ParserCreate(nullptr),
                                                                              XML_ParserFree};
    if (!parser)
        return fail(std::make_error_code(std::errc::not_enough_memory));
    Document doc;
    doc.parser = parser.get();
    XML_SetUserData(parser.get(), &doc);
    XML_SetParamEntityParsing(parser.get(), XML_PARAM_ENTITY_PARSING_NEVER);
    XML_SetStartDoctypeDeclHandler(parser.get(), [](void *data, const XML_Char *, const XML_Char *, const XML_Char *,
                                                    int) { static_cast<Document *>(data)->reject(); });
    XML_SetElementHandler(
        parser.get(),
        [](void *data, const XML_Char *name, const XML_Char **attributes) {
            auto &d = *static_cast<Document *>(data);
            d.callback([&] { d.start(name, attributes); });
        },
        [](void *data, const XML_Char *) {
            auto &d = *static_cast<Document *>(data);
            d.callback([&] { d.end(); });
        });
    XML_SetCharacterDataHandler(parser.get(), [](void *data, const XML_Char *value, int length) {
        auto &d = *static_cast<Document *>(data);
        d.callback([&] {
            const std::string_view chunk{value, static_cast<std::size_t>(length)};
            if (!d.field.empty())
                d.text += chunk;
            else if (!rawdisk::detail::trim(chunk).empty())
                d.reject();
        });
    });
    const auto result = XML_Parse(parser.get(), xml.data(), static_cast<int>(xml.size()), XML_TRUE);
    if (doc.exception)
        std::rethrow_exception(doc.exception);
    if (result != XML_STATUS_OK || doc.failed || doc.config.targets.empty())
        return fail(Errc::layout_invalid);
    return std::move(doc.config);
}
} // namespace

Result<Config> parse(std::string_view xml) noexcept {
    return rawdisk::detail::guard([&] { return parse_impl(xml); });
}

Result<Config> load(const std::filesystem::path &path) noexcept {
    return rawdisk::detail::guard([&]() -> Result<Config> {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
            return fail(ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory));
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return fail(std::make_error_code(std::errc::no_such_file_or_directory));
        std::string text;
        char buf[4096];
        while (f.read(buf, sizeof buf) || f.gcount() > 0) {
            text.append(buf, static_cast<std::size_t>(f.gcount()));
            if (text.size() > kMaxXmlBytes)
                return fail(Errc::layout_invalid);
        }
        if (f.bad())
            return fail(std::make_error_code(std::errc::io_error));
        return parse_impl(text);
    });
}

VolumeSet to_volumes(const Config &c, std::uint64_t n, Board board) {
    std::vector<Range> protect;
    if (board == Board::v6 && n >= 2)
        protect.push_back({n - 2, n});
    return detail::build_volumes(board == Board::v6 ? "scsi2sd-xml" : "scsi2sd-xml-v5", c.targets, n,
                                 board == Board::v6, std::move(protect));
}
} // namespace rawdisk::scsi2sd::xml
