#pragma once

#include <string>
#include <string_view>

namespace axk::server {

[[nodiscard]] std::string attachment_content_disposition(std::string_view filename);

} // namespace axk::server
