#pragma once

#include <cstdint>

namespace axk::server {

[[nodiscard]] bool process_is_running(std::uint64_t process_id) noexcept;

} // namespace axk::server
