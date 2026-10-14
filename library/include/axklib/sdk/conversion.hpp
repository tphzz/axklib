#pragma once

#include <memory>
#include <string>
#include <vector>

#include "axklib/sdk/export.hpp"
#include "axklib/sdk/result.hpp"

namespace axk {
class operation_context;
[[nodiscard]] AXK_SDK_API result<std::string> conversion_profiles_json() noexcept;

struct conversion_request {
    std::string profile_id;
    std::vector<std::string> source_paths;
    std::string destination_path;
    std::string destination_selection_json;
    std::string options_json;
    bool strict{};
};

class conversion_plan final {
  public:
    [[nodiscard]] static AXK_SDK_API result<conversion_plan> prepare(const conversion_request &request,
                                                                     operation_context &context) noexcept;
    AXK_SDK_API conversion_plan(conversion_plan &&) noexcept;
    AXK_SDK_API conversion_plan &operator=(conversion_plan &&) noexcept;
    conversion_plan(const conversion_plan &) = delete;
    conversion_plan &operator=(const conversion_plan &) = delete;
    AXK_SDK_API ~conversion_plan();
    [[nodiscard]] AXK_SDK_API result<std::string> report_json() const noexcept;
    // Publishes a new file only. An accepted attempt consumes the plan even on failure/cancellation.
    // The result JSON contains publication durability and any post-publication warnings.
    [[nodiscard]] AXK_SDK_API result<std::string> apply(const std::string &output_path,
                                                        const std::vector<std::string> &acknowledged_issue_ids,
                                                        operation_context &context) noexcept;

  private:
    struct impl;
    explicit conversion_plan(std::unique_ptr<impl> value) noexcept;
    std::unique_ptr<impl> impl_;
};
} // namespace axk
