#include <charconv>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axklib/application/operation_registry.hpp"
#include "axklib/application/system_service.hpp"

namespace {

std::size_t parse_iterations(std::string_view text) {
    std::size_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result == 0U)
        return 0U;
    return result;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: axk_server_direct_benchmark ITERATIONS\n";
        return 2;
    }
    const auto iterations = parse_iterations(argv[1]);
    if (iterations == 0U) {
        std::cerr << "iterations must be a positive integer\n";
        return 2;
    }

    const auto registry = axk::app::make_operation_registry();
    for (std::size_t index = 0; index < 100U; ++index) {
        if (!axk::app::registered_system_version(registry))
            return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < iterations; ++index) {
        if (!axk::app::registered_system_version(registry))
            return 1;
    }
    const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started).count();
    const auto mean_nanoseconds = elapsed / static_cast<double>(iterations);
    const nlohmann::ordered_json report{{"schemaVersion", "1.0"},
                                        {"operation", "system.version"},
                                        {"iterations", iterations},
                                        {"meanNanoseconds", mean_nanoseconds},
                                        {"operationsPerSecond", 1'000'000'000.0 / mean_nanoseconds}};
    std::cout << report.dump() << '\n';
    return 0;
}
