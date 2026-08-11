#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "support.hpp"

#include "../exit_status.hpp"

#include "axklib/error.hpp"
#include "axklib/media.hpp"
#include "axklib/utf8.hpp"

namespace axk::cli::commands {

Result<std::vector<std::filesystem::path>> expand_cli_paths(const std::vector<std::filesystem::path> &inputs,
                                                            bool include_object_directories) {
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
            if (include_object_directories) {
                const auto direct_media = axk::open_media(path);
                if (direct_media && direct_media->kind() == axk::MediaKind::axk_object_directory) {
                    result.push_back(path);
                    continue;
                }
            }
            std::filesystem::recursive_directory_iterator it{path, std::filesystem::directory_options::none, error};
            if (error)
                return std::unexpected{traversal_error(path)};
            const std::filesystem::recursive_directory_iterator end;
            while (it != end) {
                const auto entry_path = it->path();
                const auto directory = it->is_directory(error);
                if (error)
                    return std::unexpected{traversal_error(entry_path)};
                if (directory && include_object_directories) {
                    const auto media = axk::open_media(entry_path);
                    if (media && media->kind() == axk::MediaKind::axk_object_directory) {
                        result.push_back(entry_path);
                        it.disable_recursion_pending();
                    }
                }
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
