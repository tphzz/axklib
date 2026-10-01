#pragma once

#include <algorithm>
#include <expected>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/filesystem_edit.hpp"

namespace axk::detail {
// Resolve a move set before applying any path-changing edit. Descendants travel
// with their selected ancestor, and the destination never moves within the batch.
inline Result<std::vector<FilesystemEdit>> normalize_move_batch(std::span<const FilesystemEdit> edits, bool uppercase) {
    if (!std::ranges::any_of(edits, [](const auto &edit) { return std::holds_alternative<MoveFilesystemEntry>(edit); }))
        return std::vector<FilesystemEdit>{edits.begin(), edits.end()};
    const auto invalid = [](const char *message) {
        return std::unexpected(make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, message));
    };
    if (edits.size() > 10000U)
        return invalid("Move selection exceeds its limit");
    const auto canonical = [uppercase](FilesystemPath path) {
        if (uppercase)
            for (auto &name : path)
                for (auto &byte : name)
                    if (byte >= 'a' && byte <= 'z')
                        byte = static_cast<char>(byte - 'a' + 'A');
        return path;
    };
    std::set<FilesystemPath> sources;
    FilesystemPath destination;
    for (const auto &edit : edits) {
        const auto *move = std::get_if<MoveFilesystemEntry>(&edit);
        if (!move)
            return invalid("Move batches cannot contain other edit kinds");
        if (move->path.empty())
            return invalid("Partition roots cannot be moved");
        const auto target = canonical(move->destination_parent);
        if (!sources.empty() && target != destination)
            return invalid("A move batch requires one destination");
        destination = target;
        sources.insert(canonical(move->path));
    }
    std::vector<FilesystemEdit> result;
    std::set<std::string> names;
    for (const auto &source : sources) {
        if (destination.size() >= source.size() && std::equal(source.begin(), source.end(), destination.begin()))
            return invalid("A directory cannot move into itself or its descendants");
        auto parent = source;
        parent.pop_back();
        bool covered{};
        for (auto ancestor = parent; !ancestor.empty(); ancestor.pop_back())
            covered = covered || sources.contains(ancestor);
        if (covered || parent == destination)
            continue;
        if (!names.insert(source.back()).second)
            return invalid("Selected entries have conflicting destination names");
        result.emplace_back(MoveFilesystemEntry{source, destination});
    }
    return result;
}
} // namespace axk::detail
