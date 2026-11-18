#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "axklib/conversion/plan.hpp"
#include "axklib/conversion/tx16w_to_a_series/source.hpp"

namespace axk::tx16w::a_series {
struct SourceSelection {
    std::vector<conversion::SourceObject> catalog;
    std::optional<std::set<std::string>> included;
};
Result<SourceSelection> select_source(const Document &, ImportMode, const nlohmann::json &, const conversion::Limits &);
void describe_blocker_owners(const Document &, conversion::Report &,
                             const std::optional<std::set<std::string>> &included);
} // namespace axk::tx16w::a_series
