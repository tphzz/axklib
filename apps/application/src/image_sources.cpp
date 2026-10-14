#include "image_sources.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <unordered_map>
#include <utility>

#include "image_filesystem_internal.hpp"
#include "image_sessions_internal.hpp"

namespace axk::app {
namespace {

constexpr SandboxTreeLimits directory_limits{16'384U, 64ULL * 1024U * 1024U * 1024U, 32U, 4U * 1024U * 1024U};

class EntryReader final : public RandomAccessReader {
  public:
    EntryReader(std::shared_ptr<const MediaContainer> media, detail::FilesystemFileLocator location, std::uint64_t size)
        : media_(std::move(media)), location_(std::move(location)), size_(size) {}
    std::uint64_t size() const noexcept override { return size_; }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> output) const override {
        if (offset > size_ || output.size() > size_ - offset)
            return std::unexpected(make_error(ErrorCode::out_of_bounds, ErrorCategory::io,
                                              "Read extends beyond the admitted filesystem entry"));
        while (!output.empty()) {
            const auto count = std::min<std::size_t>(output.size(), 1024U * 1024U);
            auto bytes = detail::read_filesystem_range(*media_, location_, offset, count, {});
            if (!bytes)
                return std::unexpected(make_error(ErrorCode::io_read_failed, ErrorCategory::io, bytes.error().message));
            if (bytes->size() != count)
                return std::unexpected(make_error(ErrorCode::io_short_read, ErrorCategory::io, "Incomplete file read"));
            std::ranges::copy(*bytes, output.begin());
            output = output.subspan(count);
            offset += count;
        }
        return {};
    }

  private:
    std::shared_ptr<const MediaContainer> media_;
    detail::FilesystemFileLocator location_;
    std::uint64_t size_;
};

bool contains_path(const std::filesystem::path &parent, const std::filesystem::path &child) {
    const auto [left, right] = std::mismatch(parent.begin(), parent.end(), child.begin(), child.end());
    return left == parent.end() && right != child.end();
}

Result<void> validate_paths(const Sandbox &sandbox, std::span<const ImageSourceRef> references,
                            std::vector<std::filesystem::path> &paths) {
    for (const auto &reference : references) {
        if (reference.kind == ImageSourceKind::device) {
            paths.emplace_back();
            continue;
        }
        auto resolved = reference.kind == ImageSourceKind::file
                            ? sandbox.resolve_file({reference.root_id, reference.relative_path})
                            : sandbox.resolve_directory({reference.root_id, reference.relative_path});
        if (!resolved)
            return std::unexpected(resolved.error());
        for (const auto &existing : paths) {
            std::error_code error;
            if (*resolved == existing || std::filesystem::equivalent(*resolved, existing, error))
                return std::unexpected(Error{"duplicate_image_source", "The same source was selected more than once"});
            if (error)
                return std::unexpected(Error{"image_source_changed", "Source identity could not be verified"});
            if (contains_path(existing, *resolved) || contains_path(*resolved, existing))
                return std::unexpected(Error{"overlapping_image_sources", "Selected files and folders overlap"});
        }
        paths.push_back(std::move(*resolved));
    }
    return {};
}

Result<void> admit_directory(const Sandbox &sandbox, ImageSourceMember &member, const CancellationToken &cancellation) {
    const DirectoryRef reference{member.reference.root_id, member.reference.relative_path};
    auto tree = sandbox.open_tree(reference, directory_limits);
    if (!tree)
        return std::unexpected(tree.error());
    filesystems::Root root{"directory", "directory", {}};
    std::vector<std::function<Result<void>()>> verifiers;
    const std::vector<SandboxTreeEntry> original{tree->entries().begin(), tree->entries().end()};
    member.revision = tree->revision();
    for (std::size_t index = 0U; index < original.size(); ++index) {
        if (auto active = cancellation.check(); !active)
            return std::unexpected(core_error(active.error(), member.reference));
        const auto &entry = original[index];
        member.entry_revisions.push_back(entry.revision);
        filesystems::Entry file{
            entry.relative_path, entry.relative_path, entry.kind == SandboxTreeEntryKind::directory, 0U, {}};
        if (!file.directory) {
            auto opened = tree->open_file(index);
            if (!opened)
                return std::unexpected(opened.error());
            file.reader = std::move(opened->reader);
            verifiers.push_back(std::move(opened->verify_unchanged));
        }
        root.entries.push_back(std::move(file));
    }
    member.roots.push_back(std::move(root));
    member.format = "directory";
    // Membership must remain unchanged too: a new companion can change native resolution.
    member.verify_unchanged = [retained_sandbox = sandbox, reference,
                               retained_tree = std::make_shared<SandboxTree>(std::move(*tree)),
                               checks = std::move(verifiers)]() -> Result<void> {
        auto current = retained_sandbox.open_tree(reference, directory_limits);
        if (!current)
            return std::unexpected(current.error());
        if (auto checked = retained_tree->verify_same_tree(*current); !checked)
            return checked;
        for (const auto &verify : checks)
            if (auto unchanged = verify(); !unchanged)
                return unchanged;
        return {};
    };
    return {};
}

Result<void> project_filesystem(ImageSourceMember &member) {
    if (member.media) {
        auto index = detail::build_image_filesystem(*member.media, member.reader, {});
        if (!index)
            return std::unexpected(index.error());
        for (const auto &entry : index->entries) {
            if (entry.id == entry.root_id)
                member.roots.push_back({entry.id, index->filesystem_name, {}});
        }
        for (const auto &entry : index->entries) {
            if (entry.id == entry.root_id)
                continue;
            auto root = std::ranges::find(member.roots, entry.root_id, &filesystems::Root::id);
            if (root == member.roots.end())
                return std::unexpected(Error{"filesystem_invalid", "Filesystem entry has no root"});
            const auto file = index->files.find(entry.id);
            filesystems::Entry item{entry.id, entry.path, file == index->files.end(), 0U, {}};
            if (file != index->files.end())
                item.reader = std::make_shared<EntryReader>(member.media, file->second, entry.size_bytes.value_or(0U));
            root->entries.push_back(std::move(item));
        }
    }
    if (member.roots.empty()) {
        member.roots.push_back(
            {"raw-file", "file", {{"file", member.reference.relative_path, false, 0U, member.reader}}});
    }
    return {};
}

} // namespace

Result<AdmittedImageSources> admit_image_sources(const Sandbox &sandbox, PathReservationCoordinator *reservations,
                                                 DeviceSessionManager *devices,
                                                 std::span<const ImageSourceRef> references, std::string_view owner_id,
                                                 const CancellationToken &cancellation, ProgressSink *progress,
                                                 diagnostic_sink *diagnostics) {
    if (references.empty() || references.size() > native::SourceSet::maximum_members || owner_id.empty())
        return std::unexpected(Error{"invalid_image_sources", "Select between one and 32 explicit sources"});
    if (references.size() != 1U && std::ranges::any_of(references, [](const auto &reference) {
            return reference.kind == ImageSourceKind::device;
        }))
        return std::unexpected(Error{"invalid_image_sources", "A physical device must be opened on its own"});
    std::vector<std::filesystem::path> paths;
    if (auto validated = validate_paths(sandbox, references, paths); !validated)
        return std::unexpected(validated.error());
    AdmittedImageSources result;
    std::vector<native::SourceMember> native_members;
    std::unordered_map<std::string, std::size_t> admitted_identities;
    for (std::size_t index = 0U; index < references.size(); ++index) {
        const auto &reference = references[index];
        if (auto active = cancellation.check(); !active)
            return std::unexpected(core_error(active.error(), reference));
        auto member = std::make_shared<ImageSourceMember>();
        member->id = "source-" + std::to_string(index + 1U);
        member->reference = reference;
        member->canonical_path = paths[index];
        if (reservations && reference.kind != ImageSourceKind::device) {
            auto lease =
                reservations->try_acquire({{reference.root_id, reference.relative_path}, PathAccessMode::shared});
            if (!lease)
                return std::unexpected(lease.error());
            member->reservation = std::make_shared<PathReservationCoordinator::Lease>(std::move(*lease));
        }
        if (reference.kind == ImageSourceKind::directory) {
            if (auto opened = admit_directory(sandbox, *member, cancellation); !opened)
                return std::unexpected(opened.error());
        } else {
            if (reference.kind == ImageSourceKind::device) {
                if (!devices)
                    return std::unexpected(Error{"devices_unavailable", "Device sources are unavailable"});
                auto opened = devices->open_source(reference.device_session_id, reference.device_target_id, owner_id);
                if (!opened)
                    return std::unexpected(opened.error());
                member->reader = std::move(opened->reader);
                member->revision = std::move(opened->revision);
                member->verify_unchanged = std::move(opened->verify_unchanged);
                member->seal = std::move(opened->seal);
            } else {
                auto opened = sandbox.open_file({reference.root_id, reference.relative_path});
                if (!opened)
                    return std::unexpected(opened.error());
                member->reader = std::move(opened->reader);
                member->revision = std::move(opened->revision);
                member->verify_unchanged = std::move(opened->verify_unchanged);
            }
            auto media = open_media(member->reader, member->canonical_path, cancellation, progress, diagnostics);
            if (media) {
                member->format = media_kind_name(media->kind());
                member->media = std::make_shared<MediaContainer>(std::move(*media));
            } else if (media.error().code != ErrorCode::container_unrecognized) {
                return std::unexpected(core_error(media.error(), reference));
            } else {
                member->format = "file";
            }
            if (auto projected = project_filesystem(*member); !projected)
                return std::unexpected(projected.error());
        }
        if (auto unchanged = member->verify_unchanged(); !unchanged)
            return std::unexpected(unchanged.error());
        if (reference.kind != ImageSourceKind::device) {
            const auto admit_identity = [&](const std::string &identity) {
                const auto [found, inserted] = admitted_identities.emplace(identity, index);
                return inserted || found->second == index;
            };
            if (!admit_identity(member->revision) || !std::ranges::all_of(member->entry_revisions, admit_identity))
                return std::unexpected(Error{"overlapping_image_sources", "Selected sources share a filesystem entry"});
        }
        native_members.push_back({member->id, reference.relative_path, member->reader, member->roots,
                                  [retained = member]() -> axk::Result<void> {
                                      if (auto checked = retained->verify_unchanged(); !checked)
                                          return std::unexpected(make_error(ErrorCode::transaction_stale,
                                                                            ErrorCategory::io,
                                                                            checked.error().message));
                                      return {};
                                  }});
        result.members.push_back(std::move(member));
    }
    auto sources = native::SourceSet::create(std::move(native_members));
    if (!sources)
        return std::unexpected(Error{"invalid_image_sources", sources.error().message});
    result.native_sources = std::move(*sources);
    if (auto verified = result.native_sources->verify(cancellation); !verified)
        return std::unexpected(Error{"image_source_changed", verified.error().message});
    return result;
}

} // namespace axk::app
