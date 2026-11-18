#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axklib/native/registry.hpp"

namespace axk::inspection {

// Read-only catalog. No publication target or converter participates in inspection.
class DiskInspection final {
  public:
    [[nodiscard]] static Result<DiskInspection> open(std::span<const std::filesystem::path> paths,
                                                     const CancellationToken &cancellation = {});
    [[nodiscard]] static Result<DiskInspection> inspect(std::shared_ptr<const native::SourceSet> sources,
                                                        const native::FormatRegistry &formats,
                                                        const CancellationToken &cancellation = {});
    [[nodiscard]] nlohmann::json summary() const;
    [[nodiscard]] Result<nlohmann::json> objects(std::string_view document, std::size_t offset, std::size_t limit,
                                                 const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<nlohmann::json> relationships(std::string_view document, std::size_t offset, std::size_t limit,
                                                       const CancellationToken &cancellation = {}) const;

  private:
    [[nodiscard]] Result<const native::Document *> find(std::string_view id) const;
    std::shared_ptr<const native::SourceSet> sources_;
    native::OpenResult opened_;
};
} // namespace axk::inspection
