#include "validation_operations_internal.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/semantic.hpp"
#include "axklib/utf8.hpp"

namespace axk::app::validation_operations_internal {
namespace {

std::string public_object_key(const ValidationSource &source, std::string_view native_key) {
    if (source.media.kind() == axk::MediaKind::sfs)
        return std::string{native_key};
    const auto found = std::ranges::find(source.inventory.objects, native_key, &axk::MediaObjectDescriptor::key);
    if (found == source.inventory.objects.end())
        return std::string{native_key};
    const auto filename = axk::text::path_to_utf8(source.path.filename());
    if (source.media.kind() == axk::MediaKind::fat12_floppy)
        return std::format("{}:{}", filename, found->logical_path);
    if (source.media.kind() == axk::MediaKind::iso9660)
        return std::format("{}:iso9660:{}", filename, found->logical_path);
    return std::format("{}:standalone-object", filename);
}

} // namespace

std::vector<axk::ReportRow> validate_media_details(const ValidationSource &source) {
    std::vector<axk::ReportRow> issues;
    const auto validation = axk::validate_semantics(source.media, source.inventory, source.graph);
    issues.reserve(validation.issues.size());
    for (const auto &issue : validation.issues) {
        const auto severity = issue.severity == axk::ValidationSeverity::error     ? "error"
                              : issue.severity == axk::ValidationSeverity::warning ? "warning"
                                                                                   : "info";
        issues.push_back({{"severity", severity},
                          {"code", issue.code},
                          {"message", issue.message},
                          {"scope", issue.scope},
                          {"source_path", axk::text::path_to_utf8(source.path)},
                          {"sampler_path", issue.sampler_path},
                          {"object_key", public_object_key(source, issue.object_key)},
                          {"quality", issue.quality},
                          {"basis", issue.basis},
                          {"recommended_next_check", issue.recommended_next_check}});
    }
    return issues;
}

} // namespace axk::app::validation_operations_internal
