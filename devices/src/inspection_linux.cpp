#include "detail.hpp"
#include "posix_fd.hpp"
#include "rawdisk/inspection.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace rawdisk::detail {
namespace {
std::string line(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::string value;
    if (input)
        std::getline(input, value);
    if (value.size() > 4096U)
        return {};
    return trim(value);
}
std::string mount_text(std::string_view text) {
    std::string result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\\' && index + 3U < text.size() &&
            std::ranges::all_of(text.substr(index + 1U, 3U), [](char c) { return c >= '0' && c <= '7'; })) {
            const auto value = (text[index + 1U] - '0') * 64 + (text[index + 2U] - '0') * 8 + text[index + 3U] - '0';
            result.push_back(static_cast<char>(value));
            index += 3U;
        } else {
            result.push_back(text[index]);
        }
    }
    return result;
}

Result<void> append_mounts(const std::filesystem::path &path, const std::set<std::string> &devices,
                           DeviceInventory &result, std::string_view scope = {}) {
    std::ifstream mounts{path};
    if (!mounts)
        return fail(Errc::device_busy);
    std::string text;
    std::size_t lines{};
    while (std::getline(mounts, text)) {
        if (++lines > 4096U || text.size() > 65536U)
            return fail(Errc::device_busy);
        std::istringstream fields{text};
        std::vector<std::string> values;
        for (std::string field; fields >> field;)
            values.push_back(std::move(field));
        const auto separator = std::ranges::find(values, "-");
        if (values.size() < 10U || separator == values.end() || values.end() - separator < 4)
            return fail(Errc::device_busy);
        if (devices.contains(values[2]))
            result.mounted.push_back(
                {mount_text(*(separator + 2)), std::string{scope} + mount_text(values[4]), *(separator + 1), {}});
    }
    if (!mounts.eof())
        return fail(Errc::device_busy);
    return {};
}

Result<void> namespace_mounts(const std::set<std::string> &devices, DeviceInventory &result) {
    if (auto loaded = append_mounts("/proc/self/mountinfo", devices, result); !loaded)
        return loaded;
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/ns/mnt", error);
    if (error)
        return fail(Errc::device_busy);
    std::set<std::filesystem::path> seen{self};
    std::size_t count{};
    for (std::filesystem::directory_iterator at{"/proc", error}, end; !error && at != end; at.increment(error)) {
        const auto name = at->path().filename().string();
        if (name.empty() || !std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        if (++count > 8192U)
            return fail(Errc::device_busy);
        std::error_code inspected;
        const auto identity = std::filesystem::read_symlink(at->path() / "ns/mnt", inspected);
        if (inspected) {
            if (!std::filesystem::exists(at->path(), inspected) && !inspected)
                continue;
            // Unprivileged callers cannot prove the state of hidden namespaces.
            result.notices.push_back("Mount namespaces are not fully inspectable; formatting is blocked.");
            return {};
        }
        if (!seen.insert(identity).second)
            continue;
        if (seen.size() > 64U)
            return fail(Errc::device_busy);
        if (auto loaded = append_mounts(at->path() / "mountinfo", devices, result, "[other mount namespace] "); !loaded)
            return loaded;
    }
    if (error)
        return fail(error);
    result.complete = true;
    return {};
}
} // namespace

Result<DevicePresentation> presentation_platform(const DeviceInfo &info) {
    const std::filesystem::path usb{info.carrier_id};
    return DevicePresentation{line(usb / "manufacturer"), line(usb / "product"), line(usb / "serial"),
                              line(usb / "idVendor"), line(usb / "idProduct")};
}

Result<DeviceInventory> inventory_platform(const DeviceInfo &info, bool retained_claim) {
    DeviceInventory result;
    const auto name = std::filesystem::path{info.path}.filename();
    const auto root = sysfs_root() / "block" / name;
    std::set<std::string> devices;
    const auto whole = line(root / "dev");
    if (whole.empty())
        return fail(Errc::device_identity_unavailable);
    devices.insert(whole);
    std::vector<std::filesystem::path> block_nodes{root};
    std::error_code error;
    for (std::filesystem::directory_iterator at{root, error}, end; !error && at != end; at.increment(error)) {
        if (std::filesystem::exists(at->path() / "partition", error)) {
            const auto device = line(at->path() / "dev");
            if (device.empty())
                return fail(Errc::device_identity_unavailable);
            devices.insert(device);
            block_nodes.push_back(at->path());
        }
        if (devices.size() > 129U)
            return fail(Errc::device_busy);
    }
    if (error)
        return fail(error);
    for (const auto &node : block_nodes) {
        std::filesystem::directory_iterator holders{node / "holders", error};
        if (error || holders != std::filesystem::directory_iterator{}) {
            result.notices.push_back(
                "Device or partition is used by another kernel storage layer; formatting is blocked.");
            return result;
        }
    }
    if (auto loaded = namespace_mounts(devices, result); !loaded)
        return fail(loaded.error());
    std::string text;
    std::ifstream swaps{"/proc/swaps"};
    if (!swaps || !std::getline(swaps, text))
        return fail(Errc::device_busy);
    while (std::getline(swaps, text)) {
        std::istringstream fields{text};
        std::string path;
        if (!(fields >> path))
            return fail(Errc::device_busy);
        path = mount_text(path);
        if (path == info.path || (path.starts_with(info.path) && path.size() > info.path.size() &&
                                  path[info.path.size()] >= '0' && path[info.path.size()] <= '9'))
            result.mounted.push_back({path, "[active swap]", "swap", {}});
    }
    if (!swaps.eof())
        return fail(Errc::device_busy);
    if (!result.complete)
        return result;
    result.complete = false;
    if (!result.mounted.empty())
        return result;
    // Mountinfo is not exhaustive across hidden namespaces, and some multi-device
    // filesystems use anonymous device numbers. Require the kernel's whole-disk
    // exclusive claim as well, without requesting write access during inspection.
    if (!retained_claim) {
        const PosixFd claim{::open(info.path.c_str(), O_RDONLY | O_EXCL | O_CLOEXEC | O_NOCTTY)};
        struct stat opened{};
        if (!claim.valid() || ::fstat(claim.get(), &opened) != 0 || !S_ISBLK(opened.st_mode) ||
            std::to_string(major(opened.st_rdev)) + ':' + std::to_string(minor(opened.st_rdev)) != whole) {
            result.notices.push_back("A read-only exclusive kernel claim could not establish that the disk is "
                                     "unmounted; formatting is blocked.");
            return result;
        }
    }
    result.complete = true;
    result.non_dismounting_claim_supported = true;
    return result;
}
} // namespace rawdisk::detail
