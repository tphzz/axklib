#pragma once

#include "rawdisk/block_device.hpp"

#include <chrono>
#include <memory>

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>

namespace rawdisk::detail {

// Native entry points are injectable so qualification tests never touch a disk.
struct MacosClaimApi {
    decltype(&DASessionCreate) create_session = &DASessionCreate;
    decltype(&DADiskCreateFromBSDName) create_disk = &DADiskCreateFromBSDName;
    decltype(&DASessionScheduleWithRunLoop) schedule = &DASessionScheduleWithRunLoop;
    decltype(&DASessionUnscheduleFromRunLoop) unschedule = &DASessionUnscheduleFromRunLoop;
    decltype(&DARegisterDiskMountApprovalCallback) register_mount = &DARegisterDiskMountApprovalCallback;
    decltype(&DARegisterDiskEjectApprovalCallback) register_eject = &DARegisterDiskEjectApprovalCallback;
    decltype(&DARegisterDiskDisappearedCallback) register_disappeared = &DARegisterDiskDisappearedCallback;
    decltype(&DAUnregisterCallback) unregister_callback = &DAUnregisterCallback;
    decltype(&DADiskClaim) claim = &DADiskClaim;
    decltype(&DADiskUnclaim) unclaim = &DADiskUnclaim;
    decltype(&CFRunLoopRunInMode) service = &CFRunLoopRunInMode;
    Result<bool> (*matches)(DADiskRef, const DeviceInfo &) = nullptr;
    Result<void> (*check_unmounted)(DASessionRef, const DeviceInfo &) = nullptr;
};

class MacosWriteClaim final {
  public:
    static Result<std::unique_ptr<MacosWriteClaim>>
    acquire(const DeviceInfo &info, MacosClaimApi api = {},
            std::chrono::milliseconds timeout = std::chrono::seconds(10));
    ~MacosWriteClaim();
    MacosWriteClaim(const MacosWriteClaim &) = delete;
    MacosWriteClaim &operator=(const MacosWriteClaim &) = delete;
    bool valid() const noexcept;

  private:
    struct State;
    explicit MacosWriteClaim(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace rawdisk::detail
