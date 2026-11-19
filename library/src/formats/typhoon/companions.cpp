#include "axklib/formats/typhoon/companions.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <map>
#include <set>
#include <tuple>

#include "reference_name.hpp"

namespace axk::typhoon {
Result<Companions> find_companions(const Document &seed, std::span<const std::shared_ptr<const Document>> neighbors,
                                   const CancellationToken &token) {
    const auto limit = []() {
        return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                          "Typhoon companion discovery exceeds limits"));
    };
    if (neighbors.size() > 1024U)
        return limit();
    using Key = std::tuple<std::string, std::string, std::uint32_t>;
    struct Occurrence {
        std::size_t doc;
        const Item *item;
    };
    std::map<Key, std::vector<Occurrence>> index;
    std::size_t total{}, effort{};
    std::vector<const Document *> documents{&seed};
    for (const auto &n : neighbors)
        documents.push_back(n.get());
    for (std::size_t i = 0; i < documents.size(); ++i) {
        const auto *doc = documents[i];
        if (!doc || (i && doc->summary().completeness == native::Completeness::invalid))
            continue;
        for (const auto &object : doc->catalog().objects) {
            if (auto active = token.check(); !active)
                return std::unexpected(active.error());
            if (++total > 65536U)
                return limit();
            if (const auto item = doc->find(object.id); item && item->type != "native-file")
                index[{item->type, item->name, item->version}].push_back({i, item});
        }
    }
    Companions group;
    std::set<std::size_t> included{0};
    std::deque<std::size_t> queue{0};
    std::map<std::string, std::vector<std::size_t>> ties;
    while (!queue.empty()) {
        const auto *doc = documents[queue.front()];
        queue.pop_front();
        for (const auto &object : doc->catalog().objects) {
            const auto *item = doc->find(object.id);
            for (const auto &r : item->references) {
                if (auto active = token.check(); !active)
                    return std::unexpected(active.error());
                const auto matches = index.find({r.type, reference_stem(r.name), r.version});
                if (matches == index.end())
                    continue;
                bool present = false;
                std::vector<std::size_t> suppliers;
                for (const auto &entry : matches->second) {
                    if (++effort > 4U * 1024U * 1024U)
                        return limit();
                    if (!r.disk.empty() && r.disk != entry.item->disk)
                        continue;
                    if (included.contains(entry.doc)) {
                        present = true;
                        break;
                    }
                    suppliers.push_back(entry.doc);
                }
                if (present || suppliers.empty())
                    continue;
                if (suppliers.size() != 1) {
                    ties.emplace(std::format("{} {:08x} {}", r.name, r.version, r.disk), std::move(suppliers));
                    continue;
                }
                if (seed.sources()->members().size() + group.candidates.size() >= native::SourceSet::maximum_members)
                    return limit();
                included.insert(suppliers.front());
                queue.push_back(suppliers.front());
                group.candidates.push_back(suppliers.front() - 1U);
            }
        }
    }
    for (const auto &[name, choices] : ties)
        if (std::ranges::none_of(choices, [&](auto d) { return included.contains(d); }))
            group.ambiguous.insert(name);
    return group;
}
} // namespace axk::typhoon
