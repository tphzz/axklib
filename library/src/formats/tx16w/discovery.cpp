#include "axklib/formats/tx16w/dependencies.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace axk::tx16w::detail {

std::vector<CompanionGroup> group_companions(std::span<const native::SourceMember> members,
                                             std::span<const std::shared_ptr<const Document>> documents) {
    const auto has_setup = [](const Document &document) {
        return std::ranges::any_of(document.native_objects(), [](const auto &object) {
            return std::holds_alternative<NativeSetup>(object.value);
        });
    };
    std::vector<std::size_t> owners(documents.size());
    std::vector<std::vector<std::string>> ambiguities(documents.size());
    std::iota(owners.begin(), owners.end(), 0U);
    const auto supplies = [&](std::size_t target, std::size_t candidate) {
        return documents[target] &&
               std::ranges::any_of(documents[target]->summary().source_requirements, [&](const auto &requirement) {
                   return supplies_requirement(requirement, members[candidate]);
               });
    };
    // A source without a Setup may supply exactly one Setup-bearing document. Independent
    // Setups are never combined merely because they reuse file names or Wave slots.
    for (std::size_t candidate = 0U; candidate < documents.size(); ++candidate) {
        if (documents[candidate] && has_setup(*documents[candidate]))
            continue;
        std::vector<std::size_t> matches;
        for (std::size_t target = 0U; target < documents.size(); ++target)
            if (target != candidate && documents[target] && has_setup(*documents[target]) &&
                (supplies(target, candidate) || supplies(candidate, target)))
                matches.push_back(target);
        if (matches.size() == 1U)
            owners[candidate] = matches.front();
        else if (matches.size() > 1U)
            for (const auto target : matches)
                for (const auto &requirement : documents[target]->summary().source_requirements)
                    if (supplies_requirement(requirement, members[candidate]))
                        ambiguities[target].push_back(requirement.name);
    }
    auto unique_owners = owners;
    for (std::size_t target = 0U; target < documents.size(); ++target) {
        if (!documents[target] || owners[target] != target)
            continue;
        for (const auto &requirement : documents[target]->summary().source_requirements) {
            std::vector<std::size_t> suppliers;
            for (std::size_t candidate = 0U; candidate < documents.size(); ++candidate)
                if (candidate != target && owners[candidate] == target &&
                    supplies_requirement(requirement, members[candidate]))
                    suppliers.push_back(candidate);
            if (suppliers.size() > 1U) {
                ambiguities[target].push_back(requirement.name);
                for (const auto candidate : suppliers)
                    unique_owners[candidate] = candidate;
            }
        }
    }
    owners = std::move(unique_owners);
    std::vector<CompanionGroup> result;
    for (std::size_t owner = 0U; owner < documents.size(); ++owner) {
        if (owners[owner] != owner || !documents[owner])
            continue;
        std::vector<std::string> group;
        for (std::size_t index = 0U; index < owners.size(); ++index)
            if (owners[index] == owner)
                group.push_back(members[index].id);
        result.push_back({std::move(group), std::move(ambiguities[owner])});
    }
    return result;
}

Result<std::vector<std::shared_ptr<const native::Document>>>
open_documents(std::shared_ptr<const native::SourceSet> sources, const CancellationToken &cancellation) {
    std::vector<std::shared_ptr<const Document>> documents;
    for (const auto &member : sources->members()) {
        auto opened = Document::open(sources, cancellation, std::array{member.id});
        if (!opened) {
            if (opened.error().code == ErrorCode::container_unrecognized) {
                documents.push_back(nullptr);
                continue;
            }
            return std::unexpected(opened.error());
        }
        documents.push_back(std::move(*opened));
    }
    std::vector<std::shared_ptr<const native::Document>> result;
    for (const auto &group : group_companions(sources->members(), documents)) {
        if (group.member_ids.size() == 1U && group.ambiguous_requirements.empty()) {
            const auto member =
                std::ranges::find(sources->members(), group.member_ids.front(), &native::SourceMember::id);
            const auto index = static_cast<std::size_t>(member - sources->members().begin());
            result.push_back(documents[index]);
            continue;
        }
        auto opened = Document::open(sources, cancellation, group.member_ids, group.ambiguous_requirements);
        if (!opened)
            return std::unexpected(opened.error());
        result.push_back(std::move(*opened));
    }
    return result;
}

} // namespace axk::tx16w::detail
