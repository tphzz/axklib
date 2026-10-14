#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "axklib/conversion/plan.hpp"
#include "axklib/publication.hpp"

namespace axk::conversion {

struct FileRequest {
    std::string profile_id;
    std::vector<std::filesystem::path> sources;
    std::filesystem::path destination;
    nlohmann::json selection;
    nlohmann::json options;
    Fidelity fidelity{Fidelity::best};
    VolumeCapacityPolicy capacity_policy;
};

// File clients retain the reviewed payload in-process and publish only to a new output.
class FilePlan final {
  public:
    static Result<FilePlan> prepare(const FileRequest &request, const CancellationToken &cancellation = {},
                                    ProgressSink *progress = nullptr);
    FilePlan(FilePlan &&) noexcept;
    FilePlan &operator=(FilePlan &&) noexcept;
    FilePlan(const FilePlan &) = delete;
    FilePlan &operator=(const FilePlan &) = delete;
    ~FilePlan();
    [[nodiscard]] const Report &report() const noexcept;
    [[nodiscard]] Result<PublicationOutcome> apply(const std::filesystem::path &output,
                                                   std::span<const std::string> acknowledged,
                                                   const CancellationToken &cancellation = {},
                                                   ProgressSink *progress = nullptr);

  private:
    struct Impl;
    explicit FilePlan(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] nlohmann::json report_json(const Report &report);
[[nodiscard]] nlohmann::json profiles_json();

} // namespace axk::conversion
