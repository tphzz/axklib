#include "axklib/application/device_sessions.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include "axklib/application/secure_random.hpp"
#include "device_sessions_internal.hpp"

#if defined(AXK_BUILD_DEVICES)
#include "device_candidates.hpp"
#include "rawdisk/block_device.hpp"
#include "rawdisk/ownership.hpp"
#include "rawdisk/scsi2sd/v6.hpp"
#include "rawdisk/scsi2sd/xml.hpp"
#endif

namespace axk::app {
namespace {

#if defined(AXK_BUILD_DEVICES)
Error device_error(const std::error_code &error) { return {"device_access_failed", error.message()}; }

class NativeTransport final : public DeviceTransport {
  public:
    NativeTransport(rawdisk::DeviceInfo info, std::shared_ptr<void> lease, std::unique_ptr<rawdisk::BlockDevice> device,
                    std::vector<rawdisk::Range> protected_ranges,
                    std::map<std::uint64_t, std::vector<std::byte>> layout_guards)
        : info_(std::move(info)), lease_(std::move(lease)), device_(std::move(device)),
          protected_ranges_(std::move(protected_ranges)), layout_guards_(std::move(layout_guards)) {}

    std::uint64_t size() const noexcept override { return info_.size_bytes; }
    std::uint32_t sector_size() const noexcept override { return info_.sector_size; }
    std::string carrier_identity() const override { return info_.carrier_id; }
    std::string media_identity() const override {
        auto result = std::format("{}:{}:{}:{}", info_.model, info_.serial, info_.size_bytes, info_.sector_size);
        for (const auto &[offset, bytes] : layout_guards_)
            result += std::format(":{}:{}", offset, *device_detail::digest(bytes));
        return result;
    }
    Result<void> verify_bound() const override {
        const std::scoped_lock lock{mutex_};
        return verify_locked();
    }
    Result<void> authorize_writes() override {
        const std::scoped_lock lock{mutex_};
        if (auto checked = verify_locked(); !checked)
            return checked;
        if (device_->writable())
            return {};
        auto privilege = rawdisk::hold_privileges();
        device_.reset();
        auto opened = rawdisk::open_device(info_, rawdisk::Access::ReadWrite);
        if (!opened) {
            invalidated_ = true;
            return std::unexpected(device_error(opened.error()));
        }
        device_ = std::move(*opened);
        return verify_locked();
    }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override {
        const std::scoped_lock lock{mutex_};
        if (invalidated_ || !device_)
            return std::unexpected(axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io,
                                                   "device session is disconnected"));
        if (offset > size() || bytes.size() > size() - offset)
            return std::unexpected(axk::make_error(axk::ErrorCode::out_of_bounds, axk::ErrorCategory::io,
                                                   "device read is outside target"));
        const auto sector = static_cast<std::uint64_t>(sector_size());
        while (!bytes.empty()) {
            const auto within = static_cast<std::size_t>(offset % sector);
            const auto count = std::min<std::size_t>(bytes.size(), 1024U * 1024U - within);
            const auto total = ((within + count + sector - 1U) / sector) * sector;
            std::vector<std::byte> buffer(static_cast<std::size_t>(total));
            if (auto read = device_->read(offset / sector, buffer); !read) {
                invalidated_ = true;
                return std::unexpected(
                    axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io, read.error().message()));
            }
            std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(within), count, bytes.begin());
            bytes = bytes.subspan(count);
            offset += count;
        }
        return {};
    }
    Result<void> write_exact_at(std::uint64_t offset, std::span<const std::byte> bytes) override {
        const std::scoped_lock lock{mutex_};
        if (invalidated_ || !device_)
            return std::unexpected(Error{"device_disconnected", "device session is disconnected"});
        if (offset % sector_size() != 0U || bytes.size() % sector_size() != 0U)
            return std::unexpected(Error{"device_write_alignment", "device writes require complete physical sectors"});
        if (offset > size() || bytes.size() > size() - offset)
            return std::unexpected(Error{"device_write_range", "device write is outside its carrier"});
        const rawdisk::Range range{offset / sector_size(), (offset + bytes.size()) / sector_size()};
        if (std::ranges::any_of(protected_ranges_,
                                [&](const auto &protected_range) { return protected_range.overlaps(range); }))
            return std::unexpected(Error{"device_write_protected", "write overlaps SCSI2SD configuration sectors"});
        if (auto written = device_->write(offset / sector_size(), bytes); !written) {
            invalidated_ = true;
            return std::unexpected(device_error(written.error()));
        }
        return {};
    }
    Result<void> flush() override {
        const std::scoped_lock lock{mutex_};
        if (invalidated_ || !device_)
            return std::unexpected(Error{"device_disconnected", "device session is disconnected"});
        if (auto flushed = device_->flush(); !flushed)
            return std::unexpected(device_error(flushed.error()));
        return {};
    }

  private:
    Result<void> verify_locked() const {
        if (invalidated_ || !device_)
            return std::unexpected(Error{"device_disconnected", "device session is disconnected; select it again"});
        auto devices = rawdisk::list_usb_devices();
        if (!devices || std::ranges::find(*devices, info_) == devices->end()) {
            invalidated_ = true;
            return std::unexpected(Error{"device_changed", "device connection identity changed; select it again"});
        }
        for (const auto &[lba, expected] : layout_guards_) {
            std::vector<std::byte> current(expected.size());
            if (!device_->read(lba, current) || current != expected) {
                invalidated_ = true;
                return std::unexpected(Error{"device_changed", "device layout changed; select it again"});
            }
        }
        return {};
    }
    rawdisk::DeviceInfo info_;
    std::shared_ptr<void> lease_;
    mutable std::unique_ptr<rawdisk::BlockDevice> device_;
    std::vector<rawdisk::Range> protected_ranges_;
    std::map<std::uint64_t, std::vector<std::byte>> layout_guards_;
    mutable std::mutex mutex_;
    mutable bool invalidated_{};
};

