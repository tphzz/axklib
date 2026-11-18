#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "axklib/sdk/export.hpp"
#include "axklib/sdk/result.hpp"

namespace axk {
class operation_context;

// Read-only native catalog for an explicitly selected set of disk members.
class source_set final {
  public:
    [[nodiscard]] static AXK_SDK_API result<source_set> open(const std::vector<std::string> &paths,
                                                             operation_context &context) noexcept;
    AXK_SDK_API source_set(source_set &&) noexcept;
    AXK_SDK_API source_set &operator=(source_set &&) noexcept;
    AXK_SDK_API ~source_set();
    source_set(const source_set &) = delete;
    source_set &operator=(const source_set &) = delete;
    [[nodiscard]] AXK_SDK_API result<std::string> summary_json() const noexcept;
    [[nodiscard]] AXK_SDK_API result<std::string> objects_json(const std::string &document_id, std::size_t offset,
                                                               std::size_t limit,
                                                               operation_context &context) const noexcept;
    [[nodiscard]] AXK_SDK_API result<std::string> relationships_json(const std::string &document_id, std::size_t offset,
                                                                     std::size_t limit,
                                                                     operation_context &context) const noexcept;

  private:
    struct impl;
    explicit source_set(std::unique_ptr<impl> value) noexcept;
    std::unique_ptr<impl> impl_;
};
} // namespace axk
