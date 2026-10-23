#include "win32_identity.hpp"
#include "win32_common.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <initguid.h>

#include <cfgmgr32.h>
#include <devpkey.h>
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

struct NodeApi {
    Result<std::string> device_id(std::uint32_t instance) const {
        std::array<wchar_t, MAX_DEVICE_ID_LEN> name{};
        const auto status =
            ::CM_Get_Device_IDW(static_cast<DEVINST>(instance), name.data(), static_cast<ULONG>(name.size()), 0);
        if (status != CR_SUCCESS)
            return fail(win_error(::CM_MapCrToWin32Err(status, ERROR_GEN_FAILURE)));
        return utf8(name.data());
    }

    Result<std::uint64_t> last_arrival(std::uint32_t instance) const {
        DEVPROPTYPE type{};
        FILETIME arrival{};
        ULONG size = static_cast<ULONG>(sizeof arrival);
        const auto status = ::CM_Get_DevNode_PropertyW(static_cast<DEVINST>(instance), &DEVPKEY_Device_LastArrivalDate,
                                                       &type, reinterpret_cast<PBYTE>(&arrival), &size, 0);
        if (status == CR_NO_SUCH_VALUE || status == CR_BUFFER_SMALL)
            return fail(Errc::device_identity_unavailable);
        if (status != CR_SUCCESS)
            return fail(win_error(::CM_MapCrToWin32Err(status, ERROR_GEN_FAILURE)));
        if (type != DEVPROP_TYPE_FILETIME || size != sizeof arrival)
            return fail(Errc::device_identity_unavailable);
        return (static_cast<std::uint64_t>(arrival.dwHighDateTime) << 32U) | arrival.dwLowDateTime;
    }

    Result<std::uint32_t> parent(std::uint32_t instance) const {
        DEVINST parent{};
        const auto status = ::CM_Get_Parent(&parent, static_cast<DEVINST>(instance), 0);
        if (status != CR_SUCCESS)
            return fail(win_error(::CM_MapCrToWin32Err(status, ERROR_GEN_FAILURE)));
        return static_cast<std::uint32_t>(parent);
    }
};

} // namespace

Result<Win32UsbIdentity> win32_usb_identity(HANDLE disk) {
    STORAGE_DEVICE_NUMBER wanted{};
    DWORD returned{};
    if (!::DeviceIoControl(disk, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &wanted, sizeof wanted, &returned,
                           nullptr))
        return fail(last_error());
    if (returned < sizeof wanted)
        return fail(Errc::device_identity_unavailable);
    const auto set =
        ::SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return fail(last_error());
    struct SetCloser {
        HDEVINFO value;
        ~SetCloser() { ::SetupDiDestroyDeviceInfoList(value); }
    } close_set{set};
    for (DWORD index = 0; index < 4096; ++index) {
        SP_DEVICE_INTERFACE_DATA interface{};
        interface.cbSize = sizeof interface;
        if (!::SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_DISK, index, &interface)) {
            const auto error = ::GetLastError();
            return fail(error == ERROR_NO_MORE_ITEMS ? make_error_code(Errc::device_changed) : win_error(error));
        }
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
        if (returned >= sizeof current && current.DeviceType == wanted.DeviceType &&
            current.DeviceNumber == wanted.DeviceNumber) {
            NodeApi api;
            return win32_node_identity(api, static_cast<std::uint32_t>(device.DevInst));
        }
    }
    return fail(Errc::device_identity_unavailable);
}

} // namespace rawdisk::detail
