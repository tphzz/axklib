#include "axklib/application/image_session_operations.hpp"

#include "axklib/application/image_session_contracts.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"

axk::app::Result<void> axk::app::bind_image_session_operations(OperationRegistry &registry,
                                                               ImageSessionManager &images) {
    return registry.bind(
        "images.open",
        [&images](const nlohmann::json &input, const OperationContext &context) -> Result<nlohmann::json> {
            const auto source = input.find("source");
            if (source == input.end())
                return std::unexpected(Error{"invalid_request", "image source is required"});
            auto parsed_source = image_source_ref_from_json(*source);
            if (!parsed_source)
                return std::unexpected(parsed_source.error());
            auto opened = images.open(*parsed_source, context.owner_id, context.cancellation, context.progress);
            if (!opened)
                return std::unexpected(opened.error());
            return image_session_summary_json(*opened);
        });
}
