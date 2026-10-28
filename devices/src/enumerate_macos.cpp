#include "detail.hpp"
#include "rawdisk/block_device.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <memory>
#include <optional>
#include <type_traits>

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>

namespace rawdisk {
namespace {

struct CFReleaser {
    void operator()(CFTypeRef r) const noexcept {
        if (r)
            CFRelease(r);
    }
};
template <class T> using CFPtr = std::unique_ptr<std::remove_pointer_t<T>, CFReleaser>;

struct IOObject {
    io_object_t obj = IO_OBJECT_NULL;
    explicit IOObject(io_object_t o = IO_OBJECT_NULL) noexcept : obj(o) {}
    ~IOObject() {
        if (obj != IO_OBJECT_NULL)
            IOObjectRelease(obj);
    }
    IOObject(const IOObject &) = delete;
    IOObject &operator=(const IOObject &) = delete;
};

std::string to_string(CFTypeRef v) {
    if (!v || CFGetTypeID(v) != CFStringGetTypeID())
        return {};
    const auto s = static_cast<CFStringRef>(v);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    if (max <= 1 || max > 4096)
        return {};
    std::string buf(static_cast<std::size_t>(max), '\0');
    if (!CFStringGetCString(s, buf.data(), max, kCFStringEncodingUTF8))
        return {};
    buf.resize(std::char_traits<char>::length(buf.c_str()));
    return buf;
}

std::uint64_t to_u64(CFTypeRef v) {
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID())
        return 0;
    std::int64_t x = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &x))
        return 0;
    return x < 0 ? 0 : static_cast<std::uint64_t>(x);
}

bool to_bool(CFTypeRef v) {
    return v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(v));
}

// APFS "synthesized" disks are whole-disk IOMedia objects on top of a container; they are
// not physical devices and must never be offered.
bool is_apfs_synthesized(io_service_t media) {
    IOObject parent;
    if (IORegistryEntryGetParentEntry(media, kIOServicePlane, &parent.obj) != KERN_SUCCESS)
        return false;
    return IOObjectConformsTo(parent.obj, "AppleAPFSContainer");
}

std::string usb_serial(io_service_t media) {
    for (CFStringRef key : {CFSTR("USB Serial Number"), CFSTR("kUSBSerialNumberString")}) {
        CFPtr<CFTypeRef> v{IORegistryEntrySearchCFProperty(media, kIOServicePlane, key, kCFAllocatorDefault,
                                                           kIORegistryIterateRecursively | kIORegistryIterateParents)};
        if (auto s = detail::trim(to_string(v.get())); !s.empty())
            return s;
    }
    return {};
}

std::string usb_parent(io_service_t media) {
    io_registry_entry_t entry = media;
    IOObject parent;
    for (std::size_t depth = 0; depth < 64; ++depth) {
        if (IOObjectConformsTo(entry, "IOUSBHostDevice") || IOObjectConformsTo(entry, "IOUSBDevice")) {
            std::uint64_t identity{};
            if (IORegistryEntryGetRegistryEntryID(entry, &identity) == KERN_SUCCESS)
                return std::to_string(identity);
            return {};
        }
        io_registry_entry_t next{};
        if (IORegistryEntryGetParentEntry(entry, kIOServicePlane, &next) != KERN_SUCCESS)
            return {};
        if (parent.obj != IO_OBJECT_NULL)
            IOObjectRelease(parent.obj);
        parent.obj = next;
        entry = next;
    }
    return {};
}

