#include "axklib/sdk/conversion.hpp"

#include <utility>

#include "axklib/conversion/file_plan.hpp"
#include "sdk_internal.hpp"

namespace axk {
using namespace sdk_internal;
result<std::string> conversion_profiles_json() noexcept {
    return protect<std::string>([] { return conversion::profiles_json().dump(); });
}
struct AXK_SDK_HIDDEN conversion_plan::impl {
    conversion::FilePlan plan;
};
conversion_plan::conversion_plan(std::unique_ptr<impl> value) noexcept : impl_(std::move(value)) {}
conversion_plan::conversion_plan(conversion_plan &&) noexcept = default;
conversion_plan &conversion_plan::operator=(conversion_plan &&) noexcept = default;
conversion_plan::~conversion_plan() = default;

result<conversion_plan> conversion_plan::prepare(const conversion_request &request,
                                                 operation_context &context) noexcept {
    return protect<conversion_plan>([&]() -> result<conversion_plan> {
        conversion::FileRequest input;
        input.profile_id = request.profile_id;
        auto path = checked_path(request.destination_path, "conversion destination");
        if (!path)
            return path.error();
        input.destination = *path;
        for (const auto &source : request.source_paths) {
            auto source_path = checked_path(source, "conversion source");
            if (!source_path)
                return source_path.error();
            input.sources.push_back(*source_path);
        }
        try {
            input.selection = nlohmann::json::parse(request.destination_selection_json);
            input.options = nlohmann::json::parse(request.options_json);
        } catch (const nlohmann::json::exception &) {
            return invalid_argument("Conversion options and destination selection must be valid JSON");
        }
        input.fidelity = request.strict ? conversion::Fidelity::strict : conversion::Fidelity::best;
        auto plan = conversion::FilePlan::prepare(input, context.impl_->cancellation.token(), context.impl_.get());
        if (!plan)
            return public_error(plan.error());
        return conversion_plan{std::make_unique<impl>(impl{std::move(*plan)})};
    });
}

result<std::string> conversion_plan::report_json() const noexcept {
    return protect<std::string>([&]() -> result<std::string> {
        if (!impl_)
            return invalid_argument("Conversion plan has been moved");
        return conversion::report_json(impl_->plan.report()).dump();
    });
}

result<std::string> conversion_plan::apply(const std::string &output_path,
                                           const std::vector<std::string> &acknowledged_issue_ids,
                                           operation_context &context) noexcept {
    return protect<std::string>([&]() -> result<std::string> {
        if (!impl_)
            return invalid_argument("Conversion plan has been moved");
        const auto path = checked_path(output_path, "conversion output");
        if (!path)
            return path.error();
        const auto result =
            impl_->plan.apply(*path, acknowledged_issue_ids, context.impl_->cancellation.token(), context.impl_.get());
        if (!result)
            return public_error(result.error());
        auto warnings = nlohmann::json::array();
        for (const auto &warning : result->warnings)
            warnings.push_back({{"code", warning.code}, {"message", warning.message}});
        return nlohmann::json{
            {"applied", true},
            {"durability", result->durability == PublicationDurability::confirmed ? "CONFIRMED" : "UNCONFIRMED"},
            {"warnings", std::move(warnings)}}
            .dump();
    });
}
} // namespace axk
