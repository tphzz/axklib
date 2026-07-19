#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/contracts.hpp"
#include "axklib/application/path_reservations.hpp"
#include "axklib/io.hpp"

namespace axk::app {

enum class HttpMethod : std::uint8_t { get, post };
enum class ExecutionMode : std::uint8_t { request, job };
enum class OperationClass : std::uint8_t { read, write };

struct OperationDescriptor {
    std::string id;
    std::string cli_command;
    HttpMethod method{HttpMethod::post};
    std::string route;
    ExecutionMode mode{ExecutionMode::request};
    std::string variant;
    std::string request_schema;
    std::string result_schema;
    OperationClass operation_class{OperationClass::read};
    bool requires_idempotency{};
    bool cli_parity{true};
};

struct OperationContext {
    std::string owner_id;
    std::string request_id;
    CancellationToken cancellation;
    ProgressSink *progress{};
    std::function<std::string(const FileRef &)> display_path;
};

struct OperationEntry {
    OperationDescriptor descriptor;
    bool implemented{};
    std::size_t handler_type_hash{};
};

class OperationRegistry {
  public:
    using Json = nlohmann::json;
    using Handler = std::function<Result<Json>(const Json &, const OperationContext &)>;
    using PathAccessResolver = std::function<Result<std::vector<PathAccess>>(const Json &, const OperationContext &)>;

    Result<void> declare(OperationDescriptor descriptor);
    Result<void> bind(std::string_view operation_id, Handler handler);
    Result<void> bind_path_accesses(std::string_view operation_id, PathAccessResolver resolver);

    template <typename Request, typename Response, typename Callable>
    Result<void> bind_typed(std::string_view operation_id, Callable handler) {
        return bind(operation_id,
                    [handler = std::move(handler)](const Json &input, const OperationContext &context) -> Result<Json> {
                        try {
                            const auto request = input.template get<Request>();
                            auto response = std::invoke(handler, request, context);
                            if (!response)
                                return std::unexpected(response.error());
                            Json output = *response;
                            return output;
                        } catch (const nlohmann::json::exception &) {
                            return std::unexpected(
                                Error{"invalid_request", "request does not match the operation schema"});
                        }
                    });
    }

    [[nodiscard]] Result<Json> invoke(std::string_view operation_id, const Json &input,
                                      const OperationContext &context) const;
    [[nodiscard]] Result<std::vector<PathAccess>> path_accesses(std::string_view operation_id, const Json &input,
                                                                const OperationContext &context) const;
    [[nodiscard]] std::vector<OperationEntry> entries() const;
    [[nodiscard]] const OperationDescriptor *find(std::string_view operation_id) const noexcept;
    [[nodiscard]] bool is_implemented(std::string_view operation_id) const noexcept;

  private:
    struct RegisteredOperation {
        OperationDescriptor descriptor;
        Handler handler;
        PathAccessResolver path_access_resolver;
        std::size_t handler_type_hash{};
    };

    std::vector<RegisteredOperation> operations_;
    std::unordered_map<std::string, std::size_t> by_id_;
};

[[nodiscard]] OperationRegistry make_operation_registry();
[[nodiscard]] std::string_view http_method_name(HttpMethod method) noexcept;
[[nodiscard]] std::string_view execution_mode_name(ExecutionMode mode) noexcept;
[[nodiscard]] std::string_view operation_class_name(OperationClass operation_class) noexcept;

} // namespace axk::app
