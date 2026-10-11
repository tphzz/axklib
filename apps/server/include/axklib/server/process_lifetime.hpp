#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace axk::server {

[[nodiscard]] bool process_is_running(std::uint64_t process_id) noexcept;
[[nodiscard]] bool desktop_shutdown_requested(const std::filesystem::path &connection_file,
                                              std::string_view bearer_token) noexcept;

} // namespace axk::server
