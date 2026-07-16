#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace axk::app {

struct FileRef {
    std::string root_id;
    std::string relative_path;

    friend bool operator==(const FileRef &, const FileRef &) = default;
};

struct DirectoryRef {
    std::string root_id;
    std::string relative_path;

    friend bool operator==(const DirectoryRef &, const DirectoryRef &) = default;
};

struct UploadRef {
    std::string upload_id;

    friend bool operator==(const UploadRef &, const UploadRef &) = default;
};

struct Warning {
    std::string code;
    std::string message;
    std::optional<std::string> sampler_path;
};

struct ErrorContext {
    std::optional<std::uint32_t> partition_index;
    std::optional<std::string> volume_name;
    std::optional<std::string> object_type;
    std::optional<std::string> object_name;
    std::optional<std::string> relative_path;
};

struct Error {
    std::string code;
    std::string message;
    ErrorContext context;
    bool retryable{};

    Error() = default;
    Error(std::string error_code, std::string error_message, ErrorContext error_context = {}, bool can_retry = false)
        : code(std::move(error_code)), message(std::move(error_message)), context(std::move(error_context)),
          retryable(can_retry) {}
};

template <typename T> using Result = std::expected<T, Error>;

} // namespace axk::app
