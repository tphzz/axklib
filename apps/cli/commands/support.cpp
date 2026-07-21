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

Result<std::vector<std::filesystem::path>> expand_cli_paths(const std::vector<std::filesystem::path> &inputs) {
    static const std::set<std::string> extensions{".hda", ".hds", ".ima", ".img", ".iso"};
    std::vector<std::filesystem::path> result;
    const auto traversal_error = [](const std::filesystem::path &path) {
        return make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                          "could not completely scan input path: " + text::path_to_utf8(path));
    };
    for (const auto &path : inputs) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error)
            return std::unexpected{traversal_error(path)};
        if (std::filesystem::is_directory(status)) {
            std::filesystem::recursive_directory_iterator it{path, std::filesystem::directory_options::none, error};
            if (error)
                return std::unexpected{traversal_error(path)};
            const std::filesystem::recursive_directory_iterator end;
            while (it != end) {
                const auto entry_path = it->path();
                const auto regular = it->is_regular_file(error);
                if (error)
                    return std::unexpected{traversal_error(entry_path)};
                if (regular && extensions.contains(axk::text::path_to_utf8(entry_path.extension())))
                    result.push_back(it->path());
                it.increment(error);
                if (error)
                    return std::unexpected{traversal_error(entry_path)};
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
