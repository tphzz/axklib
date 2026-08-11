#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#include "axklib/error.hpp"
#include "axklib/io.hpp"
#include "axklib/sdk.hpp"

namespace axk {

struct AXK_SDK_HIDDEN operation_context::impl final : ProgressSink {
    CancellationSource cancellation;
    std::mutex mutex;
    progress_sink *destination{};

    void report(const Progress &progress) noexcept override;
};

namespace sdk_internal {

error public_error(const Error &failure);
error invalid_argument(std::string message);
error internal_error(std::string message);

template <typename T, typename Function> result<T> protect(Function &&function) noexcept {
    try {
        return std::forward<Function>(function)();
    } catch (const std::bad_alloc &) {
        return internal_error("native allocation failed");
    } catch (const std::exception &exception) {
        return internal_error(exception.what());
    } catch (...) {
        return internal_error("unexpected native exception");
    }
}

result<std::filesystem::path> checked_path(const std::string &value, std::string label);

} // namespace sdk_internal
} // namespace axk
