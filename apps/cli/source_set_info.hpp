#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace CLI {
class App;
}
namespace axk::cli {
struct SourceSetInfo {
    bool selected{};
    std::string document;
    std::string collection{"objects"};
    std::size_t offset{};
    std::size_t limit{100U};
};
void register_source_set_info(CLI::App &info, SourceSetInfo &state);
int run_source_set_info(const std::vector<std::filesystem::path> &paths, const SourceSetInfo &state);
} // namespace axk::cli
