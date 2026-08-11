#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "axklib/application/contracts.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/uploads.hpp"
#include "axklib/package_import_planning.hpp"

namespace axk::app::package_plan_internal {

using Clock = std::chrono::steady_clock;

struct PackageInput {
    std::variant<FileRef, UploadRef> reference;

    friend bool operator==(const PackageInput &, const PackageInput &) = default;
};

struct Record {
    std::string token;
    std::string owner_id;
    Clock::time_point expires_at;
    FileRef target;
    FileRef output;
    std::filesystem::path output_path;
    bool overwrite{};
    std::vector<PackageInput> inputs;
    std::vector<UploadLease> upload_leases;
    std::uint64_t source_bytes{};
    PackageImportPlan plan;
    bool claimed{};
};

struct Store {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<Record>> plans;
    std::unordered_map<std::string, std::string> destination_reservations;
    std::chrono::minutes retention{15};
    std::size_t maximum_plans{128U};
    std::uint64_t maximum_plan_source_bytes{512U * 1024U * 1024U};
    std::uint64_t maximum_retained_source_bytes{1024U * 1024U * 1024U};
    std::size_t pending_plans{};
    std::uint64_t pending_source_bytes{};
};

struct RetainedSources {
    std::uint64_t source_bytes{};
    std::vector<UploadLease> upload_leases;
};

class Admission {
  public:
    Admission(std::shared_ptr<Store> store, std::uint64_t source_bytes);
    ~Admission();
    Admission(const Admission &) = delete;
    Admission &operator=(const Admission &) = delete;
    Admission(Admission &&other) noexcept;
    Admission &operator=(Admission &&) = delete;

    [[nodiscard]] Result<void> commit(const std::shared_ptr<Record> &record);

  private:
    void release() noexcept;

    std::shared_ptr<Store> store_;
    std::uint64_t source_bytes_{};
    bool active_{true};
};

class Claim {
  public:
    Claim(std::shared_ptr<Store> store, std::shared_ptr<Record> record);
    ~Claim();
    Claim(const Claim &) = delete;
    Claim &operator=(const Claim &) = delete;
    Claim(Claim &&other) noexcept;
    Claim &operator=(Claim &&) = delete;

    [[nodiscard]] const std::shared_ptr<Record> &record() const noexcept;
    void consume();

  private:
    void release();

    std::shared_ptr<Store> store_;
    std::shared_ptr<Record> record_;
    bool active_{true};
};

[[nodiscard]] Result<Admission> admit(const std::shared_ptr<Store> &store, std::uint64_t source_bytes);
[[nodiscard]] Result<RetainedSources> retain_sources(std::span<const PackageInput> inputs, std::string_view owner_id,
                                                     const Sandbox &sandbox, UploadStore &uploads);
[[nodiscard]] Result<Claim> claim(const std::shared_ptr<Store> &store, std::string_view token,
                                  std::string_view owner_id);
[[nodiscard]] Result<void> release(const std::shared_ptr<Store> &store, std::string_view token,
                                   std::string_view owner_id);
[[nodiscard]] Result<std::vector<PathAccess>> path_accesses(const std::shared_ptr<Store> &store, std::string_view token,
                                                            std::string_view owner_id);

} // namespace axk::app::package_plan_internal
