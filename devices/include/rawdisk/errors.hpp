// rawdisk error codes. Every rawdisk function reports failures as std::error_code:
// OS errors keep their system/generic category, rawdisk's own conditions use
// rawdisk_category(). All Errc values also compare equal to a portable std::errc
// condition where one fits (e.g. Errc::device_busy == std::errc::device_or_resource_busy).
#pragma once

#include <system_error>
#include <type_traits>

namespace rawdisk {

enum class Errc : int {
    bad_length = 1,              // I/O size is not a multiple of the sector size
    out_of_range,                // I/O extends past the last sector
    read_only_handle,            // write on a handle opened with Access::ReadOnly
    write_protected,             // the medium or reader is write-protected
    device_changed,              // the selected device is gone or no longer matches (replug, other card)
    device_busy,                 // a volume on the disk is mounted/in use, or the image is locked
    not_a_usb_disk,              // refused: not a USB whole-disk device
    capacity_unreadable,         // the last sector of the reported capacity cannot be read
    elevation_cancelled,         // the user declined the elevation (UAC / password) prompt
    elevation_failed,            // system authorization provider failed
    authorization_unavailable,   // system authorization service is unavailable
    authorization_denied,        // system policy denied authorization or no prompt agent is available
    authorization_timeout,       // system authorization exceeded its deadline
    helper_protocol,             // malformed system authorization or helper message
    helper_timeout,              // elevated helper stopped responding
    too_many_handles,            // helper handle limit reached
    invalid_argument,            // bad argument (sector size, path, ...)
    layout_not_found,            // no SCSI2SD configuration on the card
    layout_invalid,              // SCSI2SD configuration/XML is malformed or unusable
    layout_conflict,             // refused: virtual disk overlaps another disk or the config sectors
    flow_missing_answer,         // open flow: a required answer is missing
    flow_invalid_answer,         // open flow: an answer is malformed, unknown or not allowed
    flow_stale_selection,        // open flow: the selected item no longer exists or no longer matches
    locator_invalid,             // malformed locator string
    device_identity_unavailable, // Windows could not establish a complete PnP connection identity
    device_connection_changed,   // Windows USB parent, disk instance or arrival changed
    device_descriptor_changed,   // Windows device model, serial or USB transport changed
    device_geometry_changed,     // Windows device capacity or sector size changed
    internal_error,              // unexpected condition; keep this the last enumerator
};

const std::error_category &rawdisk_category() noexcept;
std::error_code make_error_code(Errc e) noexcept;

} // namespace rawdisk

template <> struct std::is_error_code_enum<rawdisk::Errc> : std::true_type {};
