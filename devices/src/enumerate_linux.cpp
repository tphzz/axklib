#include "detail.hpp"
#include "rawdisk/block_device.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace rawdisk {
namespace {

namespace fs = std::filesystem;

std::mutex g_root_mutex;
fs::path g_root = "/sys";

std::string read_line(const fs::path &p) {
    std::ifstream f(p);
    std::string s;
    if (f)
        std::getline(f, s);
    return detail::trim(s);
}

std::uint64_t read_u64(const fs::path &p) {
    const std::string s = read_line(p);
    std::uint64_t v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size())
        return 0;
    return v;
}

bool path_exists(const fs::path &p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

} // namespace

void detail::set_sysfs_root(fs::path root) {
    std::lock_guard lock(g_root_mutex);
    g_root = std::move(root);
}

fs::path detail::sysfs_root() {
    std::lock_guard lock(g_root_mutex);
    return g_root;
}

std::optional<DeviceInfo> detail::linux_describe_block(const fs::path &entry) {
    std::error_code ec;
    const fs::path dir = fs::canonical(entry, ec); // /sys/devices/.../usbN/N-M/.../block/sdb
    if (ec)
        return std::nullopt;
    if (path_exists(dir / "partition"))
        return std::nullopt; // only whole disks are ever offered

    // SCSI peripheral type 0 (direct access) or 14 (RBC); excludes CD-ROMs, tapes, etc.
    const std::string type = read_line(dir / "device/type");
    if (type != "0" && type != "14")
        return std::nullopt;

    // Nearest ancestor that is a USB device (has idVendor/idProduct). None: not USB.
    fs::path usb;
    for (fs::path p = dir.parent_path(); p.has_relative_path(); p = p.parent_path()) {
        if (path_exists(p / "idVendor") && path_exists(p / "idProduct")) {
            usb = p;
            break;
        }
    }
    if (usb.empty())
        return std::nullopt;

    const std::string name = dir.filename().string();
    if (name.empty() ||
        !std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }))
        return std::nullopt;

    DeviceInfo d;
    d.path = "/dev/" + name;
    d.model = join_model(read_line(dir / "device/vendor"), read_line(dir / "device/model"));
    d.serial = read_line(usb / "serial");
    d.carrier_id = usb.string();
    d.connection_id = read_line(dir / "diskseq");
    const std::uint64_t sectors512 = read_u64(dir / "size"); // sysfs "size" is always in 512-byte units
    d.sector_size = static_cast<std::uint32_t>(read_u64(dir / "queue/logical_block_size"));
    d.removable = read_u64(dir / "removable") == 1;

    if (sectors512 == 0 || sectors512 > (UINT64_MAX / 512) || !valid_sector_size(d.sector_size))
        return std::nullopt; // no card in the reader, or nonsense values
    d.size_bytes = sectors512 * 512;
    if (d.size_bytes % d.sector_size != 0)
        return std::nullopt;
    return d;
}

Result<std::vector<DeviceInfo>> detail::list_platform() {
    std::error_code ec;
    fs::directory_iterator it(sysfs_root() / "block", ec);
    if (ec)
        return fail(ec);

    std::vector<DeviceInfo> out;
    for (const fs::directory_iterator end{}; it != end; it.increment(ec)) {
        if (auto d = linux_describe_block(it->path()))
            out.push_back(std::move(*d));
    }
    if (ec)
        return fail(ec);
    std::ranges::sort(out, {}, &DeviceInfo::path);
    return out;
}

} // namespace rawdisk