std::optional<DeviceInfo> describe(DASessionRef session, const std::string &bsd) {
    if (!bsd.starts_with("disk") || bsd.size() < 5 ||
        !std::all_of(bsd.begin() + 4, bsd.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt; // whole disks only: "disk4", never "disk4s1"

    CFPtr<DADiskRef> disk{DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd.c_str())};
    if (!disk)
        return std::nullopt;
    CFPtr<CFDictionaryRef> desc{DADiskCopyDescription(disk.get())};
    if (!desc)
        return std::nullopt;
    auto get = [&](CFStringRef key) { return CFDictionaryGetValue(desc.get(), key); };

    if (!to_bool(get(kDADiskDescriptionMediaWholeKey)))
        return std::nullopt;
    if (to_string(get(kDADiskDescriptionDeviceProtocolKey)) != "USB")
        return std::nullopt;
    if (to_bool(get(kDADiskDescriptionDeviceInternalKey)))
        return std::nullopt;

    IOObject media{DADiskCopyIOMedia(disk.get())};
    if (media.obj == IO_OBJECT_NULL || is_apfs_synthesized(media.obj))
        return std::nullopt;

    DeviceInfo d;
    d.path = "/dev/r" + bsd; // raw character device: unbuffered and much faster
    d.model = detail::join_model(to_string(get(kDADiskDescriptionDeviceVendorKey)),
                                 to_string(get(kDADiskDescriptionDeviceModelKey)));
    d.serial = usb_serial(media.obj);
    d.carrier_id = usb_parent(media.obj);
    std::uint64_t media_identity{};
    if (IORegistryEntryGetRegistryEntryID(media.obj, &media_identity) == KERN_SUCCESS)
        d.connection_id = std::to_string(media_identity);
    d.size_bytes = to_u64(get(kDADiskDescriptionMediaSizeKey));
    const std::uint64_t ss = to_u64(get(kDADiskDescriptionMediaBlockSizeKey));
    d.removable = to_bool(get(kDADiskDescriptionMediaRemovableKey));

    if (d.size_bytes == 0 || !detail::valid_sector_size(ss) || d.size_bytes % ss != 0)
        return std::nullopt;
    d.sector_size = static_cast<std::uint32_t>(ss);
    return d;
}

} // namespace

namespace detail {

Result<DevicePresentation> presentation_platform(const DeviceInfo &info) {
    DevicePresentation result;
    std::uint64_t identity{};
    const auto parsed =
        std::from_chars(info.carrier_id.data(), info.carrier_id.data() + info.carrier_id.size(), identity);
    if (parsed.ec != std::errc{} || parsed.ptr != info.carrier_id.data() + info.carrier_id.size())
        return result;
    auto matching = IORegistryEntryIDMatching(identity);
    if (!matching)
        return result;
    IOObject usb{IOServiceGetMatchingService(MACH_PORT_NULL, matching)};
    if (!usb.obj)
        return result;
    const auto string = [&](CFStringRef key) {
        CFPtr<CFTypeRef> value{IORegistryEntryCreateCFProperty(usb.obj, key, kCFAllocatorDefault, 0)};
        return to_string(value.get());
    };
    const auto hex = [&](CFStringRef key) -> std::string {
        CFPtr<CFTypeRef> value{IORegistryEntryCreateCFProperty(usb.obj, key, kCFAllocatorDefault, 0)};
        if (!value)
            return {};
        return std::format("{:04x}", to_u64(value.get()));
    };
    result.usb_manufacturer = string(CFSTR("USB Vendor Name"));
    result.usb_product = string(CFSTR("USB Product Name"));
    result.usb_serial = string(CFSTR("USB Serial Number"));
    result.usb_vendor_id = hex(CFSTR("idVendor"));
    result.usb_product_id = hex(CFSTR("idProduct"));
    return result;
}

std::optional<DeviceInfo> macos_describe(const std::string &bsd_name) {
    CFPtr<DASessionRef> session{DASessionCreate(kCFAllocatorDefault)};
    if (!session)
        return std::nullopt;
    return describe(session.get(), bsd_name);
}

Result<std::vector<DeviceInfo>> list_platform() {
    CFPtr<DASessionRef> session{DASessionCreate(kCFAllocatorDefault)};
    if (!session)
        return fail(Errc::internal_error);

    CFMutableDictionaryRef match = IOServiceMatching(kIOMediaClass); // consumed below
    if (!match)
        return fail(Errc::internal_error);
    CFDictionarySetValue(match, CFSTR(kIOMediaWholeKey), kCFBooleanTrue);

    IOObject iter;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, match, &iter.obj) != KERN_SUCCESS)
        return fail(Errc::internal_error);

    std::vector<DeviceInfo> out;
    while (true) {
        IOObject media{IOIteratorNext(iter.obj)};
        if (media.obj == IO_OBJECT_NULL)
            break;
        CFPtr<CFTypeRef> bsd{IORegistryEntryCreateCFProperty(media.obj, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0)};
        const std::string name = to_string(bsd.get());
        if (name.empty())
            continue;
        if (auto d = describe(session.get(), name))
            out.push_back(std::move(*d));
    }
    std::ranges::sort(out, {}, &DeviceInfo::path);
    return out;
}

} // namespace detail
} // namespace rawdisk
