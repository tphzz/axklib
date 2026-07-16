#include <iostream>

#include "axklib/server/config.hpp"
#include "axklib/server/server.hpp"
#include "axklib/version.hpp"

int main(int argc, char **argv) {
    const auto command_line = axk::server::parse_command_line(argc, argv);
    if (!command_line) {
        std::cerr << "error: " << command_line.error().message << '\n';
        return 2;
    }
    if (command_line->print_help) {
        std::cout << axk::server::command_line_help();
        return 0;
    }
    if (command_line->print_version) {
        const auto build = axk::current_build_info();
        std::cout << "axklib-server " << axk::version() << " (" << build.source_identity << ")\n";
        return 0;
    }
    const auto result = axk::server::run(command_line->config);
    if (!result) {
        std::cerr << "error: " << result.error().message << '\n';
        return 2;
    }
    return *result;
}
