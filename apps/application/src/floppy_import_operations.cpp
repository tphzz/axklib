#include "floppy_import_operations.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "axklib/application/secure_random.hpp"
#include "axklib/floppy_import.hpp"
#include "axklib/su700.hpp"
#include "axklib/tx16w.hpp"
#include "content_digest.hpp"
#include "filesystem_inputs.hpp"
#include "session_import_plan.hpp"

namespace axk::app {
namespace {
using namespace package_operations_internal;
constexpr std::uint64_t maximum_disk_bytes = 4U * 1024U * 1024U;
constexpr std::uint64_t maximum_retained_bytes = 512U * 1024U * 1024U;

std::string_view object_type_name(ObjectType type) {
    switch (type) {
    case ObjectType::prog:
        return "PROG";
    case ObjectType::sbac:
        return "SBAC";
    case ObjectType::sbnk:
        return "SBNK";
    case ObjectType::smpl:
        return "SMPL";
    case ObjectType::sequ:
        return "SEQU";
    case ObjectType::prf3:
        return "PRF3";
    default:
        return "UNKNOWN";
    }
}

struct Sources {
    std::vector<filesystem_inputs::OpenedInput> inputs;
    std::vector<Json> snapshots;
    Result<void> verify(const CancellationToken &cancellation) const {
        for (std::size_t i = 0; i < inputs.size(); ++i)
            if (auto checked = inputs[i].verify(snapshots[i], cancellation); !checked)
                return checked;
        return {};
    }
};
struct Inspection {
    std::string owner;
    Clock::time_point expires;
    std::uint64_t reserved_bytes{};
    FloppyImportSource source;
    std::shared_ptr<const Sources> inputs;
};
struct State {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Inspection>, std::less<>> inspections;
    std::uint64_t pending_bytes{};
    void cleanup() {
        std::erase_if(inspections, [](const auto &entry) { return entry.second->expires <= Clock::now(); });
    }
};
class Admission {
    std::shared_ptr<State> state_;
    std::uint64_t bytes_{};

