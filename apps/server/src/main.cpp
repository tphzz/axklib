#include "diagnostic_log.hpp"
#include <exception>
#include <iostream>

#include "axklib/server/config.hpp"
#include "axklib/server/server.hpp"
#include "axklib/version.hpp"

int main(int argc, char **argv) {
    try {
        const auto command_line = axk::server::parse_command_line(argc, argv);
        if (!command_line) {
            axk::server::detail::write_startup_error(command_line.error().code, command_line.error().message);
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
            axk::server::detail::write_startup_error(result.error().code, result.error().message);
            return 2;
        }
        return *result;
    } catch (const std::exception &error) {
        axk::server::detail::write_startup_error("server_start_failed", error.what());
        return 2;
    } catch (...) {
        axk::server::detail::write_startup_error("server_start_failed", "unhandled server failure");
        return 2;
    }
}
