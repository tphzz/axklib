#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/mutation_target.hpp"

namespace axk::app {

class OperationRegistry;
class DeviceFormatService;

inline constexpr std::uint64_t maximum_sampler_device_bytes = 9'000'000'000ULL;

struct DeviceIdentityDetails {
    std::string device_path;
    std::string usb_manufacturer;
    std::string usb_product;
    std::string usb_serial;
    std::string usb_vendor_id;
    std::string usb_product_id;
};

struct DeviceCandidate {
    std::string id;
    std::string model;
    std::string serial;
    std::uint64_t size_bytes{};
    std::uint32_t sector_size{};
    DeviceIdentityDetails identity{};
};

struct DeviceHostVolume {
    std::string device;
    std::string mount_point;
    std::string filesystem;
    std::string label;
};

struct DeviceWriteSafety {
    DeviceIdentityDetails identity;
    std::string membership;
    std::string recovery_membership;
    std::vector<DeviceHostVolume> mounted;
    std::vector<std::string> notices;
    bool complete{};
    bool non_dismounting_claim_supported{};
};

struct DeviceScsiTarget {
    std::uint8_t id{};
    std::uint8_t device_type{};
    std::string vendor;
    std::string product;
    std::string revision;
    std::string serial;
};

struct DeviceTargetDescription {
    std::string id;
    std::string label;
    std::uint64_t offset{};
    std::uint64_t size_bytes{};
    std::uint32_t sector_size{};
    bool read_only{true};
    std::vector<std::string> notices;
    std::optional<DeviceScsiTarget> scsi{};
};

// Only the native provider handles OS paths, elevation and exclusive write claims.
// Tests inject this interface; the manager never discovers devices on construction.
class DeviceTransport : public RandomAccessReader {
  public:
    virtual Result<void> authorize_writes() = 0;
    virtual Result<void> verify_bound() const = 0;
    virtual Result<void> verify_read_bound() const { return verify_bound(); }
    virtual Result<void> write_exact_at(std::uint64_t, std::span<const std::byte>) = 0;
    virtual Result<void> flush() = 0;
    virtual std::string carrier_identity() const = 0;
    virtual std::string media_identity() const = 0;
    virtual std::uint32_t sector_size() const noexcept = 0;
    virtual bool has_write_handle() const { return false; }
    virtual Result<DeviceWriteSafety> inspect_write_safety() const {
        return std::unexpected(Error{"device_format_unavailable", "Native format safety inspection is unavailable"});
    }
    virtual Result<void> authorize_initialization(const DeviceWriteSafety &) {
        return std::unexpected(Error{"device_format_unavailable", "Non-dismounting format claims are unavailable"});
    }
    virtual Result<void> verify_initialization_claim() const {
        return std::unexpected(Error{"device_format_unavailable", "No initialization claim is held"});
    }
};

struct ConnectedDevice {
    std::shared_ptr<DeviceTransport> transport;
    DeviceCandidate candidate;
    std::vector<DeviceTargetDescription> targets;
};

class DeviceProvider {
  public:
    virtual ~DeviceProvider() = default;
    virtual bool available() const noexcept { return true; }
    virtual Result<std::vector<DeviceCandidate>> list() = 0;
    virtual Result<ConnectedDevice> connect(std::string_view candidate_id, std::string_view layout,
                                            std::string_view xml) = 0;
};

[[nodiscard]] std::shared_ptr<DeviceProvider> native_device_provider(const std::filesystem::path &helper_path = {});
[[nodiscard]] bool native_device_writes_qualified() noexcept;

struct DeviceSessionSummary {
    std::string session_id;
    DeviceCandidate candidate;
    std::vector<DeviceTargetDescription> targets;
    bool writes_authorized{};
    bool invalidated{};
};

struct OpenedDeviceSource {
    std::shared_ptr<const RandomAccessReader> reader;
    std::function<Result<void>()> verify_unchanged;
    std::function<Result<void>(const CancellationToken &, ProgressSink *)> seal;
    std::string revision;
};

struct DeviceOperationStatus {
    std::string operation_id;
    std::string state;
    std::string model;
    std::string serial;
    std::string target_label;
    std::uint64_t target_size{};
    std::string target_identity{};
    std::string kind;
};

class DeviceSessionManager {
  public:
    DeviceSessionManager(std::filesystem::path state_directory, bool enabled = false, bool allow_writes = false,
                         std::shared_ptr<DeviceProvider> provider = {});
    ~DeviceSessionManager();
    DeviceSessionManager(const DeviceSessionManager &) = delete;
    DeviceSessionManager &operator=(const DeviceSessionManager &) = delete;

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool writes_supported() const noexcept;
    [[nodiscard]] Result<std::vector<DeviceCandidate>> list(std::string_view owner);
    [[nodiscard]] Result<DeviceSessionSummary> connect(std::string_view candidate_id, std::string_view owner,
                                                       std::string_view layout = "raw", std::string_view xml = {});
    [[nodiscard]] Result<DeviceSessionSummary> status(std::string_view session_id, std::string_view owner) const;
    // In-memory capability state only; callers must have verified their source in this read operation.
    [[nodiscard]] Result<DeviceSessionSummary> status_snapshot(std::string_view session_id,
                                                               std::string_view owner) const;
    [[nodiscard]] Result<void> close(std::string_view session_id, std::string_view owner);
    [[nodiscard]] Result<void> authorize_writes(std::string_view session_id, std::string_view target_id,
                                                std::string_view owner, bool exclusive_use_acknowledged,
                                                const CancellationToken &cancellation = {},
                                                ProgressSink *progress = nullptr);
    [[nodiscard]] Result<OpenedDeviceSource> open_source(std::string_view session_id, std::string_view target_id,
                                                         std::string_view owner);
    // Admission verifies metadata against fresh bytes and retains the exclusive
    // connection lock for the returned target's lifetime.
    [[nodiscard]] Result<std::shared_ptr<MutationTarget>> begin_mutation(std::string_view session_id,
                                                                         std::string_view target_id,
                                                                         std::string_view owner,
                                                                         std::string_view operation_id);
    [[nodiscard]] Result<void>
    apply(const std::shared_ptr<MutationTarget> &target, std::span<const AlterationJournalPatch> patches,
          const CancellationToken &cancellation, const std::function<Result<void>()> &validate,
          const std::function<void()> &on_rollback_verified,
          const std::function<Result<void>(std::shared_ptr<const RandomAccessReader>)> &admit_frozen,
          const std::function<Result<void>()> &admit_forward = {});
    [[nodiscard]] Result<std::vector<DeviceOperationStatus>> operations(std::string_view owner) const;
    // Host metadata only: fences an unstarted request so delayed submission
    // cannot write after an absent/no-write result has been acknowledged.
    [[nodiscard]] Result<DeviceOperationStatus> resolve(std::string_view operation_id, std::string_view owner);
    [[nodiscard]] Result<void> recover(std::string_view operation_id, std::string_view session_id,
                                       std::string_view target_id, std::string_view owner,
                                       bool exclusive_use_acknowledged, std::string_view confirmation_text = {},
                                       bool data_loss_acknowledged = false, const CancellationToken &cancellation = {});

  private:
    friend class DeviceFormatService;
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] Result<void> bind_device_operations(OperationRegistry &registry, DeviceSessionManager &devices);

} // namespace axk::app
