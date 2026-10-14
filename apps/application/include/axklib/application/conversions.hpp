#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axklib/application/jobs.hpp"

namespace axk::app {
class Sandbox;
class UploadStore;
class ImageSessionManager;
class AlterationJournalStore;

class ConversionService final {
  public:
    ConversionService(const Sandbox &sandbox, UploadStore &uploads, ImageSessionManager &images,
                      AlterationJournalStore &journals, JobManager &jobs);
    ~ConversionService();
    ConversionService(const ConversionService &) = delete;
    ConversionService &operator=(const ConversionService &) = delete;
    [[nodiscard]] Result<void> bind(OperationRegistry &registry);
    [[nodiscard]] nlohmann::json profiles() const;
    [[nodiscard]] Result<nlohmann::json> plan(std::string_view id, std::string_view owner, std::size_t offset = 0U,
                                              std::size_t limit = 200U);
    [[nodiscard]] Result<void> release(std::string_view id, std::string_view owner);
    [[nodiscard]] Result<JobSnapshot> execute(std::string_view id, const nlohmann::json &request,
                                              OperationContext context);

  private:
    struct Implementation;
    std::shared_ptr<Implementation> implementation_;
};

} // namespace axk::app
