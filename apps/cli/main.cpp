#include "app.hpp"
#include "command_line.hpp"
#include "exit_status.hpp"

#ifdef _WIN32
#include <iostream>
#include <vector>

int wmain(int argc, wchar_t **argv) {
    auto utf8 = axk::cli::platform::normalize_windows_command_line({argv, static_cast<std::size_t>(argc)});
    if (!utf8) {
        std::cerr << "error: " << utf8.error().message << '\n';
        return axk::cli::exit_code(axk::cli::ExitStatus::invalid_request);
    }
    std::vector<char *> pointers;
    pointers.reserve(utf8->size());
    for (auto &value : *utf8)
        pointers.push_back(value.data());
    return axk::cli::run(static_cast<int>(pointers.size()), pointers.data());
}
#else
#include <csignal>

int main(int argc, char **argv) {
    std::signal(SIGPIPE, SIG_IGN);
    return axk::cli::run(argc, argv);
}
#endif