class NativeProvider final : public DeviceProvider {
  public:
    explicit NativeProvider(const std::filesystem::path &helper) {
        if (!helper.empty())
            rawdisk::set_elevation_options({.helper_path = helper});
    }
    Result<std::vector<DeviceCandidate>> list() override {
        auto devices = rawdisk::list_usb_devices();
        if (!devices)
            return std::unexpected(device_error(devices.error()));
        const std::scoped_lock lock{mutex_};
        return device_detail::refresh_candidates(candidates_, *devices, [] { return secure_random_hex(16U); });
    }
    Result<ConnectedDevice> connect(std::string_view candidate_id, std::string_view layout,
                                    std::string_view xml) override {
        std::optional<rawdisk::scsi2sd::xml::Config> xml_config;
        if (layout == "scsi2sd-xml") {
            auto parsed = rawdisk::scsi2sd::xml::parse(xml);
            if (!parsed)
                return std::unexpected(Error{"device_layout_invalid", "invalid SCSI2SD XML"});
            xml_config = std::move(*parsed);
        } else if (layout != "raw" && layout != "scsi2sd-v6") {
            return std::unexpected(Error{"device_layout_invalid", "unsupported device layout"});
        }
        const std::scoped_lock lock{mutex_};
        const auto found = candidates_.find(std::string{candidate_id});
        if (found == candidates_.end())
            return std::unexpected(Error{"device_selection_stale", "refresh the device list and select again"});
        const auto info = found->second;
        auto directory = rawdisk::carrier_lease_directory();
        if (!directory)
            return std::unexpected(device_error(directory.error()));
        auto lease = rawdisk::claim_carrier(*directory, info.carrier_id);
        if (!lease)
            return std::unexpected(device_error(lease.error()));
        auto opened = rawdisk::open_device(info, rawdisk::Access::ReadOnly);
        if (!opened)
            return std::unexpected(device_error(opened.error()));
        std::vector<DeviceTargetDescription> targets;
        std::vector<rawdisk::Range> protected_ranges;
        std::map<std::uint64_t, std::vector<std::byte>> layout_guards;
        if (layout == "raw") {
            if (info.sector_size == 512U) {
                for (const auto lba : {rawdisk::scsi2sd::v6::config_lba(info.size_bytes / 512U),
                                       rawdisk::scsi2sd::v6::misplaced_save_lba(info.size_bytes / 512U)}) {
                    if (!lba)
                        continue;
                    auto config = rawdisk::scsi2sd::v6::read_config(**opened, *lba);
                    if (!config)
                        return std::unexpected(device_error(config.error()));
                    if (rawdisk::scsi2sd::v6::decode(*config))
                        return std::unexpected(
                            Error{"device_layout_required", "select the stored SCSI2SD layout for this carrier"});
                }
            }
            if (info.size_bytes <= maximum_sampler_device_bytes)
                targets.push_back(
                    {"disk", info.model, 0U, info.size_bytes, info.sector_size, info.sector_size != 512U, {}});
        } else {
            rawdisk::VolumeSet volumes;
            std::vector<rawdisk::scsi2sd::Target> configured_targets;
            bool layout_confirmed{};
            if (layout == "scsi2sd-v6") {
                auto config = rawdisk::scsi2sd::v6::read_primary(**opened);
                if (!config) {
                    if (config.error() == rawdisk::Errc::layout_not_found)
                        return std::unexpected(Error{"device_layout_unavailable",
                                                     "No SCSI2SDv6 configuration found in the final two sectors."});
                    if (config.error() == rawdisk::Errc::layout_invalid ||
                        config.error() == rawdisk::Errc::invalid_argument)
                        return std::unexpected(
                            Error{"device_layout_invalid",
                                  "The SCSI2SDv6 configuration has invalid or unsupported target geometry."});
                    return std::unexpected(device_error(config.error()));
                }
                layout_confirmed = true;
                const auto bytes = rawdisk::scsi2sd::v6::encode(*config);
                layout_guards.emplace((*opened)->sector_count() - rawdisk::scsi2sd::v6::kConfigSectors,
                                      std::vector<std::byte>{bytes.begin(), bytes.end()});
                configured_targets.assign(config->targets.begin(), config->targets.end());
                volumes = rawdisk::scsi2sd::v6::to_volumes(*config, info.size_bytes / 512U);
            } else if (layout == "scsi2sd-xml") {
                if (info.sector_size != 512U)
                    return std::unexpected(Error{"device_layout_invalid", "invalid SCSI2SD XML or carrier geometry"});
                volumes = rawdisk::scsi2sd::xml::to_volumes(*xml_config, info.size_bytes / 512U);
                configured_targets = xml_config->targets;
                const auto lba = rawdisk::scsi2sd::v6::config_lba(info.size_bytes / 512U);
                if (lba) {
                    auto bytes = rawdisk::scsi2sd::v6::read_config(**opened, *lba);
                    if (!bytes)
                        return std::unexpected(device_error(bytes.error()));
                    auto stored = rawdisk::scsi2sd::v6::decode(*bytes);
                    layout_confirmed =
                        stored && rawdisk::scsi2sd::same_target_layout(stored->targets, xml_config->targets);
                    layout_guards.emplace(*lba, std::vector<std::byte>{bytes->begin(), bytes->end()});
                }
            } else {
                return std::unexpected(Error{"device_layout_invalid", "unsupported device layout"});
            }
            for (const auto &volume : volumes.volumes) {
                const auto bytes = volume.sector_count * volume.sector_size;
                if (!volume.usable || bytes == 0U || bytes > maximum_sampler_device_bytes)
                    continue;
                DeviceTargetDescription target{volume.id,
                                               volume.label,
                                               volume.range.begin * volumes.parent_sector_size,
                                               bytes,
                                               volume.sector_size,
                                               !layout_confirmed || volume.sector_size != 512U ||
                                                   !rawdisk::check_writable(volumes, volume.id),
                                               {}};
                const auto configured = std::ranges::find_if(configured_targets, [&](const auto &value) {
                    return value.enabled && std::to_string(value.scsi_id) == volume.id;
                });
                if (configured != configured_targets.end() && configured->device_type <= 5U)
                    target.scsi = DeviceScsiTarget{configured->scsi_id, configured->device_type, configured->vendor,
                                                   configured->product, configured->revision,    configured->serial};
                if (!layout_confirmed)
                    target.notices.push_back(
                        "Read-only: layout is not confirmed by the card's primary V6 configuration.");
                for (const auto &notice : volumes.notices)
                    target.notices.push_back(notice.text);
                for (const auto &notice : volume.notices)
                    target.notices.push_back(notice.text);
                targets.push_back(std::move(target));
            }
            protected_ranges = volumes.protected_ranges;
            for (const auto &range : protected_ranges) {
                // Bind the exact bytes used for parsing, not a later configuration snapshot.
                if (layout_guards.contains(range.begin))
                    continue;
                const auto size = (range.end - range.begin) * info.sector_size;
                if (size > 1024U * 1024U)
                    return std::unexpected(Error{"device_layout_invalid", "layout guard range is too large"});
                std::vector<std::byte> bytes(static_cast<std::size_t>(size));
                if (auto read = (*opened)->read(range.begin, bytes); !read)
                    return std::unexpected(device_error(read.error()));
                layout_guards.emplace(range.begin, std::move(bytes));
            }
        }
        if (targets.empty())
            return std::unexpected(Error{"device_targets_unavailable", "no eligible targets at or below 9 GB"});
        return ConnectedDevice{std::make_shared<NativeTransport>(info, std::move(*lease), std::move(*opened),
                                                                 std::move(protected_ranges), std::move(layout_guards)),
                               {std::string{candidate_id}, info.model, info.serial, info.size_bytes, info.sector_size},
                               std::move(targets)};
    }

