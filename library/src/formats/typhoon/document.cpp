#include "axklib/formats/typhoon/document.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <tuple>
#include <utility>

#include "iff.hpp"
#include "instrument.hpp"
#include "reference_name.hpp"

namespace axk::typhoon {
namespace {
using namespace iff;
constexpr std::string_view format = "nuedge.typhoon";
std::string name_field(std::string_view value) {
    value = value.substr(0, value.find('\0'));
    while (!value.empty() && value.back() == ' ')
        value.remove_suffix(1);
    std::string key(value);
    for (auto &ch : key) {
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - 'a' + 'A');
        else if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126)
            ch = '?';
    }
    return key;
}
struct File {
    native::SourceLocation location;
    std::string name, type, disk, occurrence;
};
Result<std::vector<File>> scan(const native::SourceSet &s, const CancellationToken &token,
                               std::span<const std::string> member_ids = {}) {
    std::vector<File> files;
    std::size_t entries = 0;
    std::uint64_t total = 0;
    for (const auto &member : s.members()) {
        if (!member_ids.empty() && std::ranges::find(member_ids, member.id) == member_ids.end())
            continue;
        for (const auto &root : member.roots)
            for (const auto &e : root.entries) {
                if (auto ok = token.check(); !ok)
                    return std::unexpected(ok.error());
                if (e.directory)
                    continue;
                const auto path = std::string_view(e.path);
                const auto name = e.name.empty() ? path.substr(path.find_last_of("/\\") + 1) : std::string_view(e.name);
                if (name.size() < 4 || name[name.size() - 4] != '.' || name.back() < '0' || name.back() > '9' ||
                    name[name.size() - 2] < '0' || name[name.size() - 2] > '9')
                    continue;
                const auto ext = name_field(name.substr(name.size() - 3, 1));
                if (ext != "P" && ext != "O" && ext != "X" && ext != "C" && ext != "A")
                    continue;
                if (++entries > 16384)
                    return std::unexpected(invalid("Typhoon discovery exceeds entry limit"));
                auto location = s.locate(member.id, root.id, e.id);
                if (!location)
                    return std::unexpected(location.error());
                auto prefix = *location;
                prefix.length = std::min<std::uint64_t>(prefix.length, 64);
                auto b = s.read(prefix, token);
                if (!b)
                    return std::unexpected(b.error());
                if (b->size() < 36)
                    continue;
                const auto kind = item_kind(name, *b);
                if (kind.empty())
                    continue;
                total += location->length;
                if (location->length > 737280 || total > 32ULL * 737280 || files.size() >= 4096)
                    return std::unexpected(invalid("Typhoon disk set exceeds item limits"));
                const auto disk_file = member.label.substr(member.label.find_last_of("/\\") + 1U);
                files.push_back({*location, name_field(name.substr(0, name.size() - 4)), std::string(kind),
                                 name_field(root.volume_label), disk_file + " / " + std::string(name)});
            }
    }
    return files;
}
Result<void> parse_item(Item &item, Bytes bytes) {
    const auto form_type = form(bytes);
    if (!form_type)
        return std::unexpected(form_type.error());
    bool identified = false;
    const auto identify = [&](Bytes b) -> Result<void> {
        if (b.size() != 16 || identified)
            return std::unexpected(invalid("Invalid Typhoon VInf"));
        item.version = u32(b, 12);
        identified = true;
        return {};
    };
    if (item.type == "wave") {
        auto w = inspect_wave(bytes);
        if (!w)
            return std::unexpected(w.error());
        item.wave = *w;
    }
    const auto walk = [&](auto &&self, Bytes b, std::string_view parent) -> Result<void> {
        return chunks(b, [&](std::string_view tag, Bytes data) -> Result<void> {
            if (tag == "VInf" && (parent == "item" || parent == "APPL"))
                return identify(data);
            if (parent == "item" && tag == "APPL" && item.wave && data.size() >= 12 &&
                ascii(data.first(12)) == "stoc\x07Typhoon")
                return self(self, data.subspan(12), "APPL");
            const bool group = item.type == "voice" && parent == "item" && tag == "Grop";
            const bool split = parent == "Grop" && tag == "Splt";
            const bool part = item.type == "performance" && parent == "item" && (tag == "Entr" || tag == "PChg");
            if (group || split || part)
                return self(self, data, tag);
            const bool setup = item.type == "setup" && parent == "item";
            if (!((tag == "Wave" && (setup || parent == "Splt")) || (tag == "Perf" && setup) ||
                  (tag == "Voic" && (setup || parent == "Entr" || parent == "PChg"))))
                return {};
            if (data.size() != 20 || item.references.size() >= 8192)
                return std::unexpected(invalid("Invalid Typhoon item reference"));
            const auto disk = data.subspan(12, 8);
            item.references.push_back({tag == "Wave"   ? "wave"
                                       : tag == "Perf" ? "performance"
                                                       : "voice",
                                       name_field(ascii(data.first(8))),
                                       std::ranges::all_of(disk, [](auto x) { return x == std::byte{0xff}; })
                                           ? std::string{}
                                           : name_field(ascii(disk)),
                                       u32(data, 8)});
            return {};
        });
    };
    if (auto ok = walk(walk, bytes.subspan(12), "item"); !ok)
        return ok;
    if (!identified)
        return std::unexpected(invalid("Missing Typhoon item version"));
    return {};
}
} // namespace

