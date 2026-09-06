#include <array>
#include <cstdint>
#include <iostream>
#include <set>

#include <nlohmann/json.hpp>

#include "axklib/effects.hpp"

int main() {
    for (std::uint16_t raw_type = 0; raw_type <= 127U; ++raw_type) {
        for (std::uint8_t parameter = 1; parameter <= 16U; ++parameter) {
            if (!axk::effect_parameter_info(raw_type, parameter))
                continue;
            std::set<std::uint16_t> values;
            for (std::uint16_t value = 0; value <= 127U; ++value)
                values.insert(value);
            for (const auto value : std::array<std::uint16_t, 6>{255, 256, 884, 4600, 14800, 65535})
                values.insert(value);
            for (const auto raw_value : values) {
                const auto display = axk::format_effect_parameter(raw_type, parameter, raw_value);
                std::cout << nlohmann::ordered_json{
                         {"raw_type", raw_type},
                         {"parameter_number", parameter},
                         {"raw_value", raw_value},
                         {"value", display.value},
                         {"table_index", display.table_index},
                         {"quality", display.quality},
                         {"source", display.source},
                     }
                         .dump()
                  << '\n';
            }
        }
    }
    return 0;
}
