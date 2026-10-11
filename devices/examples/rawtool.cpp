// rawtool: one set of commands for every way of opening a disk (image, raw device, SCSI2SD V5/V6,
// ...). A <target> is a mode id ("scsi2sd-v6") or a locator
// ("scsi2sd-v6?device.device=...&volume.volume=3"). Answers missing from the target are asked
// interactively; the complete locator is printed so the same selection can be reused.
//
//   rawtool list                        USB disks with their identity keys
//   rawtool modes                       available modes
//   rawtool inspect <target>            the next unanswered step (or the result), never asks
//   rawtool info    <target>            what the target resolves to
//   rawtool dump    <target> [lba]      hex dump of one sector
//   rawtool backup  <target> <out.img>
//   rawtool restore <target> <in.img>
// Options: --no-prompt (fail instead of asking), --yes (skip the restore confirmation).
//
// The interactive part is a generic renderer of Step descriptors: it knows nothing about any mode,
// which makes it a reference for building a GUI on the same abstraction.
#include "rawdisk/block_device.hpp"
#include "rawdisk/open/blocks.hpp"
#include "rawdisk/open/flow.hpp"
#include "rawdisk/open/locator.hpp"
#include "rawdisk/open/modes.hpp"
#include "rawtool_common.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace rawdisk;
namespace fs = std::filesystem;
namespace ro = rawdisk::open;

namespace rawtool {

int fail(std::string_view what, const std::error_code &ec, std::string_view detail) {
    std::fputs(std::format("{}: {}\n", what, detail.empty() ? ec.message() : std::string(detail)).c_str(), stderr);
    if (ec == std::errc::permission_denied)
        std::fputs("hint: allow the administrator prompt (Windows/macOS); on Linux install the udev rule "
                   "from linux/ or run as root.\n",
                   stderr);
    else if (ec == Errc::device_busy)
        std::fputs("hint: close programs using the card or image, or unmount it first.\n", stderr);
    else if (ec == Errc::device_changed)
        std::fputs("hint: the card was removed, swapped or changed; select it again.\n", stderr);
    return 1;
}

int fail(const ro::SubmitError &e) {
    std::string where = e.step;
    if (!e.field.empty())
        where += "." + e.field;
    return fail(where.empty() ? "error" : where, e.code, e.message);
}

bool parse_u64(std::string_view s, std::uint64_t &v) {
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} && p == s.data() + s.size() && !s.empty();
}

std::string human(std::uint64_t bytes) {
    return bytes >= 1'000'000 ? std::format("{:.1f} MB", static_cast<double>(bytes) / 1e6)
                              : std::format("{} bytes", bytes);
}

// ---- generic step renderer ---------------------------------------------------------------------

const char *tag(Severity s) {
    switch (s) {
    case Severity::info:
        return "info";
    case Severity::warning:
        return "WARNING";
    case Severity::danger:
        return "DANGER";
    }
    return "?";
}

void print_notices(const std::vector<Notice> &ns, std::string_view indent) {
    for (const Notice &n : ns)
        out("{}[{}] {}\n", indent, tag(n.severity), n.text);
}

void print_field(const ro::Field &f) {
    out("  {} ({}{}){}\n", f.label.empty() ? f.key : f.label, ro::to_string(f.kind), f.required ? "" : ", optional",
        f.default_value ? std::format(" [default: {}]", *f.default_value) : "");
    if (!f.help.empty())
        out("    {}\n", f.help);
    for (std::size_t i = 0; i < f.options.size(); ++i) {
        const ro::Option &o = f.options[i];
        out("    {}) {}{}\n", i + 1, o.label, o.enabled ? "" : "  (not available)");
        if (!o.detail.empty())
            out("       {}\n", o.detail);
        if (!o.enabled && !o.disabled_reason.empty())
            out("       reason: {}\n", o.disabled_reason);
        else
            print_notices(o.notices, "       ");
        out("       id: {}\n", o.id);
    }
}

