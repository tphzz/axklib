#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/native/document.hpp"
#include "axklib/volume_capacity.hpp"

namespace axk::conversion {

enum class Fidelity : std::uint8_t { best, strict };
enum class Impact : std::uint8_t { exact, representation, loss, uncertainty, blocker };

struct Issue {
    std::string id;
    std::string code;
    Impact impact{Impact::exact};
    std::string source_object;
    std::string source_parameter;
    std::string target_object;
    std::string target_parameter;
    std::string message;
};

struct Action {
    std::string kind;
    std::string name;
    std::string source_object;
    std::string action{"create"};
    struct Detail {
        std::string label;
        std::string value;
    };
    std::vector<Detail> details{};
};

struct Report {
    std::string profile_id;
    Fidelity fidelity{Fidelity::best};
    bool executable{};
    std::vector<Action> actions;
    std::vector<Issue> issues;
    VolumeCapacityAdmission capacity;
};

struct Patch {
    std::uint64_t offset{};
    std::vector<std::byte> original;
    std::vector<std::byte> replacement;
};

struct Limits {
    std::uint64_t maximum_bytes{512ULL * 1024ULL * 1024ULL};
    std::size_t maximum_actions{32768U};
    std::size_t maximum_issues{16384U};
};

struct Destination {
    std::string format_id;
    std::shared_ptr<const RandomAccessReader> reader;
    std::filesystem::path label;
    nlohmann::json selection;
};

struct Request {
    std::shared_ptr<const native::Document> source;
    Destination destination;
    nlohmann::json options;
    Fidelity fidelity{Fidelity::best};
    VolumeCapacityPolicy capacity_policy;
    Limits limits;
};

class PreparedPlan final {
  public:
    PreparedPlan(Report report, std::vector<Patch> patches, std::uint64_t destination_size,
                 std::uint64_t retained_bytes);
    [[nodiscard]] const Report &report() const noexcept;
    [[nodiscard]] std::span<const Patch> patches() const noexcept;
    [[nodiscard]] std::uint64_t destination_size() const noexcept;
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
    [[nodiscard]] Result<void> acknowledge(std::span<const std::string> issue_ids) const;

  private:
    Report report_;
    std::vector<Patch> patches_;
    std::uint64_t destination_size_{};
    std::uint64_t retained_bytes_{};
};

struct Profile {
    std::string id;
    std::string name;
    std::string source_format;
    std::string destination_format;
    nlohmann::json options_schema;
    using Prepare = std::function<Result<std::shared_ptr<const PreparedPlan>>(
        const Request &, const CancellationToken &, ProgressSink *)>;
    Prepare prepare;
};

class Registry final {
  public:
    [[nodiscard]] Result<void> add(Profile profile);
    [[nodiscard]] std::span<const Profile> profiles() const noexcept;
    [[nodiscard]] Result<std::shared_ptr<const PreparedPlan>> prepare(std::string_view profile_id,
                                                                      const Request &request,
                                                                      const CancellationToken &cancellation = {},
                                                                      ProgressSink *progress = nullptr) const;

  private:
    std::vector<Profile> profiles_;
};

[[nodiscard]] std::string_view impact_name(Impact impact) noexcept;

} // namespace axk::conversion