std::string_view item_kind(std::string_view filename, std::span<const std::byte> prefix) {
    if (prefix.size() < 36 || filename.size() < 5)
        return {};
    const auto ext = name_field(filename.substr(filename.size() - 4));
    if (ext.size() != 4 || ext[0] != '.' || ext[2] < '0' || ext[2] > '9' || ext[3] < '0' || ext[3] > '9')
        return {};
    const auto h = iff::ascii(prefix.first(36));
    if (!h.starts_with("FORM"))
        return {};
    const auto type = h.substr(8, 4);
    if (h.substr(12, 4) == "VInf" && iff::u32(prefix, 16) == 16) {
        if (ext[1] == 'P' && type == "TYPP")
            return "performance";
        if (ext[1] == 'O' && type == "TYPV")
            return "voice";
        if (ext[1] == 'X' && type == "TYPS")
            return "setup";
    }
    if (((ext[1] == 'C' && type == "AIFC") || (ext[1] == 'A' && type == "AIFF")) && h.substr(12, 4) == "APPL" &&
        h.substr(20, 12) == "stoc\x07Typhoon")
        return "wave";
    return {};
}

Result<std::shared_ptr<const Document>> Document::open(std::shared_ptr<const native::SourceSet> sources,
                                                       const CancellationToken &token,
                                                       std::span<const std::string> member_ids) {
    if (!sources)
        return std::unexpected(iff::invalid("Typhoon source set is required"));
    auto files = scan(*sources, token, member_ids);
    if (!files)
        return std::unexpected(files.error());
    if (files->empty())
        return std::unexpected(iff::invalid("No Typhoon items"));
    auto d = std::make_shared<Document>();
    d->sources_ = std::move(sources);
    d->summary_ = {"typhoon-" + files->front().location.member_id,
                   std::string(format),
                   "typhoon-2000",
                   "TX16W Typhoon",
                   native::Completeness::complete,
                   {{"browse", true, {}}, {"edit", false, "Read-only Typhoon disk set"}}};
    d->catalog_.nodes.push_back({d->summary_.id, {}, {}, "document", "TX16W Typhoon"});
    using Key = std::tuple<std::string, std::string, std::uint32_t>;
    std::map<Key, std::vector<const Item *>> versions;
    for (const auto &f : *files) {
        const auto &l = f.location;
        const auto id = std::format("typhoon:{}:{}:{}:{}:{}:{}", l.member_id.size(), l.member_id, l.root_id.size(),
                                    l.root_id, l.entry_id.size(), l.entry_id);
        auto bytes = d->sources_->read(l, token);
        if (!bytes)
            return std::unexpected(bytes.error());
        Item item{l, id, f.type, f.name, f.disk, 0, {}, {}};
        item.occurrence = f.occurrence;
        auto ok = parse_item(item, *bytes);
        if (ok) {
            if (auto layout = instrument_layout(item, iff::Bytes{*bytes}.subspan(12)); !layout) {
                item.regions.clear();
                item.parts.clear();
                item.mapping_error = layout.error().message;
                d->catalog_.diagnostics.push_back(
                    {"typhoon.unmapped-layout", native::Severity::warning, item.mapping_error, id});
            }
        }
        if (!ok) {
            d->summary_.completeness = native::Completeness::invalid;
            d->catalog_.diagnostics.push_back(
                {"typhoon.invalid-item", native::Severity::error, ok.error().message, id});
            item.type = "native-file";
            item.wave.reset();
            item.references.clear();
        }
        auto &stored = d->items_.emplace(id, std::move(item)).first->second;
        d->catalog_.objects.push_back({id, stored.type, stored.name, {l}, {{"inspect", true, {}}}});
        d->catalog_.nodes.push_back({id, d->summary_.id, id, "object", stored.name});
        if (ok)
            versions[{stored.type, stored.name, stored.version}].push_back(&stored);
    }
    std::size_t candidate_count{};
    for (auto &[id, item] : d->items_) {
        for (auto &ref : item.references) {
            if (d->catalog_.relationships.size() >= 32768)
                return std::unexpected(iff::invalid("Typhoon relationship limit exceeded"));
            if (auto active = token.check(); !active)
                return std::unexpected(active.error());
            native::Relationship edge{std::format("{}:ref:{}", id, d->catalog_.relationships.size()),
                                      id,
                                      {},
                                      {},
                                      item.type + "-" + ref.type,
                                      std::format("{} {:08x} {}", ref.name, ref.version, ref.disk),
                                      native::Resolution::missing};
            const auto found = versions.find({ref.type, reference_stem(ref.name), ref.version});
            if (found != versions.end())
                for (const auto *candidate : found->second) {
                    if (ref.disk.empty() || ref.disk == candidate->disk) {
                        if (++candidate_count > 65536)
                            return std::unexpected(iff::invalid("Typhoon reference candidate limit exceeded"));
                        edge.candidates.push_back(candidate->id);
                    }
                }
            if (edge.candidates.size() == 1) {
                edge.target_id = edge.candidates.front();
                ref.target = edge.target_id;
                edge.resolution = native::Resolution::resolved;
            } else {
                if (!edge.candidates.empty()) {
                    edge.resolution = native::Resolution::ambiguous;
                    ref.ambiguous = true;
                }
                if (d->summary_.completeness != native::Completeness::invalid)
                    d->summary_.completeness = native::Completeness::incomplete;
                d->summary_.source_requirements.push_back(
                    {edge.native_reference,
                     edge.candidates.empty() ? native::DependencyState::missing : native::DependencyState::ambiguous,
                     id});
            }
            d->catalog_.relationships.push_back(std::move(edge));
        }
    }
    return d;
}
const Item *Document::find(std::string_view id) const {
    auto it = items_.find(id);
    return it == items_.end() ? nullptr : &it->second;
}
Result<std::vector<std::int16_t>> Document::pcm(std::string_view id, std::uint32_t first, std::uint32_t count,
                                                const CancellationToken &token) const {
    auto item = find(id);
    if (!item || !item->wave)
        return std::unexpected(iff::invalid("Select a decoded Typhoon Wave"));
    auto b = sources_->read(item->location, token);
    if (!b)
        return std::unexpected(b.error());
    return read_pcm16(*b, first, count);
}
native::FormatProvider provider() {
    return {
        {std::string(format), "TX16W Typhoon", {"typhoon-2000"}},
        [](const native::SourceSet &s, const CancellationToken &t) -> Result<native::Recognition> {
            auto files = scan(s, t);
            if (!files)
                return std::unexpected(files.error());
            native::Recognition r{std::string(format),
                                  files->empty() ? native::RecognitionState::no_match
                                                 : native::RecognitionState::supported,
                                  {}};
            for (auto &f : *files)
                r.claims.push_back(std::move(f.location));
            return r;
        },
        [](auto sources, const CancellationToken &t) -> Result<std::vector<std::shared_ptr<const native::Document>>> {
            auto d = Document::open(std::move(sources), t);
            if (!d)
                return std::unexpected(d.error());
            return std::vector<std::shared_ptr<const native::Document>>{*d};
        }};
}
} // namespace axk::typhoon