void print_step(const ro::Flow &flow) {
    const ro::Step &s = flow.step();
    out("\n== Step {}/{}: {} ==\n", flow.step_number(), flow.step_total(), s.title);
    if (!s.help.empty())
        out("{}\n", s.help);
    print_notices(s.notices, "");
    if (s.may_prompt_for_privileges)
        out("(continuing may show an administrator prompt)\n");
    for (const ro::Field &f : s.fields)
        print_field(f);
}

std::optional<std::string> read_line(std::string_view prompt) {
    out("{}", prompt);
    std::string line;
    if (!std::getline(std::cin, line))
        return std::nullopt;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

// Asks for one field; accepts an option number or id for choices.
std::optional<std::string> ask(const ro::Field &f) {
    const std::string label = f.label.empty() ? f.key : f.label;
    for (;;) {
        auto line =
            read_line(std::format("{}{}: ", label, f.default_value ? std::format(" [{}]", *f.default_value) : ""));
        if (!line)
            return std::nullopt;
        if (line->empty() && f.default_value)
            return *f.default_value;
        if (line->empty() && !f.required)
            return std::string();
        if (f.kind == ro::FieldKind::choice) {
            std::uint64_t n = 0;
            if (parse_u64(*line, n) && n >= 1 && n <= f.options.size())
                return f.options[n - 1].id;
            if (std::ranges::any_of(f.options, [&](const ro::Option &o) { return o.id == *line; }))
                return *line;
            out("  please enter a number from the list\n");
            continue;
        }
        if (f.kind == ro::FieldKind::confirm && *line != "yes" && *line != "no") {
            out("  please answer yes or no\n");
            continue;
        }
        if (!line->empty())
            return *line;
    }
}

// Runs a flow to ready(): answers from the locator first, the user for the rest. `inspect` stops at
// the first step that still needs an answer instead of asking.
std::expected<std::unique_ptr<ro::Flow>, int> complete(const std::string &target, Access access, const Options &opt,
                                                       bool inspect) {
    auto loc = ro::parse_locator(target);
    if (!loc)
        return std::unexpected(fail("target", loc.error(), "not a mode id or locator (see `rawtool modes`)"));
    const auto cap = opt.max_device_bytes == 0 ? std::nullopt : std::optional(opt.max_device_bytes);
    const ro::Registry registry = ro::builtin_modes({.max_device_bytes = cap, .max_volume_bytes = cap});
    auto mode = registry.find(loc->mode);
    if (!mode)
        return std::unexpected(
            fail("target", make_error_code(Errc::locator_invalid), std::format("unknown mode '{}'", loc->mode)));

    auto flow = ro::Flow::start(mode, access);
    if (!flow)
        return std::unexpected(fail(flow.error()));
    ro::Flow &f = **flow;
    std::set<std::string, std::less<>> used;  // locator keys consumed
    std::set<std::string, std::less<>> force; // "step.field" to ask again after an error

    while (!f.ready()) {
        const ro::Step &step = f.step();
        ro::Answers a;
        std::vector<const ro::Field *> need;
        for (const ro::Field &field : step.fields) {
            const std::string key = std::format("{}.{}", step.id, field.key);
            if (auto it = loc->answers.find(key); it != loc->answers.end()) {
                a[field.key] = it->second;
                used.insert(key);
            } else if ((field.required && !field.default_value) || force.contains(key)) {
                need.push_back(&field);
            }
        }
        if (!need.empty()) {
            if (inspect)
                return std::move(*flow);
            if (!opt.prompt)
                return std::unexpected(fail(step.id, make_error_code(Errc::flow_missing_answer),
                                            "answers missing in the target (and --no-prompt given)"));
            print_step(f);
            for (const ro::Field *field : need) {
                auto v = ask(*field);
                if (!v)
                    return std::unexpected(fail(step.id, make_error_code(Errc::flow_missing_answer), "input ended"));
                if (!v->empty())
                    a[field->key] = *v;
            }
        }
        const std::string step_id = step.id;
        if (auto r = f.submit(a); !r) {
            if (!opt.prompt || inspect)
                return std::unexpected(fail(r.error()));
            fail(r.error());
            // Forget the offending answer (also from the target) and ask for it next round.
            std::vector<std::string> keys;
            if (!r.error().field.empty())
                keys.push_back(std::format("{}.{}", step_id, r.error().field));
            else
                for (const auto &field : f.step().fields)
                    keys.push_back(std::format("{}.{}", step_id, field.key));
            for (const auto &k : keys) {
                loc->answers.erase(k);
                used.erase(k);
                force.insert(k);
            }
            continue;
        }
        std::erase_if(force, [&](const std::string &k) { return k.starts_with(step_id + "."); });
    }
    for (const auto &[key, value] : loc->answers)
        if (!used.contains(key))
            return std::unexpected(
                fail(key, make_error_code(Errc::flow_invalid_answer), "unknown answer for this mode"));
    return std::move(*flow);
}

void print_preview(const ro::Preview &p) {
    out("\nTarget:  {}\n", p.description);
    out("Size:    {} sectors of {} bytes ({})\n", p.sector_count, p.sector_size, human(p.sector_count * p.sector_size));
    out("Access:  {}\n", p.access == Access::ReadWrite ? "read-write" : "read-only");
    print_notices(p.notices, "         ");
    out("Locator: {}\n", p.locator);
}

// ---- data transfer -----------------------------------------------------------------------------

Result<void> copy(BlockDevice &src, BlockDevice &dst, std::uint64_t sectors, const char *label) {
    const std::uint32_t ss = src.sector_size();
    const std::uint64_t per = std::max<std::uint64_t>(1, (std::uint64_t{1} << 20) / ss);
    AlignedBuffer buf(static_cast<std::size_t>(per * ss));
    for (std::uint64_t lba = 0; lba < sectors;) {
        const std::uint64_t n = std::min(per, sectors - lba);
        const auto chunk = buf.span().first(static_cast<std::size_t>(n * ss));
        if (auto r = src.read(lba, chunk); !r)
            return r;
        if (auto r = dst.write(lba, chunk); !r)
            return r;
        lba += n;
        out("\r{} {:3}%", label, lba * 100 / sectors);
    }
    out("\n");
    return dst.flush();
}

Result<bool> verify(BlockDevice &a, BlockDevice &b, std::uint64_t sectors) {
    const std::uint32_t ss = a.sector_size();
    const std::uint64_t per = std::max<std::uint64_t>(1, (std::uint64_t{1} << 20) / ss);
    AlignedBuffer x(static_cast<std::size_t>(per * ss)), y(static_cast<std::size_t>(per * ss));
    for (std::uint64_t lba = 0; lba < sectors;) {
        const std::uint64_t n = std::min(per, sectors - lba);
        const auto bytes = static_cast<std::size_t>(n * ss);
        if (auto r = a.read(lba, x.span().first(bytes)); !r)
            return std::unexpected(r.error());
        if (auto r = b.read(lba, y.span().first(bytes)); !r)
            return std::unexpected(r.error());
        if (std::memcmp(x.data(), y.data(), bytes) != 0)
            return false;
        lba += n;
        out("\rverifying {:3}%", lba * 100 / sectors);
    }
    out("\n");
    return true;
}

// ---- commands ----------------------------------------------------------------------------------

int cmd_list(const Options &opt) {
    auto devs = list_usb_devices();
    if (!devs)
        return fail("list", devs.error());
    if (devs->empty())
        out("No USB disks with media found.\n");
    std::size_t hidden = 0;
    for (const auto &d : *devs) {
        if (opt.max_device_bytes != 0 && d.size_bytes > opt.max_device_bytes) {
            ++hidden;
            continue;
        }
        out("{}  {}  {}  {} B/sector  serial {}\n  key: {}\n", d.path, d.model.empty() ? "(unknown model)" : d.model,
            human(d.size_bytes), d.sector_size, d.serial.empty() ? "(none)" : d.serial, ro::device_key(d));
    }
    if (hidden != 0)
        out("{} device(s) hidden by the {}-byte limit; use --max-device-bytes 0 to show all carriers.\n", hidden,
            opt.max_device_bytes);
    return 0;
}

int cmd_modes() {
    const ro::Registry registry = ro::builtin_modes(); // keep the registry alive while iterating
    for (const auto &m : registry.list())
        out("{:<20} {}\n{:<20} {}\n", m->info().id, m->info().name, "", m->info().description);
    return 0;
}

int cmd_inspect(const std::string &target, Options opt) {
    opt.prompt = false;
    auto flow = complete(target, Access::ReadOnly, opt, true);
    if (!flow)
        return flow.error();
    if ((*flow)->ready()) {
        print_preview((*flow)->preview());
    } else {
        print_step(**flow);
        out("\nLocator so far: {}\n", (*flow)->locator());
    }
    return 0;
}

int cmd_info(const std::string &target, const Options &opt) {
    auto flow = complete(target, Access::ReadOnly, opt);
    if (!flow)
        return flow.error();
    print_preview((*flow)->preview());
    return 0;
}

int cmd_dump(const std::string &target, std::uint64_t lba, const Options &opt) {
    auto flow = complete(target, Access::ReadOnly, opt);
    if (!flow)
        return flow.error();
    auto o = (*flow)->open();
    if (!o)
        return fail(o.error());
    AlignedBuffer buf(o->device->sector_size());
    if (auto r = o->device->read(lba, buf.span()); !r)
        return fail("read", r.error());
    const auto s = buf.span();
    for (std::size_t off = 0; off < s.size(); off += 16) {
        std::string line = std::format("{:08x}  ", off);
        for (std::size_t i = 0; i < 16; ++i)
            line += std::format("{:02x} ", std::to_integer<unsigned>(s[off + i]));
        line += ' ';
        for (std::size_t i = 0; i < 16; ++i) {
            const auto c = std::to_integer<unsigned char>(s[off + i]);
            line += (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
        }
        out("{}\n", line);
    }
    out("Locator: {}\n", o->locator);
    return 0;
}

int cmd_backup(const std::string &target, const fs::path &path, const Options &opt) {
    std::error_code ec;
    if (fs::exists(path, ec))
        return fail(path.string(), std::make_error_code(std::errc::file_exists));
    auto flow = complete(target, Access::ReadOnly, opt);
    if (!flow)
        return flow.error();
    auto o = (*flow)->open();
    if (!o)
        return fail(o.error());
    BlockDevice &src = *o->device;

    // Temporary name + rename: an interrupted backup never looks like a complete image.
    fs::path tmp = path;
    tmp += ".partial";
    {
        std::ofstream create(tmp, std::ios::binary | std::ios::trunc);
        if (!create)
            return fail(tmp.string(), std::make_error_code(std::errc::io_error));
    }
    auto cleanup = [&] {
        std::error_code ignored;
        fs::remove(tmp, ignored);
    };
    fs::resize_file(tmp, src.size_bytes(), ec);
    if (ec) {
        cleanup();
        return fail("create image", ec);
    }
    int rc = 0;
    {
        auto img = open_image(tmp, Access::ReadWrite, src.sector_size());
        if (!img)
            rc = fail("open image", img.error());
        else if (auto r = copy(src, **img, src.sector_count(), "reading"); !r)
            rc = fail("backup", r.error());
    }
    if (rc != 0) {
        cleanup();
        return rc;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        cleanup();
        return fail("rename", ec);
    }
    out("Wrote {} to {}\nLocator: {}\n", human(src.size_bytes()), path.string(), o->locator);
    return 0;
}

int cmd_restore(const std::string &target, const fs::path &path, const Options &opt) {
    auto flow = complete(target, Access::ReadWrite, opt); // read-only discovery, all checks for writing
    if (!flow)
        return flow.error();
    const ro::Preview p = (*flow)->preview();
    {
        auto img = open_image(path, Access::ReadOnly, p.sector_size);
        if (!img)
            return fail("open image (its size must be a multiple of the target's sector size)", img.error());
        if ((*img)->size_bytes() > std::uint64_t{p.sector_size} * p.sector_count)
            return fail("image is larger than the target", std::make_error_code(std::errc::file_too_large));
        print_preview(p);
        out("\nAbout to OVERWRITE the target above with {} ({}).\n", path.string(), human((*img)->size_bytes()));
    }
    if (!opt.yes) {
        auto answer = read_line("Type YES to continue: ");
        if (!answer || *answer != "YES") {
            out("Aborted.\n");
            return 1;
        }
    }
    auto o = (*flow)->open(); // replays everything for writing: identity and geometry re-checked
    if (!o)
        return fail(o.error());
    auto img = open_image(path, Access::ReadOnly, p.sector_size);
    if (!img)
        return fail("open image", img.error());
    const std::uint64_t sectors = (*img)->sector_count();
    if (auto r = copy(**img, *o->device, sectors, "writing"); !r)
        return fail("write (target contents are now incomplete)", r.error());
    auto same = verify(**img, *o->device, sectors);
    if (!same)
        return fail("verify", same.error());
    out("{}", *same ? "Verified OK.\n" : "VERIFY FAILED: contents differ from the image!\n");
    return *same ? 0 : 1;
}

int usage() {
    std::fputs("usage:\n"
               "  rawtool list\n"
               "  rawtool modes\n"
               "  rawtool inspect <target>\n"
               "  rawtool info    <target>\n"
               "  rawtool dump    <target> [lba]\n"
               "  rawtool backup  <target> <out.img>\n"
               "  rawtool restore <target> <in.img>\n"
               "  rawtool probe-write <complete-locator> --plan <file> --journal <new-directory>\n"
               "  rawtool recover-write --journal <directory>\n"
               "  <target>: a mode id (rawtool modes) or a locator printed by an earlier run\n"
               "  options: --no-prompt, --yes (restore only), --max-device-bytes N (default 9000000000, 0 unlimited)\n",
               stderr);
    return 2;
}

} // namespace rawtool

using namespace rawtool;

int main(int argc, char **argv) {
    Options opt;
    std::optional<fs::path> plan, journal;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--no-prompt")
            opt.prompt = false;
        else if (a == "--yes")
            opt.yes = true;
        else if (a == "--max-device-bytes") {
            if (++i == argc || !parse_u64(argv[i], opt.max_device_bytes))
                return usage();
        } else if (a == "--plan") {
            if (++i == argc || plan)
                return usage();
            plan = fs::path(argv[i]);
        } else if (a == "--journal") {
            if (++i == argc || journal)
                return usage();
            journal = fs::path(argv[i]);
        } else if (a.starts_with("--"))
            return usage();
        else
            args.emplace_back(a);
    }
    if (args.empty())
        return usage();
    const std::string &cmd = args[0];
    if (cmd == "probe-write" && args.size() == 2 && plan && journal)
        return cmd_probe_write(args[1], *plan, *journal, opt);
    if (cmd == "recover-write" && args.size() == 1 && !plan && journal)
        return cmd_recover_write(*journal, opt);
    if (plan || journal)
        return usage();
    if (cmd == "list" && args.size() == 1)
        return cmd_list(opt);
    if (cmd == "modes" && args.size() == 1)
        return cmd_modes();
    if (cmd == "inspect" && args.size() == 2)
        return cmd_inspect(args[1], opt);
    if (cmd == "info" && args.size() == 2)
        return cmd_info(args[1], opt);
    if (cmd == "dump" && (args.size() == 2 || args.size() == 3)) {
        std::uint64_t lba = 0;
        if (args.size() == 3 && !parse_u64(args[2], lba))
            return usage();
        return cmd_dump(args[1], lba, opt);
    }
    if (cmd == "backup" && args.size() == 3)
        return cmd_backup(args[1], fs::path(args[2]), opt);
    if (cmd == "restore" && args.size() == 3)
        return cmd_restore(args[1], fs::path(args[2]), opt);
    return usage();
}
