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

inline constexpr std::uint64_t maximum_sampler_device_bytes = 9'000'000'000ULL;

struct DeviceCandidate {
    std::string id;
    std::string model;
    std::string serial;
    std::uint64_t size_bytes{};
    std::uint32_t sector_size{};
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
          const std::function<Result<void>(std::shared_ptr<const RandomAccessReader>)> &admit_frozen);
    [[nodiscard]] Result<std::vector<DeviceOperationStatus>> operations(std::string_view owner) const;
    // Host metadata only: fences an unstarted request so delayed submission
    // cannot write after an absent/no-write result has been acknowledged.
    [[nodiscard]] Result<DeviceOperationStatus> resolve(std::string_view operation_id, std::string_view owner);
    [[nodiscard]] Result<void> recover(std::string_view operation_id, std::string_view session_id,
                                       std::string_view target_id, std::string_view owner,
                                       bool exclusive_use_acknowledged);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] Result<void> bind_device_operations(OperationRegistry &registry, DeviceSessionManager &devices);

} // namespace axk::app
