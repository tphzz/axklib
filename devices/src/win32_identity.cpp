#include "win32_common.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>
#include <winioctl.h>

namespace rawdisk::detail {
namespace {

std::string utf8(std::wstring_view value) {
    const auto length = static_cast<int>(value.size());
    const auto size =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), size, nullptr,
                              nullptr) != size)
        return {};
    return result;
}

std::pair<std::string, std::string> usb_parent(DEVINST instance) {
    const auto disk_instance = instance;
    for (std::size_t depth = 0; depth < 64; ++depth) {
        std::array<wchar_t, MAX_DEVICE_ID_LEN> name{};
        if (::CM_Get_Device_IDW(instance, name.data(), static_cast<ULONG>(name.size()), 0) != CR_SUCCESS)
            return {};
        const std::wstring_view id{name.data()};
        if (id.starts_with(L"USB\\VID_") && id.find(L"&MI_") == std::wstring_view::npos)
            return {utf8(id), std::format("{}:{}", instance, disk_instance)};
        DEVINST parent{};
        if (::CM_Get_Parent(&parent, instance, 0) != CR_SUCCESS)
            return {};
        instance = parent;
    }
    return {};
}

} // namespace

std::pair<std::string, std::string> win32_usb_identity(HANDLE disk) {
    STORAGE_DEVICE_NUMBER wanted{};
    DWORD returned{};
    if (!::DeviceIoControl(disk, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &wanted, sizeof wanted, &returned,
                           nullptr))
        return {};
    const auto set =
        ::SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return {};
    struct SetCloser {
        HDEVINFO value;
        ~SetCloser() { ::SetupDiDestroyDeviceInfoList(value); }
    } close_set{set};
    for (DWORD index = 0; index < 4096; ++index) {
        SP_DEVICE_INTERFACE_DATA interface{};
        interface.cbSize = sizeof interface;
        if (!::SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_DISK, index, &interface))
            break;
        DWORD needed{};
        ::SetupDiGetDeviceInterfaceDetailW(set, &interface, nullptr, 0, &needed, nullptr);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || needed > 65536)
            continue;
        std::vector<std::byte> storage(needed);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof device;
        if (!::SetupDiGetDeviceInterfaceDetailW(set, &interface, detail, needed, nullptr, &device))
            continue;
        Handle handle{::CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    0, nullptr)};
        if (!handle.valid())
            continue;
        STORAGE_DEVICE_NUMBER current{};
        if (!::DeviceIoControl(handle.h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &current, sizeof current,
                               &returned, nullptr))
            continue;
        if (current.DeviceType == wanted.DeviceType && current.DeviceNumber == wanted.DeviceNumber)
            return usb_parent(device.DevInst);
    }
    return {};
}

} // namespace rawdisk::detail
