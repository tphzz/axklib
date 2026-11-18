#include "selection.hpp"

#include <algorithm>
#include <map>
#include <string_view>
#include <variant>

namespace axk::tx16w::a_series {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, std::move(message));
}
} // namespace

Result<SourceSelection> select_source(const Document &doc, ImportMode mode, const nlohmann::json &options,
                                      const conversion::Limits &limits) {
    SourceSelection selection;
    std::set<std::string> performance_groups;
    std::map<std::string_view, const NativeObject *> native_objects;
    for (const auto &obj : doc.native_objects()) {
        native_objects.emplace(obj.id, &obj);
        if (std::holds_alternative<Performance>(obj.value))
            performance_groups.insert(obj.file.set_key);
    }
    std::map<std::string, std::string> labels;
    for (const auto &m : doc.sources()->members())
        labels.emplace(m.id, "Disk " + std::to_string(labels.size() + 1U) + ": " + m.label);
    std::map<std::string, std::size_t, std::less<>> indices;
    std::map<std::string, std::string> owners;
    for (const auto &summary : doc.catalog().objects) {
        if (summary.type != "performance" && summary.type != "voice" && summary.type != "timbre" &&
            summary.type != "wave")
            continue;
        if (mode == ImportMode::wave_data_only && summary.type != "wave")
            continue;
        const auto &obj = *native_objects.at(summary.id);
        const bool root = mode == ImportMode::wave_data_only || summary.type == "performance" ||
                          (summary.type == "voice" && !performance_groups.contains(obj.file.set_key));
        if (selection.catalog.size() >= limits.maximum_actions)
            return std::unexpected(invalid("Conversion source catalog exceeds its bounded size"));
        indices.emplace(summary.id, selection.catalog.size());
        const auto slot = std::visit(
            [&](const auto &v) -> unsigned {
                if constexpr (requires { v.slot; })
                    return static_cast<unsigned>(v.slot) + 1U;
                return static_cast<unsigned>(obj.file.native_slot) + 1U;
            },
            obj.value);
        selection.catalog.push_back({summary.id, summary.name, summary.type,
                                     labels.at(obj.location.member_id) + " / " + obj.file.set_name + " / " +
                                         summary.type + " " + std::to_string(slot),
                                     root});
    }
    for (const auto &rel : doc.catalog().relationships) {
        if (rel.resolution != native::Resolution::resolved || !rel.target_id)
            continue;
        const auto node = indices.find(rel.source_id);
        if (node == indices.end())
            continue;
        if (rel.type == "companion-setup")
            owners.emplace(rel.source_id, *rel.target_id);
        else if (indices.contains(*rel.target_id)) {
            auto &deps = selection.catalog[node->second].dependencies;
            if (std::ranges::find(deps, *rel.target_id) == deps.end())
                deps.push_back(*rel.target_id);
        }
    }
    if (!options.contains("sourceObjectIds") && mode == ImportMode::wave_data_only)
        return selection;
    auto &included = selection.included.emplace();
    std::vector<std::size_t> pending;
    if (const auto ids = options.find("sourceObjectIds"); ids != options.end()) {
        if (!ids->is_array() || ids->empty() || ids->size() > selection.catalog.size())
            return std::unexpected(invalid("Select at least one valid TX16W source occurrence"));
        for (const auto &id : *ids) {
            if (!id.is_string())
                return std::unexpected(invalid("Expected a TX16W source occurrence ID"));
            const auto it = indices.find(id.get_ref<const std::string &>());
            if (it == indices.end() || !selection.catalog[it->second].selectable || !included.insert(it->first).second)
                return std::unexpected(invalid("Unknown, duplicate or ineligible TX16W source occurrence"));
            pending.push_back(it->second);
        }
    } else {
        for (std::size_t i = 0; i < selection.catalog.size(); ++i)
            if (selection.catalog[i].selectable) {
                included.insert(selection.catalog[i].id);
                pending.push_back(i);
            }
    }
    while (!pending.empty()) {
        const auto &obj = selection.catalog[pending.back()];
        pending.pop_back();
        if (auto owner = owners.find(obj.id); owner != owners.end())
            included.insert(owner->second);
        for (const auto &dep : obj.dependencies)
            if (included.insert(dep).second)
                pending.push_back(indices.at(dep));
    }
    return selection;
}

void describe_blocker_owners(const Document &doc, conversion::Report &report,
                             const std::optional<std::set<std::string>> &included) {
    if (std::ranges::none_of(report.issues,
                             [](const auto &issue) { return issue.impact == conversion::Impact::blocker; }))
        return;
    using Node = conversion::SourceObject;
    struct Path {
        const Node *node;
        const Node *root;
        const Node *parent;
    };
    std::map<std::string_view, Path> paths;
    std::vector<Path> pending;
    for (const auto &node : report.source_objects) {
        const bool root = node.selectable && (!included || included->contains(node.id));
        Path path{&node, root ? &node : nullptr, nullptr};
        paths.emplace(node.id, path);
        if (root)
            pending.push_back(path);
    }
    // Shared dependencies need only one example path, not a duplicate issue for each Performance.
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto [node, root, parent] = pending[i];
        for (const auto &id : node->dependencies) {
            auto &child = paths.at(id);
            if (!child.root) {
                child.root = root;
                child.parent = node;
                pending.push_back(child);
            }
        }
    }
    for (auto &issue : report.issues) {
        if (issue.impact != conversion::Impact::blocker)
            continue;
        const auto path = paths.find(issue.source_object);
        if (path != paths.end() && path->second.root && path->second.root != path->second.node) {
            const auto &root = *path->second.root;
            issue.message += " (example: required by " + root.name + ", " + root.location;
            const auto &owner = *path->second.parent;
            if (owner.kind == "voice" && path->second.node->kind == "timbre") {
                const auto &voice = std::get<Voice>((*doc.object(owner.id))->value);
                const auto &timbre = std::get<Timbre>((*doc.object(issue.source_object))->value);
                if (&owner != &root)
                    issue.message += "; Voice " + voice.name + ", " + owner.location;
                for (const auto &region : voice.regions)
                    if (region.timbre_slot == timbre.slot)
                        issue.message += ", native keys " + std::to_string(region.low_key_number) + ".." +
                                         std::to_string(region.high_key_number);
            }
            issue.message += ")";
        }
    }
}
} // namespace axk::tx16w::a_series
