#include "rawdisk/probe.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

using namespace rawdisk;
namespace rp = rawdisk::probe;

#ifdef RAWDISK_TEST_WRAP_FSYNC
static int journal_syncs = 0, fail_journal_sync = 0;
extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
    if (fail_journal_sync != 0 && ++journal_syncs == fail_journal_sync) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}
#endif

// Only memory and temporary regular files are used. Never enumerates physical devices.
class FaultDevice final : public BlockDevice {
  public:
    std::vector<std::byte> bytes = std::vector<std::byte>(512 * 128, std::byte{0});
    int reads = 0, writes = 0, flushes = 0;
    int fail_read = 0, fail_write = 0, fail_flush = 0;
    std::function<void()> on_write;
    std::function<void()> on_read;
    std::vector<std::vector<std::byte>> written_sectors;
    std::uint32_t sector_size() const noexcept override { return 512; }
    std::uint64_t sector_count() const noexcept override { return 128; }
    bool writable() const noexcept override { return true; }

  private:
    Result<void> read_raw(std::uint64_t offset, std::span<std::byte> out) override {
        if (++reads == fail_read)
            return std::unexpected(std::make_error_code(std::errc::io_error));
        if (on_read)
            on_read();
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return {};
    }
    Result<void> write_raw(std::uint64_t offset, std::span<const std::byte> in) override {
        ++writes;
        written_sectors.emplace_back(in.begin(), in.end());
        // Simulate an OS error AFTER some/all data reached the device.
        std::copy(in.begin(), in.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        if (on_write)
            on_write();
        if (writes == fail_write)
            return std::unexpected(std::make_error_code(std::errc::io_error));
        return {};
    }
    Result<void> flush_raw() override {
        if (++flushes == fail_flush)
            return std::unexpected(std::make_error_code(std::errc::io_error));
        return {};
    }
};

static rp::Plan plan() {
    return {.sector_size = 512,
            .sector_count = 128,
            .lba = 64,
            .original = std::vector<std::byte>(512),
            .guards = {{0, std::vector<std::byte>(512)}, {127, std::vector<std::byte>(512)}}};
}

static void test_probe_and_faults() {
    for (int fault = 0; fault < 6; ++fault) {
        FaultDevice d;
        if (fault == 1)
            d.fail_write = 1;
        if (fault == 2)
            d.fail_flush = 1;
        if (fault == 3)
            d.fail_read = 10; // final pre-write recheck
        if (fault == 4)
            d.fail_read = 20; // pattern read-back
        bool cancelled = false;
        if (fault == 5)
            d.on_write = [&] { cancelled = true; };
        const auto dir = test_dir() / std::format("journal-{}", fault);
        const auto before = d.bytes;
        auto r = rp::run(d, plan(), {.locator = "image?image.path=fixture&image.sector_size=512"}, dir,
                         [&] { return cancelled; });
        CHECK(r.restoration_error == std::error_code{});
        CHECK(d.bytes == before);
        if (fault == 0)
            CHECK(r.ok() && r.pattern_verified && r.restored && d.writes == 2);
        else
            CHECK(!r.ok());
        if (r.write_attempted)
            CHECK(r.restored && d.writes == 2);
        if (std::filesystem::exists(dir))
            CHECK(!rp::run(d, plan(), {}, dir).ok()); // existing journal must never be reused
    }
}

static void test_refusals_and_recovery() {
    FaultDevice d;
    auto p = plan();
    d.bytes[0] = std::byte{7};
    CHECK(!rp::run(d, p, {}, test_dir() / "stale").ok());
    CHECK(d.writes == 0);
    d.bytes[0] = std::byte{0};
    CHECK(!rp::run(d, p, {}, test_dir() / "missing" / "journal").ok());
    CHECK(d.writes == 0);
    const auto dir = test_dir() / "recovery";
    CHECK(rp::run(d, p, {}, dir).ok());
    auto j = rp::load_journal(dir);
    CHECK(j.has_value());
    if (!j)
        return;
    CHECK(rp::recover(d, *j).ok() && d.writes == 2); // already restored, no write
    d.bytes[64 * 512] ^= std::byte{1};
    CHECK(rp::recover(d, *j).ok() && d.writes == 3);
    d.bytes[64 * 512] = std::byte{5};
    CHECK(!rp::recover(d, *j).ok() && d.writes == 3); // unexpected contents
    d.bytes[64 * 512] = std::byte{1};
    d.bytes[0] = std::byte{7};
    CHECK(!rp::recover(d, *j).ok() && d.writes == 3); // changed context
    p.lba = std::numeric_limits<std::uint64_t>::max();
    CHECK(!rp::validate(p));
    p = plan();
    p.guards.push_back({64, std::vector<std::byte>(512)});
    CHECK(!rp::validate(p));
}

static void test_recovery_material_and_restore_failures() {
    for (int kind = 0; kind < 3; ++kind) {
        FaultDevice d;
        rp::Identity identity;
        if (kind == 0)
            identity.locator.assign(8193, 'a');
        if (kind == 1)
            identity.locator = std::string("bad\0locator", 11);
        if (kind == 2) {
            DeviceInfo info;
            info.serial.assign(8193, 'x');
            identity.usb = std::move(info);
        }
        CHECK(!rp::run(d, plan(), identity, test_dir() / std::format("invalid-identity-{}", kind)).ok());
        CHECK(d.writes == 0);
    }
    for (int fault = 0; fault < 3; ++fault) {
        FaultDevice d;
        if (fault == 0)
            d.fail_write = 2;
        if (fault == 1)
            d.fail_flush = 2;
        if (fault == 2)
            d.on_write = [&] {
                if (d.writes == 2)
                    d.fail_read = d.reads + 1;
            };
        const auto r = rp::run(d, plan(), {}, test_dir() / std::format("restore-fault-{}", fault));
        CHECK(r.write_attempted && r.pattern_verified && !r.restored && !r.ok());
        CHECK(!r.primary_error && r.restoration_error);
        CHECK(d.writes == 2); // no automatic reopen/retry following an uncertain restoration
    }
    FaultDevice d;
    const auto directory = test_dir() / "durable-before-write";
    d.on_write = [&] {
        CHECK(rp::load_journal(directory).has_value());
        CHECK(d.written_sectors.back().size() == 512);
    };
    CHECK(rp::run(d, plan(), {}, directory).ok());
    CHECK(d.written_sectors.size() == 2);
    if (d.written_sectors.size() == 2) {
        auto expected = plan().original;
        expected[0] ^= std::byte{1};
        CHECK(d.written_sectors[0] == expected && d.written_sectors[1] == plan().original);
    }
    FaultDevice unstable;
    unstable.on_read = [&] {
        if (unstable.reads == 2)
            unstable.bytes[64 * 512] = std::byte{1};
    };
    CHECK(!rp::preflight(unstable, plan()));
    CHECK(!rp::run(unstable, plan(), {}, test_dir() / "unstable").ok() && unstable.writes == 0);
}

static void test_plan_codec_and_large_offset() {
    auto p = plan();
    const auto file = test_dir() / "probe.plan";
    CHECK(rp::save_plan(file, p).has_value());
    CHECK(!rp::save_plan(file, p));
    auto loaded = rp::load_plan(file);
    CHECK(loaded && loaded->original == p.original && loaded->lba == p.lba && loaded->guards.size() == 2);
    {
        std::fstream f(file, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(40);
        f.put('x');
    }
    CHECK(!rp::load_plan(file)); // damaged plan checksum

    const auto image = make_card("large.img", 1);
#ifdef _WIN32
    // Keep the >4 GiB offset regression small on disk, including on Windows CI.
    const HANDLE handle =
        CreateFileW(image.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    DWORD returned = 0;
    const BOOL sparse = DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
    CHECK(sparse != FALSE);
    CHECK(CloseHandle(handle) != FALSE);
    if (sparse == FALSE)
        return;
#endif
    std::filesystem::resize_file(image, (std::uint64_t{1} << 32) + 8192);
    auto d = open_image(image, Access::ReadWrite, 4096);
    CHECK(d.has_value());
    if (!d)
        return;
    p = {.sector_size = 4096,
         .sector_count = (*d)->sector_count(),
         .lba = (std::uint64_t{1} << 20),
         .original = std::vector<std::byte>(4096)};
    CHECK(rp::run(**d, p, {}, test_dir() / "large-journal").ok());
}

static void test_journal_durability_failure() {
#ifdef RAWDISK_TEST_WRAP_FSYNC
    for (int sync = 1; sync <= 3; ++sync) { // journal file, its directory, directory's parent
        FaultDevice d;
        journal_syncs = 0;
        fail_journal_sync = sync;
        const auto r = rp::run(d, plan(), {}, test_dir() / std::format("journal-sync-fault-{}", sync));
        fail_journal_sync = 0;
        CHECK(!r.ok() && r.primary_error == std::errc::io_error);
        CHECK(!r.write_attempted && d.writes == 0);
    }
#endif
}

int main() {
    test_probe_and_faults();
    test_refusals_and_recovery();
    test_recovery_material_and_restore_failures();
    test_plan_codec_and_large_offset();
    test_journal_durability_failure();
    return finish_tests();
}
