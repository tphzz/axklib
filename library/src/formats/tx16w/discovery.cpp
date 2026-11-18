#include "axklib/formats/tx16w/dependencies.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/native/natural_compare.hpp"

namespace axk::tx16w::detail {
namespace {
using Names = std::set<std::string>;
struct Inventory {
    Names files;
    Names decoded;
    Names setups;
    Names duplicates;
    Names required;
};

Inventory inventory(const Document &document) {
    Inventory result;
    for (const auto &file : document.native_files()) {
        if (std::string_view("SVUW").find(file.identity.kind) == std::string_view::npos)
            continue;
        const auto key = file_key(file.identity);
        if (!result.files.insert(key).second)
            result.duplicates.insert(key);
        if (file.decoded)
            result.decoded.insert(key);
        if (file.identity.kind == 'S')
            result.setups.insert(key);
    }
    result.required = required_file_keys(document);
    return result;
}
bool intersects(const Names &left, const Names &right) {
    return std::ranges::any_of(left, [&](const auto &name) { return right.contains(name); });
}
bool conflicts(const Inventory &left, const Inventory &right) {
    // Unreferenced Waves can occur on several disks. Only a named dependency must have one occurrence.
    return std::ranges::any_of(left.files,
                               [&](const auto &name) {
                                   return right.files.contains(name) &&
                                          (left.required.contains(name) || right.required.contains(name));
                               }) ||
           intersects(left.duplicates, right.required) || intersects(right.duplicates, left.required);
}
} // namespace

std::vector<std::size_t> nearby_order(std::string_view seed, std::span<const std::string> labels) {
    const auto marker = labels.size();
    auto base = [&](std::size_t i) {
        const std::string_view name = i == marker ? seed : std::string_view(labels[i]);
        const auto slash = name.find_last_of("/\\");
        return slash == name.npos ? name : name.substr(slash + 1U);
    };
    std::vector<std::size_t> sorted(marker + 1U);
    std::iota(sorted.begin(), sorted.end(), 0U);
    std::ranges::stable_sort(sorted, [&](auto l, auto r) { return native::natural_compare(base(l), base(r)) < 0; });
    const auto center = static_cast<std::size_t>(std::ranges::find(sorted, marker) - sorted.begin());
    std::vector<std::size_t> result;
    for (std::size_t distance = 1U; result.size() < marker; ++distance) {
        if (distance <= center)
            result.push_back(sorted[center - distance]);
        if (distance < sorted.size() - center)
            result.push_back(sorted[center + distance]);
    }
    return result;
}

Result<CompanionGroup> resolve_companions(const Document &seed, std::span<const native::SourceMember> selected,
                                          std::span<const native::SourceMember> candidates,
                                          std::span<const std::shared_ptr<const Document>> documents,
                                          const CancellationToken &cancellation) {
    if (selected.empty() || candidates.size() != documents.size())
        return std::unexpected(
            make_error(ErrorCode::invalid_argument, ErrorCategory::object, "Invalid companion scope"));
    std::vector<Inventory> inventories;
    std::vector<std::string> labels;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        inventories.push_back(documents[i] && documents[i]->summary().completeness != native::Completeness::invalid
                                  ? inventory(*documents[i])
                                  : Inventory{});
        labels.push_back(candidates[i].label);
    }
    const auto order = nearby_order(selected.front().label, labels);
    std::vector<native::SourceMember> admitted(selected.begin(), selected.end());
    std::shared_ptr<const Document> combined;
    CompanionGroup result;
    while (true) {
        if (auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        const auto &current = combined ? *combined : seed;
        std::map<std::string, std::vector<std::size_t>> suppliers;
        std::map<std::string, std::string> display_names;
        for (const auto &requirement : current.summary().source_requirements) {
            if (requirement.state != native::DependencyState::missing &&
                requirement.state != native::DependencyState::unsupported)
                continue;
            if (const auto identity = identify_file(requirement.name)) {
                auto key = file_key(*identity);
                suppliers.try_emplace(key);
                display_names.emplace(std::move(key), requirement.name);
            }
        }
        if (suppliers.empty())
            return result;
        const auto held = inventory(current);
        for (const auto i : order) {
            const auto &candidate = inventories[i];
            // A Wave shared with a different Setup does not make that Setup a companion.
            if (conflicts(candidate, held) || intersects(candidate.duplicates, candidate.required) ||
                std::ranges::any_of(candidate.setups, [&](const auto &name) { return !suppliers.contains(name); }))
                continue;
            for (const auto &name : candidate.decoded)
                if (auto found = suppliers.find(name); found != suppliers.end())
                    found->second.push_back(i);
        }
        std::set<std::size_t> forced;
        for (const auto &[_, choices] : suppliers)
            if (choices.size() == 1U)
                forced.insert(choices.front());
        // Two individually required disks may themselves introduce a duplicate. Neither wins by order.
        std::set<std::size_t> incompatible;
        auto required = held.required;
        for (const auto i : forced)
            required.insert(inventories[i].required.begin(), inventories[i].required.end());
        std::map<std::string, std::size_t> claims;
        for (const auto i : forced)
            for (const auto &name : inventories[i].files)
                if (const auto [it, inserted] = claims.emplace(name, i); !inserted && required.contains(name)) {
                    incompatible.insert(it->second);
                    incompatible.insert(i);
                }
        if (forced.size() == incompatible.size()) {
            for (const auto &[key, choices] : suppliers)
                if (choices.size() > 1U || (!choices.empty() && incompatible.contains(choices.front())))
                    result.ambiguous_requirements.push_back(display_names.at(key));
            return result;
        }
        for (const auto i : order)
            if (forced.contains(i) && !incompatible.contains(i)) {
                if (admitted.size() >= native::SourceSet::maximum_members)
                    return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                                      "Resolved source set exceeds its member limit"));
                admitted.push_back(candidates[i]);
                result.member_ids.push_back(candidates[i].id);
            }
        auto sources = native::SourceSet::create(admitted);
        if (!sources)
            return std::unexpected(sources.error());
        auto reopened = Document::open(*sources, cancellation);
        if (!reopened)
            return std::unexpected(reopened.error());
        combined = std::move(*reopened);
    }
}

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

