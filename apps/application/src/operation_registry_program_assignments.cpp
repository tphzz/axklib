#include "operation_registry_program_assignments.hpp"

namespace axk::app {

const std::array<OperationDescriptor, 2U> &program_assignment_cleanup_descriptors() {
    static const std::array descriptors{
        OperationDescriptor{"images.program_assignments.cleanup.inspect",
                            {},
                            HttpMethod::post,
                            "/api/v1/image-program-assignment-cleanup-inspections",
                            ExecutionMode::request,
                            {},
                            "ImageProgramAssignmentCleanupInspectionRequest",
                            "ImageProgramAssignmentCleanupInspection",
                            OperationClass::read,
                            false,
                            false},
        OperationDescriptor{"images.program_assignments.cleanup",
                            {},
                            HttpMethod::post,
                            "/api/v1/image-program-assignment-cleanups",
                            ExecutionMode::job,
                            {},
                            "ImageProgramAssignmentCleanupRequest",
                            "ImageProgramAssignmentCleanupResult",
                            OperationClass::write,
                            true,
                            false},
    };
    return descriptors;
}

} // namespace axk::app
