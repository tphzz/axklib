#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "axklib/formats/a_series/document.hpp"
#include "axklib/native/document.hpp"

namespace axk::formats::a_series {

// Binds the decoded owner to admitted occurrences without parsing or resolving again.
class NativeDocument final : public native::Document {
  public:
    using Locations = std::unordered_map<std::string, std::vector<native::SourceLocation>>;
    [[nodiscard]] static Result<std::shared_ptr<const NativeDocument>>
    create(std::shared_ptr<const a_series::Document> decoded, std::shared_ptr<const native::SourceSet> sources,
           std::string id, Locations locations = {});

    [[nodiscard]] const native::DocumentSummary &summary() const noexcept override { return summary_; }
    [[nodiscard]] const native::Catalog &catalog() const noexcept override { return catalog_; }
    [[nodiscard]] const std::shared_ptr<const native::SourceSet> &sources() const noexcept override { return sources_; }
    [[nodiscard]] const std::shared_ptr<const a_series::Document> &decoded() const noexcept { return decoded_; }

  private:
    NativeDocument(std::shared_ptr<const a_series::Document> decoded, std::shared_ptr<const native::SourceSet> sources,
                   native::DocumentSummary summary, native::Catalog catalog);
    std::shared_ptr<const a_series::Document> decoded_;
    std::shared_ptr<const native::SourceSet> sources_;
    native::DocumentSummary summary_;
    native::Catalog catalog_;
};

} // namespace axk::formats::a_series