  private:
    std::map<std::string, rawdisk::DeviceInfo, std::less<>> candidates_;
    std::mutex mutex_;
};
#else
class NativeProvider final : public DeviceProvider {
  public:
    bool available() const noexcept override { return false; }
    Result<std::vector<DeviceCandidate>> list() override {
        return std::unexpected(Error{"devices_unavailable", "this build does not include device access"});
    }
    Result<ConnectedDevice> connect(std::string_view, std::string_view, std::string_view) override {
        return std::unexpected(Error{"devices_unavailable", "this build does not include device access"});
    }
};
#endif
} // namespace

std::shared_ptr<DeviceProvider> native_device_provider(const std::filesystem::path &helper) {
#if defined(AXK_BUILD_DEVICES)
    return std::make_shared<NativeProvider>(helper);
#else
    static_cast<void>(helper);
    return std::make_shared<NativeProvider>();
#endif
}

bool native_device_writes_qualified() noexcept {
#if defined(AXK_BUILD_DEVICES) &&                                                                                      \
    ((defined(__APPLE__) && (defined(__x86_64__) || defined(__aarch64__))) ||                                          \
     ((defined(__linux__) || defined(_WIN32)) && (defined(__x86_64__) || defined(_M_X64))))
    return true;
#else
    return false;
#endif
}
} // namespace axk::app
