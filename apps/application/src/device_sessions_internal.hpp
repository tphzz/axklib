#pragma once

#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/application/device_sessions.hpp"
#include "device_read_cache.hpp"
#include "device_read_metrics.hpp"

namespace axk::app::device_detail {

using Json = nlohmann::json;
using Guards = std::map<std::uint64_t, std::string>;
inline constexpr std::size_t maximum_guard_sectors = 65'536U;

struct Connection {
    std::string id;
    std::string owner;
    ConnectedDevice device;
    std::recursive_mutex io;
    std::string authorized_target;
    std::string initialization_operation;
    bool invalidated{};
    bool mutation_active{};
    std::uint64_t revision{1U};
    std::map<std::string, Guards, std::less<>> metadata_guards;
    std::map<std::string, std::uint64_t, std::less<>> logical_sizes;
    mutable ReadCounters reads;
};

[[nodiscard]] Result<std::string> digest(std::span<const std::byte> bytes);
[[nodiscard]] Result<void> save_json(const std::filesystem::path &path, const Json &value, bool create,
                                     ReadCounters *counters = nullptr);
[[nodiscard]] Result<void> verify_bound(const Connection &connection);
[[nodiscard]] Result<void> verify_read_bound(const Connection &connection);
[[nodiscard]] Result<Json> load_json(const std::filesystem::path &path);
[[nodiscard]] std::string target_identity(const Connection &connection, const DeviceTargetDescription &target);
[[nodiscard]] const DeviceTargetDescription *find_target(const Connection &connection, std::string_view id);
[[nodiscard]] Result<void> verify_guards(const Connection &connection, const DeviceTargetDescription &target,
                                         const Guards &guards, std::span<const AlterationJournalPatch> excluded = {},
                                         const CancellationToken &cancellation = {}, ProgressSink *progress = nullptr,
                                         std::string_view label = "Verifying device metadata");
[[nodiscard]] axk::Result<void> read_target(const Connection &connection, const DeviceTargetDescription &target,
                                            std::uint64_t offset, std::span<std::byte> bytes);

class Source final : public RandomAccessReader {
  public:
    Source(std::shared_ptr<Connection> connection, DeviceTargetDescription target, std::uint64_t logical_size = 0U);
    std::uint64_t size() const noexcept override;
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override;
    Result<void> seal(const CancellationToken &cancellation, ProgressSink *progress);
    Json read_metrics() const;

  private:
    std::shared_ptr<Connection> connection_;
    DeviceTargetDescription target_;
    mutable Guards guards_;
    mutable MetadataReadCache cache_;
    mutable bool sealed_{};
    std::uint64_t revision_{};
    ReadCounters initial_reads_;
    std::uint64_t logical_size_{};
};

class Mutation final : public MutationTarget {
  public:
    Mutation(std::shared_ptr<Connection> connection, DeviceTargetDescription target, std::string operation,
             std::filesystem::path directory, bool recovery = false, std::uint64_t logical_size = 0U);
    ~Mutation() override;
    std::uint64_t size() const noexcept override;
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override;
    Result<void> write_exact_at(std::uint64_t offset, std::span<const std::byte> bytes) override;
    Result<void> flush() override;
    Result<void> verify_bound() const override;
    Result<void> verify_unchanged() const override;
    std::string revision() const override;
    std::string stable_identity() const override;
    MutationIdentity journal_identity() const override;
    std::uint32_t write_alignment() const noexcept override;
    Result<void> set_state(std::string_view state) const;
    Result<void> publish_guards(bool create) const;
    Json read_metrics() const;
    void journal_streaming(bool value) noexcept {
        cache_.clear();
        journal_streaming_ = value;
    }

    std::shared_ptr<Connection> connection;
    DeviceTargetDescription target;
    std::string operation_id;
    std::filesystem::path directory;
    mutable Guards guards;

  private:
    std::unique_lock<std::recursive_mutex> lease_;
    mutable bool writing_{};
    bool journal_streaming_{};
    mutable MetadataReadCache cache_;
    ReadCounters initial_reads_;
    std::uint64_t logical_size_{};
};

[[nodiscard]] Result<std::vector<AlterationJournalPatch>>
align_patches(const std::shared_ptr<MutationTarget> &target, std::span<const AlterationJournalPatch> patches);

[[nodiscard]] Result<std::shared_ptr<Source>> prepare_source(const std::shared_ptr<Connection> &connection,
                                                             const DeviceTargetDescription &target);
[[nodiscard]] std::string carrier_fence(const Connection &connection);
[[nodiscard]] Result<void> require_clear_carrier(const std::filesystem::path &directory, std::string_view fence,
                                                 std::string_view except_operation = {});
[[nodiscard]] std::string device_confirmation(const DeviceCandidate &candidate, const DeviceTargetDescription &target,
                                              std::string_view verb);
[[nodiscard]] Result<void> verify_physical_guards(const Connection &connection, const Guards &guards,
                                                  std::uint64_t target_offset = 0U,
                                                  std::span<const AlterationJournalPatch> excluded = {},
                                                  const CancellationToken &cancellation = {});

} // namespace axk::app::device_detail

struct axk::app::DeviceSessionManager::Implementation {
    std::filesystem::path directory;
    bool enabled{};
    bool allow_writes{};
    bool ready{};
    std::shared_ptr<DeviceProvider> provider;
    mutable std::mutex mutex;
    std::mutex operation_admission;
    std::map<std::string, std::shared_ptr<device_detail::Connection>, std::less<>> sessions;
    std::map<std::string, std::set<std::string>, std::less<>> candidates;

    Result<std::shared_ptr<device_detail::Connection>> owned(std::string_view id, std::string_view owner) const;
};
