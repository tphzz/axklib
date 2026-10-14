#pragma once

#include <array>

#include "axklib/application/operation_registry.hpp"

namespace axk::app {
inline const std::array<OperationDescriptor, 2U> &conversion_descriptors() {
    static const std::array descriptors{OperationDescriptor{"conversions.prepare",
                                                            "axklib convert",
                                                            HttpMethod::post,
                                                            "/api/v1/conversion-plans",
                                                            ExecutionMode::job,
                                                            {},
                                                            "ConversionPlanRequest",
                                                            "ConversionPlan",
                                                            OperationClass::read,
                                                            false,
                                                            true},
                                        OperationDescriptor{"conversions.execute",
                                                            {},
                                                            HttpMethod::post,
                                                            "/api/v1/conversion-plans/{planId}/executions",
                                                            ExecutionMode::job,
                                                            {},
                                                            "ConversionExecutionRequest",
                                                            "ConversionResult",
                                                            OperationClass::write,
                                                            true,
                                                            false}};
    return descriptors;
}
} // namespace axk::app
