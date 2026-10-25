#include "detail.hpp"
#include "rawdisk/open/blocks.hpp"
#include "rawdisk/scsi2sd/v6.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <map>

namespace rawdisk::open {
namespace {

std::unexpected<SubmitError> fail(std::error_code ec, std::string step, std::string field = {},
                                  std::string message = {}) {
    if (message.empty())
        message = ec.message();
    return std::unexpected(SubmitError{ec, std::move(step), std::move(field), std::move(message)});
}

std::string human_size(std::uint64_t bytes) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    std::size_t u = 0;
    while (v >= 1000.0 && u + 1 < std::size(units)) {
        v /= 1000.0;
        ++u;
    }
    return u == 0 ? std::format("{} B", bytes) : std::format("{:.1f} {}", v, units[u]);
}

// Percent-encodes everything but unreserved characters (keeps keys readable and ':'-separable).
std::string pct(std::string_view s) {
    std::string out;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~')
            out += static_cast<char>(c);
        else
            out += std::format("%{:02X}", static_cast<unsigned>(c));
    }
    return out;
}

std::filesystem::path path_from_utf8(std::string_view s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

Access pass_access(const BlockEnv &env) { return env.final_pass ? env.access : Access::ReadOnly; }

constexpr bool kPrivilegePrompt =
#if defined(_WIN32) || defined(__APPLE__)
    true;
#else
    false;
#endif

// ---- device ------------------------------------------------------------------------------------

class SelectDevice final : public Block {
  public:
    explicit SelectDevice(std::optional<std::uint64_t> max_bytes) : max_bytes_(max_bytes) {}
    std::string_view id() const noexcept override { return "device"; }

    Outcome<Step> prepare(const BlockEnv &env, const Stage &) override {
        auto list = env.context.devices->list();
        if (!list)
            return fail(list.error(), "device", "", std::format("cannot list devices: {}", list.error().message()));
        devices_.clear();
        std::map<std::string, int> count;
        for (const DeviceInfo &d : *list)
            ++count[device_key(d)];

        Field f{.key = "device", .label = "Device", .help = "The card reader with the card inserted."};
        std::size_t hidden = 0;
        for (const DeviceInfo &d : *list) {
            if (max_bytes_ && d.size_bytes > *max_bytes_) {
                ++hidden;
                continue;
            }
            std::string key = device_key(d);
            if (count[key] > 1)
                key += "@" + d.path; // identical devices without serial: disambiguate by path
            f.options.push_back({.id = key,
                                 .label = std::format("{} ({})", d.model.empty() ? "Unknown device" : d.model,
                                                      human_size(d.size_bytes)),
                                 .detail = std::format("{} · {} B/sector · serial {}", d.path, d.sector_size,
                                                       d.serial.empty() ? "(none)" : d.serial)});
            devices_.emplace(std::move(key), d);
        }
        Step s{
            .id = "device", .title = "Select the device", .help = "USB disks with a medium inserted.", .fields = {f}};
        if (list->empty())
            s.notices.push_back({Severity::info, "device.none", "No USB disk with a medium was found."});
        if (hidden != 0)
            s.notices.push_back({Severity::info, "device.filtered",
                                 std::format("{} device(s) hidden by the {}-byte limit.", hidden, *max_bytes_)});
        s.may_prompt_for_privileges = kPrivilegePrompt;
        s.refreshable = true;
        return s;
    }

    Outcome<Stage> apply(const BlockEnv &env, const Stage &, const Answers &a) override {
        const auto it = devices_.find(a.at("device"));
        if (it == devices_.end())
            return fail(make_error_code(Errc::flow_stale_selection), "device", "device");
        const DeviceInfo &d = it->second;
        auto dev = env.context.devices->open(d, pass_access(env), env.context.authorization);
        if (!dev)
            return fail(dev.error(), "device", "device");
        if (max_bytes_ && ((*dev)->sector_size() == 0 || (*dev)->sector_count() > *max_bytes_ / (*dev)->sector_size()))
            return fail(make_error_code(Errc::device_changed), "device", "device",
                        "opened device exceeds the size limit");
        Stage out;
        out.device = std::move(*dev);
        out.usb = d;
        out.description = std::format("{}{} at {}", d.model.empty() ? "device" : d.model,
                                      d.serial.empty() ? "" : std::format(" (serial {})", d.serial), d.path);
        return out;
    }

  private:
    std::optional<std::uint64_t> max_bytes_;
    std::map<std::string, DeviceInfo> devices_;
};

// ---- image file --------------------------------------------------------------------------------

class SelectImage final : public Block {
  public:
    explicit SelectImage(std::optional<std::uint32_t> fixed) : fixed_(fixed) {}
    std::string_view id() const noexcept override { return "image"; }

    Outcome<Step> prepare(const BlockEnv &, const Stage &) override {
        Step s{.id = "image", .title = "Select the image file"};
        s.fields.push_back({.key = "path",
                            .label = "Image file",
                            .help = "A raw sector image (no container format).",
                            .kind = FieldKind::file,
                            .filters = {{"Disk images", "*.img;*.bin;*.hda;*.iso;*.raw"}, {"All files", "*"}}});
        if (!fixed_) {
            Field ss{.key = "sector_size", .label = "Sector size", .help = "Bytes per sector of the image."};
            for (std::uint32_t v : {512u, 1024u, 2048u, 4096u})
                ss.options.push_back({.id = std::to_string(v), .label = std::format("{} bytes", v)});
            ss.default_value = "512";
            s.fields.push_back(std::move(ss));
        }
        return s;
    }

    Outcome<Stage> apply(const BlockEnv &env, const Stage &, const Answers &a) override {
        const std::string &file = a.at("path");
        const std::uint32_t ss = fixed_ ? *fixed_ : static_cast<std::uint32_t>(std::stoul(a.at("sector_size")));
        auto img = open_image(path_from_utf8(file), pass_access(env), ss);
        if (!img) {
            const std::error_code ec = img.error();
            if (ec == Errc::bad_length)
                return fail(ec, "image", fixed_ ? "path" : "sector_size",
                            std::format("the image size is not a multiple of {} bytes", ss));
            return fail(ec, "image", "path");
        }
        Stage out;
        out.device = std::shared_ptr<BlockDevice>(std::move(*img));
        out.description = file;
        return out;
    }

  private:
    std::optional<std::uint32_t> fixed_;
};

// ---- SCSI2SD V6 ----------------------------------------------------------------------------------

Outcome<void> require_card(const Stage &in, std::string_view step) {
    if (!in.device)
        return fail(make_error_code(Errc::internal_error), std::string(step), "", "no device before this step");
    if (in.device->sector_size() != scsi2sd::kCardSectorSize)
        return fail(make_error_code(Errc::invalid_argument), std::string(step), "",
                    "SCSI2SD cards have 512-byte sectors; this device does not");
    return {};
}

class Scsi2sdV6 final : public Block {
  public:
    std::string_view id() const noexcept override { return "scsi2sd"; }

    Outcome<Step> prepare(const BlockEnv &env, const Stage &in) override {
        if (auto r = require_card(in, "scsi2sd"); !r)
            return std::unexpected(r.error());
        scsi2sd::v6::ScanOptions opts;
        opts.stop = env.stop;
        auto d = scsi2sd::v6::discover(*in.device, opts);
        if (!d)
            return fail(d.error(), "scsi2sd", "",
                        std::format("cannot read the SCSI2SD configuration: {}", d.error().message()));
        discovery_ = std::move(*d);
        Step s{.id = "scsi2sd", .title = "SCSI2SD V6 configuration", .notices = discovery_.notices};
        if (!discovery_.from_card()) {
            s.help = "The card holds no SCSI2SD V6 configuration.";
            s.fields.push_back({.key = "use_default",
                                .label = "Use the firmware default",
                                .help = "Continue with the layout the board uses for such a card: one disk, SCSI ID 0, "
                                        "starting at card sector 0.",
                                .kind = FieldKind::confirm});
        }
        return s;
    }

    Outcome<Stage> apply(const BlockEnv &, const Stage &in, const Answers &a) override {
        if (!discovery_.from_card() && a.at("use_default") != "yes")
            return fail(make_error_code(Errc::layout_not_found), "scsi2sd", "use_default",
                        "no configuration on the card and the firmware default was declined");
        Stage out = in;
        out.volumes = discovery_.volumes;
        return out;
    }

  private:
    scsi2sd::v6::Discovery discovery_;
};

// ---- SCSI2SD XML ---------------------------------------------------------------------------------

class Scsi2sdXml final : public Block {
  public:
    explicit Scsi2sdXml(std::optional<scsi2sd::xml::Board> fixed) : fixed_(fixed) {}
    std::string_view id() const noexcept override { return "xml"; }

    Outcome<Step> prepare(const BlockEnv &, const Stage &in) override {
        if (auto r = require_card(in, "xml"); !r)
            return std::unexpected(r.error());
        Step s{.id = "xml",
               .title = "SCSI2SD layout file",
               .help = "The XML file exported by scsi2sd-util for this card."};
        s.fields.push_back({.key = "file",
                            .label = "Layout file (XML)",
                            .kind = FieldKind::file,
                            .filters = {{"SCSI2SD layout", "*.xml"}, {"All files", "*"}}});
        if (!fixed_) {
            s.fields.push_back({.key = "board",
                                .label = "Board",
                                .help = "V6 keeps its configuration in the last 2 card sectors (protected); V5 keeps "
                                        "it in the board.",
                                .options = {{.id = "v6", .label = "SCSI2SD V6"}, {.id = "v5", .label = "SCSI2SD V5"}},
                                .default_value = "v6"});
        }
        return s;
    }

    Outcome<Stage> apply(const BlockEnv &, const Stage &in, const Answers &a) override {
        auto cfg = scsi2sd::xml::load(path_from_utf8(a.at("file")));
        if (!cfg)
            return fail(cfg.error(), "xml", "file",
                        cfg.error() == Errc::layout_invalid ? "not a valid scsi2sd-util layout file"
                                                            : cfg.error().message());
        const auto board =
            fixed_ ? *fixed_ : (a.at("board") == "v5" ? scsi2sd::xml::Board::v5 : scsi2sd::xml::Board::v6);
        Stage out = in;
        out.volumes = scsi2sd::xml::to_volumes(*cfg, in.device->sector_count(), board);
        return out;
    }

  private:
    std::optional<scsi2sd::xml::Board> fixed_;
};

// ---- volume --------------------------------------------------------------------------------------

class SelectVolume final : public Block {
  public:
    explicit SelectVolume(std::optional<std::uint64_t> max_bytes) : max_bytes_(max_bytes) {}
    std::string_view id() const noexcept override { return "volume"; }

    Outcome<Step> prepare(const BlockEnv &env, const Stage &in) override {
        if (!in.volumes || !in.device)
            return fail(make_error_code(Errc::internal_error), "volume", "", "no layout before this step");
        const VolumeSet &set = *in.volumes;
        const bool rw = env.access == Access::ReadWrite;
        Field f{.key = "volume", .label = "Disk"};
        bool danger = false;
        std::size_t hidden = 0;
        for (const Volume &v : set.volumes) {
            if (max_bytes_ && (v.sector_size == 0 || v.sector_count > *max_bytes_ / v.sector_size)) {
                ++hidden;
                continue;
            }
            Option o{.id = v.id,
                     .label = v.label,
                     .detail = std::format("sectors {}..{} · {} B/sector · {}", v.range.begin,
                                           v.range.empty() ? v.range.begin : v.range.end - 1, v.sector_size,
                                           human_size(v.sector_count * v.sector_size)),
                     .notices = v.notices};
            if (!v.usable) {
                o.enabled = false;
                const auto it =
                    std::ranges::find_if(v.notices, [](const Notice &n) { return n.severity >= Severity::warning; });
                o.disabled_reason = it != v.notices.end() ? it->text : "not accessible";
            } else if (rw && !check_writable(set, v.id)) {
                o.enabled = false;
                o.disabled_reason = "overlaps another disk; cannot be opened for writing here";
            }
            danger |= o.enabled && rw &&
                      (has_severity(v.notices, Severity::danger) || has_severity(set.notices, Severity::danger));
            f.options.push_back(std::move(o));
        }
        Step s{.id = "volume", .title = "Select the disk", .fields = {f}};
        s.notices = in.notices;
        s.notices.insert(s.notices.end(), set.notices.begin(), set.notices.end());
        if (hidden != 0)
            s.notices.push_back({Severity::info, "volume.filtered",
                                 std::format("{} target(s) hidden by the {}-byte limit.", hidden, *max_bytes_)});
        if (danger)
            s.fields.push_back({.key = "acknowledge",
                                .label = "I understand the danger notices",
                                .help = "Required to open a disk with danger notices for writing.",
                                .kind = FieldKind::confirm,
                                .required = false});
        return s;
    }

    Outcome<Stage> apply(const BlockEnv &env, const Stage &in, const Answers &a) override {
        const VolumeSet &set = *in.volumes;
        const std::string &id = a.at("volume");
        const Volume *v = set.find(id);
        if (!v)
            return fail(make_error_code(Errc::flow_stale_selection), "volume", "volume");
        const bool rw = env.access == Access::ReadWrite;
        if (rw && (has_severity(v->notices, Severity::danger) || has_severity(set.notices, Severity::danger))) {
            const auto ack = a.find("acknowledge");
            if (ack == a.end() || ack->second != "yes")
                return fail(make_error_code(Errc::flow_missing_answer), "volume", "acknowledge",
                            "this disk has danger notices; confirm them to open it for writing");
        }
        auto dev = open_volume(in.device, set, id, pass_access(env));
        if (!dev)
            return fail(dev.error(), "volume", "volume");
        if (v->range.begin > (std::numeric_limits<std::uint64_t>::max() - in.offset_bytes) / in.device->sector_size())
            return fail(make_error_code(Errc::out_of_range), "volume", "volume", "root byte offset overflows");
        Stage out;
        out.device = std::move(*dev);
        out.usb = in.usb;
        out.offset_bytes = in.offset_bytes + v->range.begin * in.device->sector_size();
        out.description = std::format("{} on {}", v->label, in.description);
        out.notices = in.notices;
        out.notices.insert(out.notices.end(), set.notices.begin(), set.notices.end());
        out.notices.insert(out.notices.end(), v->notices.begin(), v->notices.end());
        return out;
    }

  private:
    std::optional<std::uint64_t> max_bytes_;
};

} // namespace

std::string device_key(const DeviceInfo &d) {
    return std::format("usb:{}:{}:{}:{}", pct(d.model), pct(d.serial), d.size_bytes, d.sector_size);
}

std::unique_ptr<Block> select_device(std::optional<std::uint64_t> max_bytes) {
    return std::make_unique<SelectDevice>(max_bytes);
}
std::unique_ptr<Block> select_image(std::optional<std::uint32_t> fixed) { return std::make_unique<SelectImage>(fixed); }
std::unique_ptr<Block> scsi2sd_v6() { return std::make_unique<Scsi2sdV6>(); }
std::unique_ptr<Block> scsi2sd_xml(std::optional<scsi2sd::xml::Board> fixed) {
    return std::make_unique<Scsi2sdXml>(fixed);
}
std::unique_ptr<Block> select_volume(std::optional<std::uint64_t> max_bytes) {
    return std::make_unique<SelectVolume>(max_bytes);
}

} // namespace rawdisk::open
