#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "support.hpp"

#include "../exit_status.hpp"

#include "axklib/error.hpp"
#include "axklib/utf8.hpp"

namespace axk::cli::commands {

std::vector<std::filesystem::path> expand_cli_paths(const std::vector<std::filesystem::path> &inputs) {
    static const std::set<std::string> extensions{".hda", ".hds", ".ima", ".img", ".iso"};
    std::vector<std::filesystem::path> result;
    for (const auto &path : inputs) {
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
            for (std::filesystem::recursive_directory_iterator it{path, error}, end; it != end && !error;
                 it.increment(error)) {
                if (it->is_regular_file(error) && extensions.contains(axk::text::path_to_utf8(it->path().extension())))
                    result.push_back(it->path());
            }
        } else {
            result.push_back(path);
        }
    }
    std::ranges::sort(result, {}, [](const auto &path) { return axk::text::path_to_utf8(path); });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

int report_failure(const axk::Error &error) {
    std::cerr << axk::render_error(error) << '\n';
    return axk::cli::exit_code(axk::cli::core_error_status(error));
}

} // namespace axk::cli::commands
