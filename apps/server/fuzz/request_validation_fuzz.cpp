#include <cstddef>
#include <cstdint>
#include <string_view>

#include "axklib/server/request_validation.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    constexpr std::size_t maximum_input_bytes = 1024U * 1024U;
    if (data == nullptr || size > maximum_input_bytes)
        return 0;

    axk::server::Config config;
    const auto body = std::string_view{reinterpret_cast<const char *>(data), size};
    static_cast<void>(axk::server::parse_json_request(body, config));
    return 0;
}
