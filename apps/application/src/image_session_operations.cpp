#include "axklib/application/image_session_operations.hpp"

#include "axklib/application/image_session_contracts.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/application/volume_capacity.hpp"
#include "operation_diagnostics.hpp"

axk::app::Result<void> axk::app::bind_image_session_operations(OperationRegistry &registry,
                                                               ImageSessionManager &images) {
    const auto bound = registry.bind(
        "images.open",
        [&images](const nlohmann::json &input, const OperationContext &context) -> Result<nlohmann::json> {
            auto parsed_source = image_open_sources_from_json(input);
            if (!parsed_source)
                return std::unexpected(parsed_source.error());
            OperationDiagnosticSink diagnostics{context};
            auto opened =
                images.open(*parsed_source, context.owner_id, context.cancellation, context.progress, &diagnostics);
            if (!opened)
                return std::unexpected(opened.error());
            return image_session_summary_json(*opened);
        });
    if (!bound)
        return bound;
    return bind_volume_capacity_operations(registry, images);
}
