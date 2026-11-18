#include "axklib/sfs_disk_sources.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>

#include "../media_signatures.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/sfs_record_reader.hpp"
#include "axklib/utf8.hpp"

namespace axk::inspection {
using formats::a_series::Document;
using formats::a_series::NativeDocument;

Result<bool> SfsDiskSources::recognizes(const RandomAccessReader &reader) {
    // EX5 hard disks can share the leading Yamaha signature. Inspect its descriptor too.
    std::array<std::byte, 0x220U> bytes{};
    auto prefix =
        std::span{bytes}.first(static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), reader.size())));
    if (auto read = reader.read_exact_at(0U, prefix); !read)
        return std::unexpected(read.error());
    return detail::sfs_disk_signature(prefix);
}

Result<void> SfsDiskSources::admit(native::SourceMember &member, const std::filesystem::path &path,
                                   std::string fingerprint, const CancellationToken &cancellation) {
    auto container = open_image(member.reader, path, {.cancellation = cancellation});
    if (!container)
        return std::unexpected(container.error());
    auto decoded = Document::open(MediaContainer{std::move(*container)}, cancellation);
    if (!decoded)
        return std::unexpected(decoded.error());
    const auto &state = (*decoded)->state();
    if (state.inventory.catalog.objects.empty())
        return {}; // SFS alone does not establish a native A-series object document.

    std::shared_ptr<const Container> storage{*decoded, &std::get<Container>(state.media.storage())};
    // Selections outlive a CLI invocation. Bind them to this canonical file and its content.
    const auto identity = fingerprint + text::path_to_utf8(path);
    const auto digest = package_internal::sha256(std::as_bytes(std::span{identity}));
    Disk disk{member.id, "a-series-" + member.id + "-" + package_internal::hex_digest(digest), *decoded, {}};
    std::map<std::uint32_t, filesystems::Root> roots;
    for (const auto &object : state.inventory.catalog.objects) {
        if (auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        const auto &descriptor = *state.descriptors.at(object.key);
        auto &root = roots[object.partition.value];
        root.id = "sfs-" + std::to_string(object.partition.value);
        root.kind = "SFS";
        const auto entry = std::to_string(object.sfs_id.value);
        root.entries.push_back({entry, descriptor.logical_path, false, 0U,
                                std::make_shared<detail::SfsRecordReader>(storage, object.partition, object.sfs_id,
                                                                          descriptor.size, CancellationToken{})});
        disk.locations[object.key].push_back({member.id, root.id, entry, 0U, descriptor.size});
    }
    for (auto &[partition, root] : roots)
        member.roots.push_back(std::move(root));
    disks_.push_back(std::move(disk));
    return {};
}

native::FormatProvider SfsDiskSources::provider() && {
    auto disks = std::make_shared<const std::vector<Disk>>(std::move(disks_));
    return {{"yamaha.a-series", "Yamaha A-series", {"a-series"}},
            [disks](const native::SourceSet &sources, const CancellationToken &) -> Result<native::Recognition> {
                native::Recognition result{};
                result.format_id = "yamaha.a-series";
                for (const auto &disk : *disks) {
                    auto claim = sources.locate(disk.member_id, {}, {});
                    if (!claim)
                        return std::unexpected(claim.error());
                    result.claims.push_back(*claim);
                    result.state = native::RecognitionState::supported;
                }
                return result;
            },
            [disks](std::shared_ptr<const native::SourceSet> sources, const CancellationToken &cancellation)
                -> Result<std::vector<std::shared_ptr<const native::Document>>> {
                std::vector<std::shared_ptr<const native::Document>> result;
                for (const auto &disk : *disks) {
                    if (auto active = cancellation.check(); !active)
                        return std::unexpected(active.error());
                    auto doc = NativeDocument::create(disk.decoded, sources, disk.document_id, disk.locations,
                                                      disk.document_id + "/");
                    if (!doc)
                        return std::unexpected(doc.error());
                    result.push_back(std::move(*doc));
                }
                return result;
            }};
}
} // namespace axk::inspection
