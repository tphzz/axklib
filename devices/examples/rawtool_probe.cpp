#include "rawdisk/open/locator.hpp"
#include "rawdisk/probe.hpp"
#include "rawtool_common.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace rawtool {
namespace ro = rawdisk::open;
namespace rp = rawdisk::probe;
using rawdisk::Access;
namespace {
#ifdef _WIN32
std::atomic_bool interrupted{false};
BOOL WINAPI console_signal(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        return FALSE;
    interrupted.store(true, std::memory_order_relaxed);
    return TRUE;
}
class Signals {
  public:
    Signals() {
        interrupted.store(false, std::memory_order_relaxed);
        ready_ = SetConsoleCtrlHandler(console_signal, TRUE) != FALSE;
    }
    ~Signals() {
        if (ready_)
            SetConsoleCtrlHandler(console_signal, FALSE);
    }
    bool ready() const { return ready_; }

  private:
    bool ready_ = false;
};
bool interactive() { return _isatty(_fileno(stdin)) != 0; }
bool cancelled() { return interrupted.load(std::memory_order_relaxed); }
#else
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
class Signals {
  public:
    Signals() {
        interrupted = 0;
        struct sigaction action{};
        action.sa_handler = signal_handler;
        sigemptyset(&action.sa_mask);
        int_ready_ = sigaction(SIGINT, &action, &old_int_) == 0;
        term_ready_ = sigaction(SIGTERM, &action, &old_term_) == 0;
    }
    ~Signals() {
        if (int_ready_)
            sigaction(SIGINT, &old_int_, nullptr);
        if (term_ready_)
            sigaction(SIGTERM, &old_term_, nullptr);
    }
    bool ready() const { return int_ready_ && term_ready_; }

  private:
    struct sigaction old_int_{}, old_term_{};
    bool int_ready_ = false, term_ready_ = false;
};
bool interactive() { return isatty(fileno(stdin)) != 0; }
bool cancelled() { return interrupted != 0; }
#endif

int error(std::string_view text) {
    return fail("write probe", rawdisk::make_error_code(rawdisk::Errc::invalid_argument), text);
}

bool file_mode(std::string_view id) { return id == "image" || id == "scsi2sd-v6-image" || id == "scsi2sd-xml-image"; }

std::expected<std::unique_ptr<ro::Flow>, int> prepare(const std::string &target, const Options &opt) {
    if (opt.yes)
        return std::unexpected(error("--yes is not allowed for probe-write or recover-write"));
    const auto locator = ro::parse_locator(target);
    if (!locator || locator->answers.empty())
        return std::unexpected(error("a complete locator is required; use info first"));
    if (!file_mode(locator->mode) && (!opt.prompt || !interactive()))
        return std::unexpected(error("physical probes require an interactive console and identity confirmation"));
    Options fixed = opt;
    fixed.prompt = false; // never ask for another device or silently substitute a selection
    return complete(target, Access::ReadWrite, fixed);
}

rp::Identity identity(const ro::Preview &p) { return {p.locator, p.usb, p.offset_bytes}; }

bool confirm(const ro::Preview &p, const rp::Plan &plan, const Options &opt, std::string_view action) {
    print_preview(p);
    out("\n{}: target LBA {}, one {}-byte sector.\nRoot byte range: [{}, {}).\n", action, plan.lba, plan.sector_size,
        p.offset_bytes + plan.lba * plan.sector_size, p.offset_bytes + (plan.lba + 1) * plan.sector_size);
    out("The journal supports recovery, but cannot guarantee rollback after power loss or forced termination.\n");
    if (!p.usb && !opt.prompt)
        return true; // automated fixture-image tests only
    const std::string token =
        p.usb ? std::format("WRITE {} LBA {}", p.usb->path, plan.lba) : std::format("WRITE IMAGE LBA {}", plan.lba);
    const auto answer = read_line(std::format("Type exactly {}: ", token));
    return answer && *answer == token && !cancelled();
}

int print_report(const rp::Report &r, const std::filesystem::path &dir) {
    out("Write attempted: {}\nPattern verified: {}\nRestored and guards verified: {}\nJournal: {}\n",
        r.write_attempted ? "yes" : "no", r.pattern_verified ? "yes" : "no", r.restored ? "yes" : "no", dir.string());
    if (r.primary_error)
        fail("probe operation", r.primary_error);
    if (r.restoration_error)
        fail("RESTORATION FAILED - keep the device connected and retain the journal", r.restoration_error);
    if (r.write_attempted && !r.restored)
        out("Recovery is required. Do not guess a new device or resume writing; investigate before recover-write.\n");
    return r.ok() ? 0 : 1;
}
} // namespace

int cmd_probe_write(const std::string &target, const std::filesystem::path &file, const std::filesystem::path &journal,
                    const Options &opt) {
    const Signals signals;
    if (!signals.ready())
        return error("cannot install cancellation handlers; no target opened");
    auto plan = rp::load_plan(file);
    if (!plan)
        return fail("plan", plan.error());
    auto flow = prepare(target, opt);
    if (!flow)
        return flow.error();
    const auto preview = (*flow)->preview();
    {
        auto ro_device = (*flow)->read_only();
        if (!ro_device)
            return fail(ro_device.error());
        if (auto r = rp::preflight(*ro_device->device, *plan, cancelled); !r)
            return fail("read-only preflight", r.error());
    }
    if (!confirm(preview, *plan, opt, "Reversible write probe"))
        return error("aborted; no write attempted");
    if (cancelled())
        return error("cancelled; no write attempted");
    auto opened = (*flow)->open();
    if (!opened)
        return fail(opened.error());
    if (identity(preview) != rp::Identity{opened->locator, opened->usb, opened->offset_bytes})
        return error("target identity changed; no write attempted");
    out("Keep the device connected until restoration completes. Ctrl+C requests restoration before exit.\n");
    return print_report(rp::run(*opened->device, *plan, identity(preview), journal, cancelled), journal);
}

int cmd_recover_write(const std::filesystem::path &directory, const Options &opt) {
    const Signals signals;
    if (!signals.ready())
        return error("cannot install cancellation handlers; no target opened");
    auto journal = rp::load_journal(directory);
    if (!journal)
        return fail("journal", journal.error());
    auto flow = prepare(journal->identity.locator, opt);
    if (!flow)
        return flow.error();
    const auto preview = (*flow)->preview();
    if (identity(preview) != journal->identity || preview.sector_size != journal->plan.sector_size ||
        preview.sector_count != journal->plan.sector_count)
        return error("journal identity/geometry does not match this target");
    {
        auto ro_device = (*flow)->read_only();
        if (!ro_device)
            return fail(ro_device.error());
        const auto checked = rp::recover(*ro_device->device, *journal, cancelled);
        if (checked.ok()) {
            out("Already restored; guards match. No write required.\n");
            return 0;
        }
        if (checked.primary_error != rawdisk::Errc::read_only_handle || checked.restoration_error)
            return print_report(checked, directory);
    }
    if (!confirm(preview, journal->plan, opt, "Restore journaled sector"))
        return error("aborted; no write attempted");
    auto opened = (*flow)->open();
    if (!opened)
        return fail(opened.error());
    if (rp::Identity{opened->locator, opened->usb, opened->offset_bytes} != journal->identity)
        return error("target identity changed; no write attempted");
    return print_report(rp::recover(*opened->device, *journal, cancelled), directory);
}
} // namespace rawtool
