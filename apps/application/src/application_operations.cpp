#include "axklib/application/application_operations.hpp"

#include "axklib/application/extraction_operations.hpp"
#include "axklib/application/file_operations.hpp"
#include "axklib/application/package_operations.hpp"
#include "axklib/application/validation_operations.hpp"
#include "axklib/application/write_operations.hpp"

axk::app::Result<void> axk::app::bind_application_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                             UploadStore &uploads) {
    if (auto bound = bind_file_operations(registry, sandbox); !bound)
        return bound;
    if (auto bound = bind_validation_operations(registry, sandbox); !bound)
        return bound;
    if (auto bound = bind_extraction_operations(registry, sandbox); !bound)
        return bound;
    if (auto bound = bind_package_operations(registry, sandbox, uploads); !bound)
        return bound;
    return bind_write_operations(registry, sandbox, uploads);
}

axk::app::Result<axk::app::OperationRegistry>
axk::app::make_application_registry(const Sandbox &sandbox, UploadStore &uploads, OperationRegistry registry) {
    if (auto bound = bind_application_operations(registry, sandbox, uploads); !bound)
        return std::unexpected(bound.error());
    return registry;
}
