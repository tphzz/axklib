#include "rawdisk/inspection.hpp"
#include "detail.hpp"

#include <algorithm>

namespace rawdisk {
Result<DevicePresentation> describe_usb_device(const DeviceInfo &expected) noexcept {
    return detail::guard([&] { return detail::presentation_platform(expected); });
}

Result<DeviceInventory> inspect_usb_device(const DeviceInfo &expected, BlockDevice *retained_claim) noexcept {
    return detail::guard([&]() -> Result<DeviceInventory> {
        if (expected.carrier_id.empty() || expected.connection_id.empty())
            return detail::fail(Errc::device_identity_unavailable);
        auto devices = list_usb_devices();
        if (!devices)
            return detail::fail(devices.error());
        if (std::ranges::none_of(*devices, [&](const auto &device) { return detail::same_identity(device, expected); }))
            return detail::fail(Errc::device_changed);
        if (retained_claim) {
            if (auto claimed = retained_claim->verify_non_dismounting_claim(expected); !claimed)
                return detail::fail(claimed.error());
        }
        DeviceInventory inventory;
        inventory.complete = true;
        inventory.non_dismounting_claim_supported = true;
        if (auto presentation = detail::presentation_platform(expected); presentation)
            inventory.presentation = std::move(*presentation);
        for (const auto &device : *devices) {
            if (device.carrier_id != expected.carrier_id)
                continue;
            auto member = detail::inventory_platform(device, retained_claim && detail::same_identity(device, expected));
            if (!member)
                return detail::fail(member.error());
            inventory.members.push_back(device);
            inventory.complete = inventory.complete && member->complete;
            inventory.non_dismounting_claim_supported =
                inventory.non_dismounting_claim_supported && member->non_dismounting_claim_supported;
            inventory.mounted.insert(inventory.mounted.end(), member->mounted.begin(), member->mounted.end());
            inventory.notices.insert(inventory.notices.end(), member->notices.begin(), member->notices.end());
        }
        // Native claims currently protect one OS whole disk. Never pretend this
        // establishes mount inhibition on a sibling LUN of the same USB carrier.
        if (inventory.members.size() != 1U) {
            inventory.non_dismounting_claim_supported = false;
            inventory.notices.push_back("Formatting is unavailable: this USB carrier exposes multiple OS disks.");
        }
        return inventory;
    });
}
} // namespace rawdisk
