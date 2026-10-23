#include "rawdisk/errors.hpp"

#include <string>

namespace rawdisk {
namespace {

class Category final : public std::error_category {
  public:
    const char *name() const noexcept override { return "rawdisk"; }

    std::string message(int v) const override {
        switch (static_cast<Errc>(v)) {
        case Errc::bad_length:
            return "transfer size is not a multiple of the sector size";
        case Errc::out_of_range:
            return "sector range is beyond the end of the device";
        case Errc::read_only_handle:
            return "device was opened read-only";
        case Errc::write_protected:
            return "medium is write-protected";
        case Errc::device_changed:
            return "device is gone or no longer matches the selected one";
        case Errc::device_identity_unavailable:
            return "Windows could not verify the device connection identity; refresh and select the device again";
        case Errc::device_connection_changed:
            return "device connection changed; refresh and select the device again";
        case Errc::device_descriptor_changed:
            return "device model, serial number or USB transport changed; select the device again";
        case Errc::device_geometry_changed:
            return "device capacity or sector size changed; select the device again";
        case Errc::device_busy:
            return "disk is mounted or in use, or the image is locked";
        case Errc::not_a_usb_disk:
            return "not a USB disk";
        case Errc::capacity_unreadable:
            return "reader reports a capacity it cannot read (faulty reader or card)";
        case Errc::elevation_cancelled:
            return "administrator permission was declined";
        case Errc::elevation_failed:
            return "system authorization failed (provider missing, crashed or disconnected)";
        case Errc::authorization_unavailable:
            return "system device authorization is unavailable; install or start UDisks2 and the system D-Bus service";
        case Errc::authorization_denied:
            return "system device authorization was denied; check policy and that a desktop polkit authentication "
                   "agent is running";
        case Errc::authorization_timeout:
            return "system device authorization timed out; try opening the device again";
        case Errc::helper_protocol:
            return "malformed message from the system authorization provider or helper";
        case Errc::helper_timeout:
            return "elevated helper stopped responding";
        case Errc::too_many_handles:
            return "too many devices open through the elevated helper";
        case Errc::invalid_argument:
            return "invalid argument";
        case Errc::layout_not_found:
            return "no SCSI2SD configuration found on the card";
        case Errc::layout_invalid:
            return "SCSI2SD configuration is malformed or unusable";
        case Errc::layout_conflict:
            return "virtual disk overlaps another disk or the configuration sectors";
        case Errc::flow_missing_answer:
            return "a required answer is missing";
        case Errc::flow_invalid_answer:
            return "an answer is malformed, unknown or not allowed";
        case Errc::flow_stale_selection:
            return "the selected item no longer exists or no longer matches";
        case Errc::locator_invalid:
            return "malformed locator";
        case Errc::internal_error:
            return "internal error";
        }
        return "unknown rawdisk error";
    }

    std::error_condition default_error_condition(int v) const noexcept override {
        switch (static_cast<Errc>(v)) {
        case Errc::bad_length:
        case Errc::invalid_argument:
            return std::errc::invalid_argument;
        case Errc::out_of_range:
            return std::errc::result_out_of_range;
        case Errc::read_only_handle:
            return std::errc::operation_not_permitted;
        case Errc::write_protected:
            return std::errc::read_only_file_system;
        case Errc::device_changed:
        case Errc::device_connection_changed:
        case Errc::device_descriptor_changed:
        case Errc::device_geometry_changed:
        case Errc::not_a_usb_disk:
            return std::errc::no_such_device;
        case Errc::device_busy:
            return std::errc::device_or_resource_busy;
        case Errc::capacity_unreadable:
        case Errc::device_identity_unavailable:
            return std::errc::io_error;
        case Errc::elevation_cancelled:
            return std::errc::operation_canceled;
        case Errc::elevation_failed:
        case Errc::authorization_unavailable:
        case Errc::authorization_denied:
            return std::errc::permission_denied;
        case Errc::authorization_timeout:
            return std::errc::timed_out;
        case Errc::helper_protocol:
            return std::errc::protocol_error;
        case Errc::helper_timeout:
            return std::errc::timed_out;
        case Errc::too_many_handles:
            return std::errc::too_many_files_open;
        case Errc::layout_not_found:
            return std::errc::no_such_file_or_directory;
        case Errc::layout_invalid:
            return std::errc::invalid_argument;
        case Errc::layout_conflict:
            return std::errc::device_or_resource_busy;
        case Errc::flow_missing_answer:
        case Errc::flow_invalid_answer:
        case Errc::locator_invalid:
            return std::errc::invalid_argument;
        case Errc::flow_stale_selection:
            return std::errc::no_such_device;
        case Errc::internal_error:
            break;
        }
        return {v, *this};
    }
};

} // namespace

const std::error_category &rawdisk_category() noexcept {
    static const Category category;
    return category;
}

std::error_code make_error_code(Errc e) noexcept { return {static_cast<int>(e), rawdisk_category()}; }

} // namespace rawdisk
