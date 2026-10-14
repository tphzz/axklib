#include "file_sources.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "axklib/filesystems/fat/view.hpp"
#include "axklib/utf8.hpp"

namespace axk::conversion::file_internal {
namespace {
constexpr std::uint64_t maximum_source_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_files = 4096U;
Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::io, std::move(message));
}
Result<std::vector<std::filesystem::path>> members(const std::filesystem::path &directory) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator{directory, error};
    for (; !error && iterator != std::filesystem::recursive_directory_iterator{}; iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (error)
            break;
        if (std::filesystem::is_symlink(status) ||
            (!std::filesystem::is_directory(status) && !std::filesystem::is_regular_file(status)))
            return std::unexpected(
                invalid("Conversion source folders must contain only regular files and directories"));
        if (result.size() >= maximum_files || iterator.depth() > 32)
            return std::unexpected(invalid("Conversion source folder exceeds its entry or depth limit"));
        result.push_back(iterator->path());
    }
    if (error)
        return std::unexpected(invalid("Cannot enumerate conversion source folder"));
    std::ranges::sort(result);
    return result;
}
} // namespace

Result<Binding> bind(const std::filesystem::path &path, const CancellationToken &cancellation) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error || !std::filesystem::is_regular_file(canonical, error) || error)
        return std::unexpected(invalid("Conversion input must be a regular file"));
    const auto reader = FileReader::open(canonical);
    if (!reader)
        return std::unexpected(reader.error());
    const auto digest = package_internal::sha256_reader(**reader, cancellation);
    if (!digest)
        return std::unexpected(digest.error());
    return Binding{canonical, *digest, (*reader)->size()};
}

Result<Sources> admit(std::span<const std::filesystem::path> paths, const CancellationToken &cancellation) {
    if (paths.empty() || paths.size() > native::SourceSet::maximum_members)
        return std::unexpected(invalid("Select between one and 32 conversion sources"));
    Sources result;
    std::set<std::filesystem::path> selected;
    std::vector<native::SourceMember> sources;
    std::uint64_t total_bytes{};
    auto snapshot = [&](const std::filesystem::path &path) -> Result<std::shared_ptr<const RandomAccessReader>> {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        const auto reader = FileReader::open(path);
        if (!reader)
            return std::unexpected(reader.error());
        if ((*reader)->size() > maximum_source_bytes - total_bytes || result.files.size() >= maximum_files)
            return std::unexpected(invalid("Conversion sources exceed the 64 MiB or 4096-file snapshot limit"));
        std::vector<std::byte> bytes(static_cast<std::size_t>((*reader)->size()));
        if (const auto read = (*reader)->read_exact_at(0U, bytes); !read)
            return std::unexpected(read.error());
        result.files.push_back({path, package_internal::sha256(bytes), bytes.size()});
        total_bytes += bytes.size();
        return std::make_shared<MemoryReader>(std::move(bytes));
    };
    for (const auto &path : paths) {
        std::error_code error;
        const auto canonical = std::filesystem::canonical(path, error);
        if (error || !selected.insert(canonical).second)
            return std::unexpected(invalid("Conversion source is unavailable or selected more than once"));
        native::SourceMember member;
        member.id = "source-" + std::to_string(sources.size() + 1U);
        member.label = text::path_to_utf8(canonical.filename());
        if (std::filesystem::is_directory(canonical, error) && !error) {
            auto entries = members(canonical);
            if (!entries)
                return std::unexpected(entries.error());
            filesystems::Root root{member.id, "DIRECTORY", {}};
            for (const auto &entry : *entries) {
                const auto relative = text::path_to_utf8(entry.lexically_relative(canonical));
                if (std::filesystem::is_directory(entry, error) && !error) {
                    root.entries.push_back({relative, relative, true, 0U, {}});
                    continue;
                }
                auto reader = snapshot(entry);
                if (!reader)
                    return std::unexpected(reader.error());
                root.entries.push_back({relative, relative, false, 0U, std::move(*reader)});
            }
            member.roots.push_back(std::move(root));
            result.directories.push_back({canonical, std::move(*entries)});
        } else {
            if (!std::filesystem::is_regular_file(canonical, error) || error)
                return std::unexpected(invalid("Conversion sources must be files or folders"));
            auto reader = snapshot(canonical);
            if (!reader)
                return std::unexpected(reader.error());
            auto root = filesystems::open_fat(*reader, member.id, cancellation);
            if (!root)
                return std::unexpected(root.error());
            member.reader = std::move(*reader);
            member.roots.push_back(std::move(*root));
        }
        sources.push_back(std::move(member));
    }
    auto source_set = native::SourceSet::create(std::move(sources));
    if (!source_set)
        return std::unexpected(source_set.error());
    result.sources = std::move(*source_set);
    if (auto checked = verify(result, cancellation); !checked)
        return std::unexpected(checked.error());
    return result;
}

Result<void> verify(const Sources &sources, const CancellationToken &cancellation) {
    for (const auto &directory : sources.directories) {
        const auto current = members(directory.path);
        if (!current || *current != directory.members)
            return std::unexpected(invalid("Conversion source folder changed; prepare a new plan"));
    }
    for (const auto &original : sources.files) {
        const auto current = bind(original.path, cancellation);
        if (!current)
            return std::unexpected(current.error());
        if (current->path != original.path || current->digest != original.digest || current->size != original.size)
            return std::unexpected(invalid("Conversion source changed; prepare a new plan"));
    }
    return {};
}
} // namespace axk::conversion::file_internal
