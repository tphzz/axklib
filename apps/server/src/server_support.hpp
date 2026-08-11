#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

#include <crow.h>
#include <nlohmann/json.hpp>

#include "axklib/application/contracts.hpp"
#include "axklib/application/uploads.hpp"
#include "axklib/server/config.hpp"

namespace axk::server::detail {

using Json = nlohmann::json;

constexpr std::size_t maximum_cursor_length = 512U;
constexpr std::string_view event_subprotocol{"axklib.events.v1"};

struct ByteRange {
    std::uint64_t offset{};
    std::uint64_t length{};
};

std::function<void(const Json &)> operation_diagnostic_sink();
bool writable_directory(const std::filesystem::path &directory);
bool cleanup_complete(const std::filesystem::path &directory);

app::Result<Json> parse_json_body(const crow::request &request, const Config &config);
crow::response json_response(int status, const Json &body, std::string_view request_id);
crow::response error_response(int status, const app::Error &error, std::string_view request_id);
int status_for_error(const app::Error &error, int fallback = 422);

bool requests_subprotocol(const crow::request &request, std::string_view expected);
std::optional<std::uint64_t> parse_sequence(const char *text);
std::optional<std::uint64_t> parse_unsigned(std::string_view value);
std::optional<app::UploadKind> parse_upload_kind(std::string_view value);
Json upload_json(const app::UploadSnapshot &upload);
std::optional<ByteRange> parse_byte_range(std::string_view value, std::uint64_t file_size, std::size_t maximum_length);

} // namespace axk::server::detail
