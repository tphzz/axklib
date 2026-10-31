#include "conversion_sources.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "axklib/application/image_session_contracts.hpp"
#include "axklib/filesystems/fat/view.hpp"
#include "axklib/utf8.hpp"
#include "content_digest.hpp"
#include "image_sources.hpp"

namespace axk::app::conversion_internal {

Result<NativeDocumentSnapshot> open_source(const nlohmann::json &input, const native::FormatRegistry &formats,
                                           std::string_view format_id, const Sandbox &sandbox, UploadStore &uploads,
                                           ImageSessionManager &images, const OperationContext &context) {
    try {
        const auto kind = input.at("kind").get<std::string>();
        if (kind == "DOCUMENT") {
            if (input.size() != 4U || !input.at("expectedRevision").is_number_integer() ||
                input.at("expectedRevision") < 1)
                return std::unexpected(Error{"invalid_request", "Document source has unknown fields"});
            return images.native_snapshot(image_document_ref(input), context.owner_id,
                                          input.at("expectedRevision").get<std::uint64_t>(), context.cancellation);
        }
        if (kind != "SOURCES" || input.size() != 2U || !input.at("sources").is_array())
            return std::unexpected(Error{"invalid_request", "Select a native document or explicit source set"});
        const auto &references = input.at("sources");
        if (references.empty() || references.size() > native::SourceSet::maximum_members)
            return std::unexpected(Error{"invalid_request", "Select between one and 32 sources"});
        std::vector<ImageSourceRef> files;
        std::vector<native::SourceMember> members;
        std::set<std::string> upload_ids;
        for (const auto &reference : references) {
            if (reference.at("kind") != "UPLOAD") {
                auto parsed = image_source_ref_from_json(reference);
                if (!parsed)
                    return std::unexpected(parsed.error());
                if (parsed->kind == ImageSourceKind::device)
                    return std::unexpected(Error{"invalid_request", "Open a device workspace before conversion"});
                files.push_back(*parsed);
                continue;
            }
            if (!reference.is_object() || reference.size() != 2U || !reference.at("upload").is_object() ||
                reference.at("upload").size() != 1U)
                return std::unexpected(Error{"invalid_request", "Upload source has unknown or missing fields"});
            const auto upload_id = reference.at("upload").at("uploadId").get<std::string>();
            if (!upload_ids.insert(upload_id).second)
                return std::unexpected(Error{"duplicate_image_source", "An upload was selected more than once"});
            auto snapshot = uploads.inspect({upload_id}, context.owner_id);
            if (!snapshot)
                return std::unexpected(snapshot.error());
            if (snapshot->kind != UploadKind::disk_image)
                return std::unexpected(
                    Error{"upload_kind_mismatch", "Conversion requires an admitted disk image upload"});
            auto lease = uploads.lease({upload_id}, context.owner_id);
            if (!lease)
                return std::unexpected(lease.error());
            auto upload_storage = Sandbox::create({{"upload", "Upload", lease->path().parent_path(), false}});
            if (!upload_storage)
                return std::unexpected(upload_storage.error());
            auto file = upload_storage->open_file({"upload", text::path_to_utf8(lease->path().filename())});
            if (!file)
                return std::unexpected(file.error());
            const auto reader = file->reader;
            const auto fingerprint = detail::reader_sha256(*reader, context.cancellation);
            if (!fingerprint)
                return std::unexpected(fingerprint.error());
            auto root = filesystems::open_fat(reader, snapshot->filename, context.cancellation);
            if (!root)
                return std::unexpected(Error{root.error().code == ErrorCode::operation_cancelled
                                                 ? "operation_cancelled"
                                                 : "conversion_source_invalid",
                                             root.error().message});
            native::SourceMember member{"upload-" + upload_id, snapshot->filename, reader, {std::move(*root)}, {}};
            member.verify_member = [retained = std::make_shared<UploadLease>(std::move(*lease)), reader,
                                    bound = std::move(file->verify_unchanged),
                                    digest = *fingerprint](const CancellationToken &token) -> axk::Result<void> {
                static_cast<void>(retained);
                if (const auto active = token.check(); !active)
                    return std::unexpected(active.error());
                if (const auto checked = bound(); !checked)
                    return std::unexpected(
                        make_error(ErrorCode::transaction_stale, ErrorCategory::io, checked.error().message));
                const auto current = detail::reader_sha256(*reader, token);
                if (!current && current.error().code == "operation_cancelled")
                    return std::unexpected(
                        make_error(ErrorCode::operation_cancelled, ErrorCategory::io, current.error().message));
                if (!current || *current != digest)
                    return std::unexpected(
                        make_error(ErrorCode::transaction_stale, ErrorCategory::io, "Conversion upload changed"));
                if (const auto checked = bound(); !checked)
                    return std::unexpected(
                        make_error(ErrorCode::transaction_stale, ErrorCategory::io, checked.error().message));
                return {};
            };
            member.verify_location = [verify = member.verify_member](const auto &, const CancellationToken &token) {
                return verify(token);
            };
            members.push_back(std::move(member));
        }
        if (!files.empty()) {
            auto admitted = admit_image_sources(sandbox, nullptr, nullptr, files, context.owner_id,
                                                context.cancellation, context.progress);
            if (!admitted)
                return std::unexpected(admitted.error());
            for (const auto &member : admitted->native_sources->members())
                members.push_back(member);
        }
        auto sources = native::SourceSet::create(std::move(members));
        if (!sources)
            return std::unexpected(Error{"conversion_source_invalid", sources.error().message});
        auto opened = formats.open(*sources, {std::string{format_id}}, context.cancellation);
        if (!opened)
            return std::unexpected(Error{opened.error().code == ErrorCode::operation_cancelled
                                             ? "operation_cancelled"
                                             : "conversion_source_invalid",
                                         opened.error().message});
        if (opened->documents.size() != 1U || opened->selection_required)
            return std::unexpected(Error{"conversion_source_ambiguous", "Select exactly one native source document"});
        return NativeDocumentSnapshot{{}, 0U, opened->documents.front(), {}};
    } catch (const nlohmann::json::exception &) {
        return std::unexpected(Error{"invalid_request", "Malformed conversion source selection"});
    }
}

} // namespace axk::app::conversion_internal
