#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "axklib/application/contracts.hpp"
#include "axklib/application/image_documents.hpp"
#include "axklib/conversion/plan.hpp"

namespace axk::app::conversion_internal {

struct Payload {
    std::shared_ptr<const conversion::PreparedPlan> prepared;
    ImageDocumentRef destination;
    std::uint64_t revision{};
    std::string destination_fingerprint;
    VolumeCapacityPolicy capacity_policy;
    std::shared_ptr<const native::Document> source;
    std::function<Result<void>()> verify_workspace;
    std::uint64_t source_retention_bytes{};
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept {
        return prepared->retained_bytes() + source_retention_bytes;
    }
};

enum class State { ready, accepted, executing, terminal };

struct Record {
    std::string id;
    std::string owner;
    std::chrono::steady_clock::time_point expires;
    std::uint64_t expires_unix_ms{};
    State state{State::ready};
    std::string job_id;
    std::shared_ptr<const Payload> payload;
};

class PlanStore final {
  public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;
    struct Limits {
        std::size_t plans{128U};
        std::uint64_t plan_bytes{512ULL * 1024ULL * 1024ULL};
        std::uint64_t total_bytes{1024ULL * 1024ULL * 1024ULL};
        std::chrono::seconds retention{std::chrono::minutes{15}};
    };

    PlanStore();
    explicit PlanStore(Limits limits, Now now = [] { return Clock::now(); });
    // Reserve the complete preparation allowance before any source decoding.
    [[nodiscard]] Result<std::string> reserve(std::string owner);
    [[nodiscard]] Result<Record> publish(std::string_view id, std::string_view owner,
                                         std::shared_ptr<const Payload> payload);
    void abandon(std::string_view id, std::string_view owner);
    [[nodiscard]] Result<Record> get(std::string_view id, std::string_view owner);
    [[nodiscard]] Result<void> release(std::string_view id, std::string_view owner);
    using Submit = std::function<Result<std::string>()>;
    [[nodiscard]] Result<std::string> submit(std::string_view id, std::string_view owner,
                                             std::span<const std::string> acknowledgements, const Submit &submit_job);
    [[nodiscard]] Result<std::shared_ptr<const Payload>> start(std::string_view id, std::string_view owner);
    void finish(std::string_view id, std::string_view owner);
    void finish_job(std::string_view job_id, std::string_view owner);
    void collect_expired();

  private:
    void expire();
    Limits limits_;
    Now now_;
    std::mutex mutex_;
    std::unordered_map<std::string, Record> records_;
    std::unordered_map<std::string, std::string> pending_;
};

} // namespace axk::app::conversion_internal
