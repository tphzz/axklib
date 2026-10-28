#include "rawdisk/block_device.hpp"
#include "detail.hpp"
#if defined(_WIN32)
#include "win32_identity.hpp"
#endif

#include <algorithm>
#include <cstring>
#include <optional>

namespace rawdisk {
namespace {

constexpr std::size_t kMaxChunk = std::size_t{1} << 20; // 1 MiB per OS call

bool is_aligned(const void *p) noexcept { return reinterpret_cast<std::uintptr_t>(p) % kIoAlignment == 0; }

Result<void> check_range(std::uint32_t ss, std::uint64_t count, std::uint64_t lba, std::size_t bytes) {
    if (ss == 0 || bytes % ss != 0)
        return detail::fail(Errc::bad_length);
    const std::uint64_t n = bytes / ss;
    if (lba > count || n > count - lba)
        return detail::fail(Errc::out_of_range);
    return {};
}

std::size_t chunk_size(std::uint32_t ss) noexcept { return std::max<std::size_t>(ss, kMaxChunk / ss * ss); }

// Rejects readers that report more capacity than they can deliver (a real failure mode of
// cheap USB CF readers) before anything is written.
Result<void> check_capacity(BlockDevice &dev) {
    const std::uint64_t count = dev.sector_count();
    if (count == 0)
        return detail::fail(Errc::capacity_unreadable);
    AlignedBuffer buf(dev.sector_size());
    if (auto r = dev.read(0, buf.span()); !r)
        return r;
    if (auto r = dev.read(count - 1, buf.span()); !r)
        return detail::fail(Errc::capacity_unreadable);
    return {};
}

} // namespace

Result<void> BlockDevice::read(std::uint64_t lba, std::span<std::byte> out) noexcept {
    return detail::guard([&]() -> Result<void> {
        const std::uint32_t ss = sector_size();
        if (auto r = check_range(ss, sector_count(), lba, out.size()); !r)
            return r;
        if (out.empty())
            return {};

        std::lock_guard lock(mutex_);
        const std::uint64_t base = lba * ss;
        const std::size_t chunk = chunk_size(ss);
        const bool direct = is_aligned(out.data());
        std::optional<AlignedBuffer> bounce;
        if (!direct)
            bounce.emplace(std::min(chunk, out.size()));

        for (std::size_t done = 0; done < out.size();) {
            const std::size_t n = std::min(chunk, out.size() - done);
            const auto dst = out.subspan(done, n);
            if (direct) {
                if (auto r = read_raw(base + done, dst); !r)
                    return r;
            } else {
                const auto tmp = bounce->span().first(n);
                if (auto r = read_raw(base + done, tmp); !r)
                    return r;
                std::memcpy(dst.data(), tmp.data(), n);
            }
            done += n;
        }
        return {};
    });
}

Result<void> BlockDevice::write(std::uint64_t lba, std::span<const std::byte> in) noexcept {
    return detail::guard([&]() -> Result<void> {
        if (!writable())
            return detail::fail(Errc::read_only_handle);
        const std::uint32_t ss = sector_size();
        if (auto r = check_range(ss, sector_count(), lba, in.size()); !r)
            return r;
        if (in.empty())
            return {};
        if (auto r = check_write(lba, in.size() / ss); !r)
            return r;

        std::lock_guard lock(mutex_);
        const std::uint64_t base = lba * ss;
        const std::size_t chunk = chunk_size(ss);
        const bool direct = is_aligned(in.data());
        std::optional<AlignedBuffer> bounce;
        if (!direct)
            bounce.emplace(std::min(chunk, in.size()));

        for (std::size_t done = 0; done < in.size();) {
            const std::size_t n = std::min(chunk, in.size() - done);
            const auto src = in.subspan(done, n);
            if (direct) {
                if (auto r = write_raw(base + done, src); !r)
                    return r;
            } else {
                const auto tmp = bounce->span().first(n);
                std::memcpy(tmp.data(), src.data(), n);
                if (auto r = write_raw(base + done, tmp); !r)
                    return r;
            }
            done += n;
        }
        return {};
    });
}

Result<void> BlockDevice::flush() noexcept {
    return detail::guard([&]() -> Result<void> {
        if (!writable())
            return {};
        std::lock_guard lock(mutex_);
        return flush_raw();
    });
}

Result<void> BlockDevice::verify_binding() noexcept {
    return detail::guard([&]() -> Result<void> {
        const std::lock_guard lock{mutex_};
        return verify_binding_raw();
    });
}

Result<void> BlockDevice::verify_binding_raw() { return detail::fail(Errc::not_a_usb_disk); }
Result<void> BlockDevice::verify_non_dismounting_claim(const DeviceInfo &expected) noexcept {
    return detail::guard([&] {
        const std::lock_guard lock{mutex_};
        return verify_non_dismounting_claim_raw(expected);
    });
}
Result<void> BlockDevice::verify_non_dismounting_claim_raw(const DeviceInfo &) {
    return detail::fail(Errc::device_busy);
}

Result<std::vector<DeviceInfo>> list_usb_devices() noexcept {
    return detail::guard([] { return detail::list_platform(); });
}

Result<std::unique_ptr<BlockDevice>> open_device(const DeviceInfo &expected, Access access,
                                                 std::shared_ptr<AuthorizationSession> authorization) noexcept {
    return detail::guard([&]() -> Result<std::unique_ptr<BlockDevice>> {
        if (access != Access::ReadOnly && access != Access::ReadWrite && access != Access::ReadWriteUnmounted)
            return detail::fail(Errc::invalid_argument);
        auto devices = detail::list_platform();
        if (!devices)
            return detail::fail(devices.error());

        const auto it =
            std::ranges::find_if(*devices, [&](const DeviceInfo &d) { return detail::same_identity(d, expected); });
        if (it == devices->end()) {
#if defined(_WIN32)
            const auto current = std::ranges::find(*devices, expected.path, &DeviceInfo::path);
            if (current != devices->end())
                return detail::fail(detail::win32_identity_mismatch(*current, expected));
#endif
            return detail::fail(Errc::device_changed);
        }

        const auto check_format = [&](BlockDevice *retained_claim) -> Result<void> {
            if (access != Access::ReadWriteUnmounted)
                return {};
            auto inventory = inspect_usb_device(*it, retained_claim);
            if (!inventory)
                return detail::fail(inventory.error());
            if (!inventory->complete || !inventory->non_dismounting_claim_supported ||
                inventory->members.size() != 1U || !inventory->mounted.empty())
                return detail::fail(Errc::device_busy);
            return {};
        };
        if (auto inspected = check_format(nullptr); !inspected)
            return detail::fail(inspected.error());

        if (!authorization) {
            auto created = create_authorization_session();
            if (!created)
                return detail::fail(created.error());
            authorization = std::move(*created);
        }
        auto dev = detail::open_platform(*it, access, *authorization);
        if (!dev)
            return dev;
        if (!*dev)
            return detail::fail(Errc::internal_error);
        if (auto inspected = check_format(dev->get()); !inspected)
            return detail::fail(inspected.error());
        if (auto r = check_capacity(**dev); !r)
            return detail::fail(r.error());
        return dev;
    });
}

Result<std::unique_ptr<BlockDevice>> open_image(const std::filesystem::path &path, Access access,
                                                std::uint32_t sector_size) noexcept {
    return detail::guard([&]() -> Result<std::unique_ptr<BlockDevice>> {
        if (!detail::valid_sector_size(sector_size) || path.empty())
            return detail::fail(Errc::invalid_argument);
        return detail::open_image_platform(path, access, sector_size);
    });
}

} // namespace rawdisk
