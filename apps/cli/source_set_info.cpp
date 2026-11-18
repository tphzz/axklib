#include "source_set_info.hpp"

#include <CLI/CLI.hpp>
#include <iostream>

#include "axklib/inspection/disk.hpp"
#include "exit_status.hpp"

namespace axk::cli {
void register_source_set_info(CLI::App &info, SourceSetInfo &state) {
    auto *flag = info.add_flag("--source-set", state.selected,
                               "inspect explicit disk members together as a native catalog (JSON)");
    for (const char *name :
         {"--format", "--strict", "--max-depth", "--show-quality", "--show-unresolved", "--show-default-programs"})
        flag->excludes(info.get_option(name));
    auto *document =
        info.add_option("--document", state.document, "native document ID from the source-set summary")->needs(flag);
    info.add_option("--collection", state.collection, "native document collection")
        ->check(CLI::IsMember({"objects", "relationships"}))
        ->needs(document);
    info.add_option("--offset", state.offset, "zero-based native collection offset")->needs(document);
    info.add_option("--limit", state.limit, "maximum native rows to display")
        ->check(CLI::Range(1, 1024))
        ->needs(document);
}
int run_source_set_info(const std::vector<std::filesystem::path> &paths, const SourceSetInfo &state) {
    const auto opened = inspection::DiskInspection::open(paths);
    if (!opened) {
        std::cerr << render_error(opened.error()) << '\n';
        return exit_code(core_error_status(opened.error()));
    }
    if (state.document.empty()) {
        std::cout << opened->summary().dump(2) << '\n';
        return exit_code(ExitStatus::success);
    }
    const auto page = state.collection == "objects" ? opened->objects(state.document, state.offset, state.limit)
                                                    : opened->relationships(state.document, state.offset, state.limit);
    if (!page) {
        std::cerr << render_error(page.error()) << '\n';
        return exit_code(core_error_status(page.error()));
    }
    std::cout << page->dump(2) << '\n';
    return exit_code(ExitStatus::success);
}
} // namespace axk::cli
