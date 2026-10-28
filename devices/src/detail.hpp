#pragma once

#include "rawdisk/block_device.hpp"
#include "rawdisk/inspection.hpp"

#include <expected>
#include <filesystem>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace rawdisk::detail {

// ---- implemented once per platform -------------------------------------------------------
Result<std::vector<DeviceInfo>> list_platform();
Result<DevicePresentation> presentation_platform(const DeviceInfo &info);
Result<DeviceInventory> inventory_platform(const DeviceInfo &info, bool retained_claim);
// `info` comes from a fresh enumeration. Must verify identity on the opened handle.
Result<std::unique_ptr<BlockDevice>> open_platform(const DeviceInfo &info, Access access,
                                                   AuthorizationSession &authorization);
Result<std::unique_ptr<BlockDevice>> open_image_platform(const std::filesystem::path &path, Access access,
                                                         std::uint32_t sector_size);

// ---- shared helpers ----------------------------------------------------------------------
inline std::unexpected<std::error_code> fail(std::error_code ec) noexcept { return std::unexpected(ec); }
inline std::unexpected<std::error_code> fail(Errc e) noexcept { return std::unexpected(make_error_code(e)); }
inline std::error_code errno_code(int e) noexcept { return {e, std::generic_category()}; }

inline bool valid_sector_size(std::uint64_t ss) noexcept { return ss >= 512 && ss <= 65536 && (ss & (ss - 1)) == 0; }

inline bool same_identity(const DeviceInfo &a, const DeviceInfo &b) noexcept {
    return a.path == b.path && a.model == b.model && a.serial == b.serial && a.size_bytes == b.size_bytes &&
           a.sector_size == b.sector_size && a.carrier_id == b.carrier_id && a.connection_id == b.connection_id;
}

// Runs f and converts any escaping exception into an error, so nothing crosses the API.
template <class F> auto guard(F &&f) noexcept -> std::invoke_result_t<F &> {
    try {
        return f();
    } catch (const std::bad_alloc &) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::system_error &e) { // includes std::filesystem::filesystem_error
        return std::unexpected(e.code());
    } catch (...) {
        return std::unexpected(make_error_code(Errc::internal_error));
    }
}

inline std::string trim(std::string_view s) {
    constexpr std::string_view ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos)
        return {};
    return std::string(s.substr(b, s.find_last_not_of(ws) - b + 1));
}

inline std::string join_model(std::string_view vendor, std::string_view product) {
    std::string v = trim(vendor), p = trim(product);
    if (v.empty())
        return p;
    if (p.empty())
        return v;
    return v + " " + p;
}

#if defined(__linux__)
// Exposed for tests: describe one /sys/block (or /sys/dev/block) entry, or nullopt if it is
// not a USB whole disk with media. The sysfs root is configurable for tests only.
std::optional<DeviceInfo> linux_describe_block(const std::filesystem::path &entry);
void set_sysfs_root(std::filesystem::path root);
std::filesystem::path sysfs_root();
#endif

} // namespace rawdisk::detail
