#include "axklib/application/operation_registry.hpp"

std::string_view axk::app::http_method_name(HttpMethod method) noexcept {
    switch (method) {
    case HttpMethod::get:
        return "GET";
    case HttpMethod::post:
        return "POST";
    }
    return "POST";
}

std::string_view axk::app::execution_mode_name(ExecutionMode mode) noexcept {
    switch (mode) {
    case ExecutionMode::request:
        return "request";
    case ExecutionMode::job:
        return "job";
    }
    return "request";
}

std::string_view axk::app::operation_class_name(OperationClass operation_class) noexcept {
    switch (operation_class) {
    case OperationClass::read:
        return "read";
    case OperationClass::write:
        return "write";
    }
    return "read";
}
