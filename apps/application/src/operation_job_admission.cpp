#include "axklib/application/operation_registry.hpp"

namespace axk::app {
Result<void> OperationRegistry::bind_job_admission(std::string_view id, JobAdmission admission) {
    const auto found = by_id_.find(std::string{id});
    if (found == by_id_.end())
        return std::unexpected(Error{"unknown_operation", "operation is not registered"});
    auto &operation = operations_[found->second];
    if (!admission || operation.descriptor.mode != ExecutionMode::job ||
        operation.descriptor.operation_class != OperationClass::write || !operation.descriptor.requires_idempotency)
        return std::unexpected(Error{"invalid_job_admission", "admission requires an idempotent write job"});
    if (operation.job_admission)
        return std::unexpected(Error{"duplicate_job_admission", "job admission is already bound"});
    operation.job_admission = std::move(admission);
    return {};
}

Result<std::shared_ptr<void>> OperationRegistry::admit_job(std::string_view id, const Json &input,
                                                           const OperationContext &context) const {
    const auto found = by_id_.find(std::string{id});
    if (found == by_id_.end())
        return std::unexpected(Error{"unknown_operation", "operation is not registered"});
    const auto &admission = operations_[found->second].job_admission;
    if (!admission)
        return std::shared_ptr<void>{};
    try {
        return admission(input, context);
    } catch (const nlohmann::json::exception &) {
        return std::unexpected(Error{"invalid_request", "job does not match its admission contract"});
    }
}
} // namespace axk::app
