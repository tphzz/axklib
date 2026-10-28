#pragma once

#include "rawdisk/block_device.hpp"

#include <string>
#include <vector>

namespace rawdisk {

// Informational descriptors are never a substitute for DeviceInfo's native binding.
struct DevicePresentation {
    std::string usb_manufacturer;
    std::string usb_product;
    std::string usb_serial;
    std::string usb_vendor_id;
    std::string usb_product_id;
};

struct HostVolume {
    std::string device;
    std::string mount_point;
    std::string filesystem;
    std::string label;
};

struct DeviceInventory {
    DevicePresentation presentation;
    std::vector<DeviceInfo> members;
    std::vector<HostVolume> mounted;
    std::vector<std::string> notices;
    bool complete{};
    bool non_dismounting_claim_supported{};
};

// Fresh native metadata only: never browses a filesystem, mounts or dismounts.
Result<DevicePresentation> describe_usb_device(const DeviceInfo &expected) noexcept;
Result<DeviceInventory> inspect_usb_device(const DeviceInfo &expected, BlockDevice *retained_claim = nullptr) noexcept;

} // namespace rawdisk
