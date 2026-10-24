#include "axklib/application/operation_registry.hpp"

#include <utility>

namespace axk::app {
Result<void> OperationRegistry::bind_read_scheduling(std::string_view operation_id, ReadSchedulingResolver resolver) {
    const auto found = by_id_.find(std::string{operation_id});
    if (found == by_id_.end())
        return std::unexpected(Error{"unknown_operation", "operation is not registered"});
    auto &operation = operations_[found->second];
    if (operation.descriptor.operation_class != OperationClass::read ||
        operation.descriptor.mode != ExecutionMode::job || !resolver)
        return std::unexpected(Error{"invalid_read_scheduling", "only read jobs can have a scheduling resolver"});
    if (operation.read_scheduling_resolver)
        return std::unexpected(Error{"duplicate_read_scheduling", "read scheduling is already bound"});
    operation.read_scheduling_resolver = std::move(resolver);
    return {};
}

Result<std::optional<ReadJobScheduling>> OperationRegistry::read_scheduling(std::string_view operation_id,
                                                                            const Json &input,
                                                                            const OperationContext &context) const {
    const auto found = by_id_.find(std::string{operation_id});
    if (found == by_id_.end())
        return std::unexpected(Error{"unknown_operation", "operation is not registered"});
    const auto &resolver = operations_[found->second].read_scheduling_resolver;
    if (!resolver)
        return std::optional<ReadJobScheduling>{};
    try {
        auto scheduling = resolver(input, context);
        if (scheduling && *scheduling &&
            ((**scheduling).scope.empty() || (**scheduling).scope.size() > 512U ||
             (**scheduling).intent > ReadIntent::prefetch))
            return std::unexpected(Error{"invalid_read_scheduling", "invalid read scheduling scope or intent"});
        return scheduling;
    } catch (const nlohmann::json::exception &) {
        return std::unexpected(Error{"invalid_request", "read job does not match its request contract"});
    }
}
} // namespace axk::app
