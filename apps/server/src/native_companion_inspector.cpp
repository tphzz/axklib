#include "native_companion_inspector.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

#include <nlohmann/json.hpp>

#include "axklib/filesystems/fat/view.hpp"
#include "axklib/formats/tx16w/dependencies.hpp"
#include "axklib/io.hpp"
#include "axklib/native/source.hpp"

namespace axk::server {

// Private stdin protocol: one bounded JSON line of basenames and lengths,
// followed by the exact snapshots from the desktop's authorized open handles.
// No filesystem path, HTTP listener or server state is needed.
int inspect_native_companions() {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
        return 2;
    }
#endif
    std::vector<char> line(2U * 1024U * 1024U);
    if (!std::cin.getline(line.data(), static_cast<std::streamsize>(line.size()))) {
        return 2;
    }
    const auto request = nlohmann::json::parse(line.data());
    const auto selected_count = request.at("selected").get<std::size_t>();
    const auto &files = request.at("files");
    if (!files.is_array() || selected_count == 0U || selected_count > native::SourceSet::maximum_members ||
        files.size() < selected_count || files.size() - selected_count > 1024U) {
        return 2;
    }
    std::vector<native::SourceMember> selected;
    std::vector<native::SourceMember> candidates;
    std::vector<std::shared_ptr<const tx16w::Document>> documents;
    std::size_t candidate_bytes{};
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto &file = files.at(index);
        const auto name = file.at("name").get<std::string>();
        const auto size = file.at("size").get<std::size_t>();
        if (name.empty() || name.size() > 1024U || name.find_first_of("/\\") != std::string::npos ||
            size > 4U * 1024U * 1024U) {
            return 2;
        }
        if (index >= selected_count && (candidate_bytes += size) > 64U * 1024U * 1024U) {
            return 2;
        }
        std::vector<std::byte> bytes(size);
        if (!std::cin.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size))) {
            return 2;
        }
        auto reader = std::make_shared<MemoryReader>(std::move(bytes));
        auto root = filesystems::open_fat(reader, name);
        if (!root) {
            if (index < selected_count) {
                return 2;
            }
            continue;
        }
        native::SourceMember member{std::to_string(index), name, std::move(reader), {std::move(*root)}};
        if (index < selected_count) {
            selected.push_back(std::move(member));
        } else {
            auto set = native::SourceSet::create({member});
            if (set) {
                auto document = tx16w::Document::open(*set);
                if (document) {
                    candidates.push_back(std::move(member));
                    documents.push_back(std::move(*document));
                }
            }
        }
    }
    auto set = native::SourceSet::create(selected);
    if (!set) {
        return 2;
    }
    auto seed = tx16w::Document::open(*set);
    if (!seed) {
        return 2;
    }
    auto group = tx16w::detail::resolve_companions(**seed, selected, candidates, documents);
    if (!group) {
        return 2;
    }
    std::vector<std::size_t> adopted;
    for (const auto &id : group->member_ids) {
        adopted.push_back(static_cast<std::size_t>(std::stoul(id)));
    }
    std::cout << nlohmann::json{{"adopted", adopted}, {"ambiguous", group->ambiguous_requirements}}.dump() << '\n';
    return 0;
}
} // namespace axk::server
