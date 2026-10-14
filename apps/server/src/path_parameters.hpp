#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

#include "axklib/utf8.hpp"
#include "server_support.hpp"
#include "transport.hpp"

namespace axk::server::detail {

inline bool decode_path_parameter(std::string &value) {
    const auto hex = [](char character) {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        if (character >= 'A' && character <= 'F')
            return character - 'A' + 10;
        return -1;
    };
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        char character = value[index];
        if (character == '%') {
            if (value.size() - index < 3U)
                return false;
            const auto high = hex(value[index + 1U]);
            const auto low = hex(value[index + 2U]);
            if (high < 0 || low < 0)
                return false;
            character = static_cast<char>(high * 16 + low);
            index += 2U;
        }
        if (static_cast<unsigned char>(character) < 0x20U || character == 0x7f)
            return false;
        decoded.push_back(character);
    }
    if (!axk::text::is_valid_utf8(decoded))
        return false;
    value = std::move(decoded);
    return true;
}

template <typename... Parameters>
std::function<crow::response(const crow::request &, Parameters...)>
decoded_path_route(std::function<crow::response(const crow::request &, Parameters...)> route) {
    return [handler = std::move(route)](const crow::request &request, Parameters... parameters) {
        if (!(decode_path_parameter(parameters) && ...))
            return error_response(400, {"invalid_path_parameter", "Resource ID has invalid percent encoding or text"},
                                  request_id(request));
        return handler(request, std::move(parameters)...);
    };
}

} // namespace axk::server::detail