  public:
    Admission(std::shared_ptr<State> state, std::uint64_t bytes) : state_(std::move(state)), bytes_(bytes) {}
    ~Admission() {
        std::lock_guard lock{state_->mutex};
        state_->pending_bytes -= bytes_;
    }
    Admission(const Admission &) = delete;
    Admission &operator=(const Admission &) = delete;
};

Json describe(const FloppyImportInspection &inspection, const std::string &token) {
    Json objects = Json::array();
    for (const auto &object : inspection.objects) {
        objects.push_back({{"objectKey", object.key},
                           {"name", object.name},
                           {"displayName", object.display_name},
                           {"objectType", object_type_name(object.type)},
                           {"sizeBytes", object.size_bytes},
                           {"requiredObjectKeys", object.required_object_keys},
                           {"exclusionReason", object.exclusion_reason}});
    }
    Json excluded = Json::array();
    for (const auto &file : inspection.excluded_files)
        excluded.push_back({{"memberName", file.member_name}, {"path", file.path}, {"sizeBytes", file.size_bytes}});
    Json issues = Json::array();
    for (const auto &issue : inspection.issues)
        issues.push_back({{"code", issue.code}, {"message", issue.message}});
    Json members = Json::array();
    for (const auto &member : inspection.members)
        members.push_back({{"index", member.index}, {"label", member.label}});
    return Json{{"format", "A_SERIES"},
                {"inspectionToken", token},
                {"complete", inspection.complete},
                {"label", inspection.label},
                {"nextRequiredIndex", inspection.next_required_index ? Json(*inspection.next_required_index) : Json{}},
                {"members", std::move(members)},
                {"objects", std::move(objects)},
                {"excludedFiles", std::move(excluded)},
                {"issues", std::move(issues)}};
}

Result<Json> inspect(const Json &request, const OperationContext &context, const Sandbox &sandbox, UploadStore &uploads,
                     const std::shared_ptr<State> &state) {
    const auto &sources = request.at("sources");
    if (!sources.is_array() || sources.empty() || sources.size() > FloppyDiskSet::maximum_members)
        return std::unexpected(Error{"invalid_request", "Choose one disk or up to 32 companion disks."});
    // Reserve before reading payloads, including catalog and snapshot working copies.
    const auto reservation = static_cast<std::uint64_t>(sources.size()) * maximum_disk_bytes * 3U;
    {
        std::lock_guard lock{state->mutex};
        state->cleanup();
        auto retained = state->pending_bytes;
        for (const auto &[token, record] : state->inspections) {
            (void)token;
            retained += record->reserved_bytes;
        }
        if (state->inspections.size() >= 16U || retained > maximum_retained_bytes ||
            reservation > maximum_retained_bytes - retained)
            return std::unexpected(
                Error{"floppy_inspection_capacity", "Close another floppy import before continuing."});
        state->pending_bytes += reservation;
    }
    const Admission admission{state, reservation};
    auto retained_sources = std::make_shared<Sources>();
    std::vector<FatImage> members;
    std::set<std::string> formats;
    for (const auto &source : sources) {
        auto input = filesystem_inputs::open(source, context.owner_id, sandbox, uploads);
        if (!input)
            return std::unexpected(input.error());
        if (input->reader->size() > maximum_disk_bytes)
            return std::unexpected(
                Error{"floppy_source_invalid", "The selected source is larger than a floppy image."});
        auto snapshot = input->snapshot(context.cancellation);
        if (!snapshot)
            return std::unexpected(snapshot.error());
        std::vector<std::byte> bytes(static_cast<std::size_t>(input->reader->size()));
        if (auto read = input->reader->read_exact_at(0U, bytes); !read)
            return std::unexpected(core_error(read.error()));
        const auto reader = std::make_shared<MemoryReader>(std::move(bytes));
        const auto hash = detail::reader_sha256(*reader, context.cancellation);
        if (!hash)
            return std::unexpected(hash.error());
        if (*hash != snapshot->at("sha256").get<std::string>())
            return std::unexpected(Error{"floppy_source_changed", "The floppy changed while it was being inspected."});
        if (auto checked = input->verify_identity(); !checked)
            return std::unexpected(checked.error());
        std::string filename;
        if (source.contains("fileRef"))
            filename = source.at("fileRef").at("relativePath").get<std::string>();
        else {
            auto upload = uploads.inspect({source.at("uploadRef").at("uploadId").get<std::string>()}, context.owner_id);
            if (!upload)
                return std::unexpected(upload.error());
            if (upload->kind != UploadKind::disk_image)
                return std::unexpected(Error{"upload_kind_mismatch", "Choose an admitted disk image upload."});
            filename = upload->filename;
        }
        auto member = FatImage::open(reader, filename, context.cancellation);
        if (!member)
            return std::unexpected(core_error(member.error()));
        std::uint64_t logical_bytes{};
        if (member->files().size() > 8192U)
            return std::unexpected(Error{"floppy_source_invalid", "Too many floppy entries."});
        for (const auto &file : member->files()) {
            if (file.size > reader->size() - logical_bytes)
                return std::unexpected(
                    Error{"floppy_source_invalid", "The floppy logical payload exceeds supported bounds."});
            logical_bytes += file.size;
        }
        auto objects = member->objects(MediaObjectReadMode::decoded_metadata, maximum_disk_bytes, context.cancellation);
        if (!objects)
            return std::unexpected(core_error(objects.error()));
        if (member->yamaha_catalog() || !objects->empty()) {
            formats.insert("A_SERIES");
        } else {
            auto su700 = inspect_su700_floppy(*member, context.cancellation);
            if (su700 && su700->status != Su700ImportStatus::unrelated)
                formats.insert("SU700");
            else if (auto tx16w = tx16w::inspect_disk(*member, context.cancellation); tx16w)
                formats.insert("TX16W");
            else
                formats.insert("UNKNOWN");
        }
        retained_sources->inputs.push_back(std::move(*input));
        retained_sources->snapshots.push_back(std::move(*snapshot));
        members.push_back(std::move(*member));
    }
    if (const auto checked = context.cancellation.check(); !checked)
        return std::unexpected(core_error(checked.error()));
    if (formats.size() != 1U)
        return std::unexpected(Error{"floppy_formats_mixed", "Choose disks from one sampler and one disk set."});
    if (*formats.begin() != "A_SERIES") {
        return Json{{"format", *formats.begin()},
                    {"inspectionToken", nullptr},
                    {"complete", false},
                    {"label", ""},
                    {"nextRequiredIndex", nullptr},
                    {"members", Json::array()},
                    {"objects", Json::array()},
                    {"excludedFiles", Json::array()},
                    {"issues", Json::array()}};
    }
    auto source = FloppyImportSource::open(std::move(members), context.cancellation);
    if (!source)
        return std::unexpected(core_error(source.error()));
    auto token = secure_random_hex(24U);
    if (!token)
        return std::unexpected(token.error());
    auto result = describe(source->inspection(), *token);
    auto record = std::make_shared<Inspection>(Inspection{context.owner_id, Clock::now() + std::chrono::minutes{15},
                                                          reservation, std::move(*source), retained_sources});
    {
        std::lock_guard lock{state->mutex};
        if (!state->inspections.emplace(*token, std::move(record)).second)
            return std::unexpected(Error{"secure_random_failed", "Floppy inspection token collision."});
    }
    return result;
}

Result<std::shared_ptr<Inspection>> owned(const std::shared_ptr<State> &state, const std::string &token,
                                          const std::string &owner) {
    std::lock_guard lock{state->mutex};
    state->cleanup();
    const auto found = state->inspections.find(token);
    if (found == state->inspections.end() || found->second->owner != owner)
        return std::unexpected(Error{"floppy_inspection_not_found", "The floppy inspection is absent or expired."});
    return found->second;
}
} // namespace

Result<void> bind_floppy_import_operations(OperationRegistry &registry, const Sandbox &sandbox, UploadStore &uploads,
                                           ImageSessionManager &images,
                                           const std::shared_ptr<SessionPackageOperationState> &plans) {
    const auto state = std::make_shared<State>();
    for (const std::string operation :
         {"images.floppy_import.inspect", "images.floppy_import.release", "images.floppy_import.plan"}) {
        auto bound = registry.bind(
            operation,
            [&, state, plans, operation](const Json &request, const OperationContext &context) -> Result<Json> {
                try {
                    if (operation == "images.floppy_import.inspect")
                        return inspect(request, context, sandbox, uploads, state);
                    const auto token = request.at("inspectionToken").get<std::string>();
                    auto record = owned(state, token, context.owner_id);
                    if (!record)
                        return std::unexpected(record.error());
                    if (operation == "images.floppy_import.release") {
                        std::lock_guard lock{state->mutex};
                        state->inspections.erase(token);
                        return Json{{"released", true}};
                    }
                    const auto identity = parse_session_identity(request);
                    if (!identity)
                        return std::unexpected(identity.error());
                    auto session = images.begin_read(identity->first, context.owner_id, identity->second);
                    if (!session)
                        return std::unexpected(session.error());
                    if (session->media->kind() != MediaKind::sfs)
                        return std::unexpected(
                            Error{"image_mutation_unsupported", "Floppy import requires a writable SFS image."});
                    const auto inputs = (*record)->inputs;
                    if (auto checked = inputs->verify(context.cancellation); !checked)
                        return std::unexpected(checked.error());
                    auto graph = (*record)->source.prepare(
                        request.at("selectedObjectKeys").get<std::vector<std::string>>(), context.cancellation);
                    if (!graph)
                        return std::unexpected(core_error(graph.error()));
                    const auto retained = retained_package_bytes(*graph);
                    if (!retained)
                        return std::unexpected(retained.error());
                    auto packages = std::make_shared<VerifiedPackageSet>();
                    packages->packages.push_back(std::move(*graph));
                    packages->retained_payload_bytes = *retained;
                    packages->verify_sources = [inputs](const CancellationToken &cancellation) {
                        return inputs->verify(cancellation);
                    };
                    return store_session_import_plan(plans, request, context, *session, packages);
                } catch (const Json::exception &) {
                    return std::unexpected(
                        Error{"invalid_request", "Floppy import request is incomplete or malformed."});
                }
            });
        if (!bound)
            return bound;
    }
    return registry.bind_path_accesses(
        "images.floppy_import.inspect",
        [](const Json &request, const OperationContext &) -> Result<std::vector<PathAccess>> {
            try {
                std::vector<PathAccess> paths;
                for (const auto &source : request.at("sources")) {
                    if (!source.contains("fileRef"))
                        continue;
                    const auto &ref = source.at("fileRef");
                    paths.push_back({{ref.at("rootId").get<std::string>(), ref.at("relativePath").get<std::string>()},
                                     PathAccessMode::shared});
                }
                return paths;
            } catch (const Json::exception &) {
                return std::unexpected(Error{"invalid_request", "Floppy source reference is malformed."});
            }
        });
}
} // namespace axk::app
