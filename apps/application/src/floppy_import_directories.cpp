#include "floppy_import_directories.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/floppy_catalog_internal.hpp"

#include "content_digest.hpp"
#include "filesystem_inputs.hpp"
#include "package_operations_internal.hpp"

namespace axk::app {
using package_operations_internal::core_error;
namespace {
constexpr SandboxTreeLimits limits{AxkObjectDirectory::maximum_entries, AxkObjectDirectory::maximum_payload_bytes, 2U,
                                   64U * 1024U};
using Json = nlohmann::json;
Json tree_identity(const SandboxTree &tree) {
    Json identity = Json::array();
    for (const auto &entry : tree.entries())
        identity.push_back({entry.relative_path, entry.kind == SandboxTreeEntryKind::file, entry.size});
    std::sort(identity.begin(), identity.end());
    return identity;
}
struct Retained {
    std::vector<filesystem_inputs::OpenedInput> inputs;
    std::vector<Json> snapshots;
    std::vector<std::pair<DirectoryRef, Json>> trees;
};
Error invalid(std::string message) { return {"floppy_source_invalid", std::move(message)}; }
} // namespace

Result<FloppyDirectorySources> open_floppy_directories(const Json &sources, const OperationContext &context,
                                                       const Sandbox &sandbox, UploadStore &uploads) {
    auto retained = std::make_shared<Retained>();
    FloppyDirectorySources result;
    std::set<std::pair<std::string, std::string>> files;
    std::uint64_t total_bytes{};
    for (const auto &source : sources) {
        const auto &reference = source.at("directoryRef");
        const DirectoryRef directory{reference.at("rootId").get<std::string>(),
                                     reference.at("relativePath").get<std::string>()};
        auto tree = sandbox.open_tree(directory, limits);
        if (!tree)
            return std::unexpected(tree.error());
        retained->trees.emplace_back(directory, tree_identity(*tree));
        std::map<std::string, std::vector<AxkObjectDirectoryEntry>, std::less<>> groups;
        for (std::size_t index = 0; index < tree->entries().size(); ++index) {
            if (auto checked = context.cancellation.check(); !checked)
                return std::unexpected(core_error(checked.error()));
            const auto &entry = tree->entries()[index];
            if (entry.kind != SandboxTreeEntryKind::file)
                continue;
            const auto separator = entry.relative_path.find('/');
            const auto parent =
                separator == std::string::npos ? std::string{} : entry.relative_path.substr(0, separator);
            const auto name =
                separator == std::string::npos ? entry.relative_path : entry.relative_path.substr(separator + 1U);
            if (name.find('/') != std::string::npos)
                return std::unexpected(invalid("Select a disk folder or its immediate parent."));
            const auto path = directory.relative_path.empty() ? entry.relative_path
                                                              : directory.relative_path + "/" + entry.relative_path;
            if (!files.emplace(directory.root_id, path).second)
                return std::unexpected(invalid("The same disk folder was selected more than once."));
            if (files.size() > limits.maximum_entries || entry.size > limits.maximum_total_file_bytes - total_bytes)
                return std::unexpected(invalid("The disk folders exceed the entry or payload limit."));
            total_bytes += entry.size;
            auto input = filesystem_inputs::open({{"fileRef", {{"rootId", directory.root_id}, {"relativePath", path}}}},
                                                 context.owner_id, sandbox, uploads);
            if (!input)
                return std::unexpected(input.error());
            auto snapshot = input->snapshot(context.cancellation);
            if (!snapshot)
                return std::unexpected(snapshot.error());
            if (input->reader->size() != entry.size)
                return std::unexpected(invalid("The disk folder changed during inspection."));
            std::vector<std::byte> bytes(static_cast<std::size_t>(entry.size));
            if (auto read = input->reader->read_exact_at(0U, bytes); !read)
                return std::unexpected(core_error(read.error()));
            auto reader = std::make_shared<MemoryReader>(std::move(bytes));
            const auto hash = detail::reader_sha256(*reader, context.cancellation);
            if (!hash)
                return std::unexpected(hash.error());
            if (*hash != snapshot->at("sha256").get<std::string>())
                return std::unexpected(invalid("The disk folder changed during inspection."));
            groups[parent].push_back({name, std::move(reader)});
            retained->inputs.push_back(std::move(*input));
            retained->snapshots.push_back(std::move(*snapshot));
        }
        auto direct = groups.find("");
        bool leaf = false;
        if (direct != groups.end()) {
            auto recognized =
                AxkObjectDirectory::recognizes(direct->second, directory.relative_path, context.cancellation);
            if (!recognized)
                return std::unexpected(core_error(recognized.error()));
            leaf = *recognized || std::ranges::any_of(direct->second, [](const auto &entry) {
                return axk::detail::is_yamaha_floppy_catalog_path(entry.name);
            });
        }
        if (leaf) {
            if (groups.size() != 1U)
                return std::unexpected(
                    invalid("This folder mixes disk contents and nested folders; select one disk source."));
            result.members.push_back({directory.relative_path, std::move(direct->second)});
        } else {
            for (auto &[name, entries] : groups) {
                if (name.empty())
                    continue;
                auto member = AxkObjectDirectory::open(entries, name, context.cancellation);
                if (!member || !member->disk_identity().trusted_for_disk_set)
                    return std::unexpected(
                        invalid("Select a disk folder or a parent containing one catalog-identified companion set."));
                result.members.push_back({directory.relative_path + "/" + name, std::move(entries)});
            }
        }
        if (result.members.size() > FloppyDiskSet::maximum_members)
            return std::unexpected(invalid("Select at most 32 companion disk folders."));
    }
    if (result.members.empty())
        return std::unexpected(invalid("No unpacked A-series floppy disks were found in this folder."));
    result.verify = [retained, &sandbox](const CancellationToken &cancellation) -> Result<void> {
        for (const auto &[reference, expected] : retained->trees) {
            if (auto checked = cancellation.check(); !checked)
                return std::unexpected(core_error(checked.error()));
            auto tree = sandbox.open_tree(reference, limits);
            if (!tree)
                return std::unexpected(tree.error());
            if (tree_identity(*tree) != expected)
                return std::unexpected(
                    Error{"floppy_source_changed", "The disk folder contents changed; inspect the source again."});
        }
        for (std::size_t index = 0; index < retained->inputs.size(); ++index)
            if (auto checked = retained->inputs[index].verify(retained->snapshots[index], cancellation); !checked)
                return checked;
        return {};
    };
    if (auto checked = result.verify(context.cancellation); !checked)
        return std::unexpected(checked.error());
    return result;
}
} // namespace axk::app