static Result<void> open_group(const std::shared_ptr<const native::SourceSet> &sources,
                               std::span<const native::SourceMember> members, const CancellationToken &cancellation,
                               std::vector<std::shared_ptr<const native::Document>> &result) {
    std::vector<std::shared_ptr<const Document>> documents;
    for (const auto &member : members) {
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
    for (const auto &group : group_companions(members, documents)) {
        if (group.member_ids.size() == 1U && group.ambiguous_requirements.empty()) {
            const auto member = std::ranges::find(members, group.member_ids.front(), &native::SourceMember::id);
            const auto index = static_cast<std::size_t>(member - members.begin());
            result.push_back(documents[index]);
            continue;
        }
        auto opened = Document::open(sources, cancellation, group.member_ids, group.ambiguous_requirements);
        if (!opened)
            return std::unexpected(opened.error());
        result.push_back(std::move(*opened));
    }
    return {};
}

Result<std::vector<std::shared_ptr<const native::Document>>>
open_documents(std::shared_ptr<const native::SourceSet> sources, const CancellationToken &cancellation) {
    std::vector<std::shared_ptr<const native::Document>> result;
    if (auto opened = open_group(sources, sources->members(), cancellation, result); !opened)
        return std::unexpected(opened.error());
    return result;
}

Result<std::vector<std::shared_ptr<const native::Document>>>
open_grouped_documents(std::shared_ptr<const native::SourceSet> sources, const CancellationToken &cancellation,
                       std::span<const std::vector<std::string>> groups) {
    std::vector<std::vector<native::SourceMember>> scopes(groups.size() + 1U);
    for (const auto &member : sources->members()) {
        const auto group = std::ranges::find_if(
            groups, [&](const auto &ids) { return std::ranges::find(ids, member.id) != ids.end(); });
        scopes[static_cast<std::size_t>(group - groups.begin())].push_back(member);
    }
    std::vector<std::shared_ptr<const native::Document>> result;
    for (const auto &members : scopes)
        if (auto opened = open_group(sources, members, cancellation, result); !opened)
            return std::unexpected(opened.error());
    return result;
}

} // namespace axk::tx16w::detail
