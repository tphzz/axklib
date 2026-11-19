#include "axklib/application/conversions.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "axklib/application/download_archives.hpp"
#include "axklib/application/image_session_contracts.hpp"
#include "axklib/application/volume_capacity.hpp"
#include "axklib/conversion/image_set_publication.hpp"
#include "axklib/formats/tx16w/document.hpp"
#include "axklib/formats/typhoon/document.hpp"
#include "companion_source_resolution.hpp"
#include "content_digest.hpp"
#include "conversion_inspection.hpp"
#include "conversion_plan_store.hpp"
#include "conversion_sources.hpp"
#include "directory_cleanup.hpp"
#include "prepared_session_commit.hpp"

namespace axk::app {
namespace {
using Json = nlohmann::json;
using namespace conversion_internal;
class ExpiryCollector {
  public:
    explicit ExpiryCollector(std::shared_ptr<PlanStore> store)
        : worker_([this, retained_store = std::move(store)] {
              std::unique_lock lock{mutex_};
              while (!wake_.wait_for(lock, std::chrono::seconds{1}, [this] { return stopped_; }))
                  retained_store->collect_expired();
          }) {}
    ~ExpiryCollector() {
        {
            const std::scoped_lock lock{mutex_};
            stopped_ = true;
        }
        wake_.notify_one();
        worker_.join();
    }

  private:
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopped_{};
    std::thread worker_;
};

class PlanBytes final : public RandomAccessReader {
  public:
    PlanBytes(std::shared_ptr<const conversion::PreparedPlan> owner, std::span<const std::byte> bytes)
        : owner_(std::move(owner)), bytes_(bytes) {}
    std::uint64_t size() const noexcept override { return bytes_.size(); }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> output) const override {
        if (offset > bytes_.size() || output.size() > bytes_.size() - offset)
            return std::unexpected(
                make_error(ErrorCode::out_of_bounds, ErrorCategory::io, "Conversion patch read is out of range"));
        std::ranges::copy(bytes_.subspan(static_cast<std::size_t>(offset), output.size()), output.begin());
        return {};
    }

  private:
    std::shared_ptr<const conversion::PreparedPlan> owner_;
    std::span<const std::byte> bytes_;
};
Error failed(const axk::Error &error) {
    return {error.code == ErrorCode::operation_cancelled ? "operation_cancelled" : "conversion_failed", error.message};
}

Json record_json(const Record &record, std::size_t offset, std::size_t limit) {
    const auto state = record.state == State::ready       ? "READY"
                       : record.state == State::accepted  ? "ACCEPTED"
                       : record.state == State::executing ? "EXECUTING"
                                                          : "TERMINAL";
    Json result{{"planId", record.id},
                {"state", state},
                {"expiresAt", record.expires_unix_ms},
                {"jobId", record.job_id.empty() ? Json(nullptr) : Json(record.job_id)},
                {"report", nullptr}};
    if (!record.payload)
        return result;
    const auto &payload = *record.payload;
    const auto &report = payload.prepared->report();
    Json actions = Json::array(), issues = Json::array();
    for (std::size_t index = offset; index < report.actions.size() && index - offset < limit; ++index) {
        const auto &action = report.actions[index];
        auto details = Json::array();
        for (const auto &detail : action.details)
            details.push_back({{"label", detail.label}, {"value", detail.value}});
        actions.push_back({{"kind", action.kind},
                           {"name", action.name},
                           {"sourceObject", action.source_object},
                           {"action", action.action},
                           {"details", std::move(details)}});
    }
    for (std::size_t index = offset; index < report.issues.size() && index - offset < limit; ++index) {
        const auto &issue = report.issues[index];
        issues.push_back({{"id", issue.id},
                          {"code", issue.code},
                          {"impact", conversion::impact_name(issue.impact)},
                          {"sourceObject", issue.source_object},
                          {"sourceParameter", issue.source_parameter},
                          {"targetObject", issue.target_object},
                          {"targetParameter", issue.target_parameter},
                          {"message", issue.message}});
        if (!issue.decision.empty())
            issues.back()["decision"] = issue.decision;
    }
    result["report"] = {{"profileId", report.profile_id},
                        {"fidelity", report.fidelity == conversion::Fidelity::best ? "BEST" : "STRICT"},
                        {"executable", report.executable},
                        {"actions", std::move(actions)},
                        {"issues", std::move(issues)},
                        {"actionCount", report.actions.size()},
                        {"issueCount", report.issues.size()},
                        {"offset", offset}};
    auto &out = result["report"];
    if (!report.source_objects.empty()) {
        out["sourceCount"] = report.source_objects.size();
        auto &rows = out["sourceObjects"] = Json::array();
        for (std::size_t index = offset; index < report.source_objects.size() && index - offset < limit; ++index)
            rows.push_back(report.source_objects[index]);
    }
    if (const auto *edit = std::get_if<EditTarget>(&payload.target)) {
        out["capacity"] = capacity_admission_json(std::get<VolumeCapacityAdmission>(report.destination));
        out["destination"] = {{"imageId", edit->destination.image_id},
                              {"documentId", edit->destination.document_id},
                              {"expectedRevision", edit->revision}};
    } else if (std::holds_alternative<ImageSetDelivery>(payload.target)) {
        out["capacity"] = nullptr;
        out["destination"] = std::get<conversion::NewImageSetReview>(report.destination);
        out["destination"]["kind"] = "NEW_IMAGE_SET";
    } else {
        const auto &digest = std::get<ImageDelivery>(payload.target).digest;
        out["capacity"] = nullptr;
        out["destination"] = {{"kind", "NEW_IMAGE"},
                              {"sizeBytes", std::get<conversion::NewImageReview>(report.destination).size_bytes},
                              {"sha256", digest.empty() ? Json(nullptr) : Json(digest)}};
        if (const auto &file = std::get<ImageDelivery>(payload.target).file)
            out["destination"]["file"] = {{"rootId", file->reference().root_id},
                                          {"relativePath", file->reference().relative_path}};
    }
    return result;
}
} // namespace

struct ConversionService::Implementation {
    const Sandbox &sandbox;
    UploadStore &uploads;
    ImageSessionManager &images;
    AlterationJournalStore &journals;
    JobManager &jobs;
    DownloadArchiveStore &downloads;
    conversion::Registry profiles;
    native::FormatRegistry formats;
    std::shared_ptr<PlanStore> store{std::make_shared<PlanStore>()};
    JobManager::SubscriptionId subscription{};
    std::unique_ptr<ExpiryCollector> collector{};

    Result<Json> prepare(const Json &input, const OperationContext &context) {
        auto token = store->reserve(context.owner_id);
        if (!token)
            return std::unexpected(token.error());
        struct Reservation {
            PlanStore &store;
            std::string_view id, owner;
            ~Reservation() { store.abandon(id, owner); }
        } reservation{*store, *token, context.owner_id};
        try {
            if (!input.is_object() || input.size() != (input.contains("capacityPolicy") ? 6U : 5U) ||
                !input.at("destination").is_object())
                return std::unexpected(
                    Error{"invalid_request", "Conversion request contains unknown or missing fields"});
            const auto profile_id = input.at("profileId").get<std::string>();
            const auto profile = std::ranges::find(profiles.profiles(), profile_id, &conversion::Profile::id);
            if (profile == profiles.profiles().end())
                return std::unexpected(Error{"invalid_request", "Unknown conversion profile"});
            const auto &destination = input.at("destination");
            const bool new_image = profile->output_kind != conversion::OutputKind::existing_image;
            std::optional<SandboxOutputFile> output_file;
            if (profile->output_kind == conversion::OutputKind::new_image && destination.contains("file")) {
                const auto &file = destination.at("file");
                if (!file.is_object() || file.size() != 2U)
                    return std::unexpected(Error{"invalid_request", "Expected an output file reference"});
                auto bound = sandbox.bind_output_file(
                    {file.at("rootId").get<std::string>(), file.at("relativePath").get<std::string>()}, false);
                if (!bound)
                    return std::unexpected(bound.error());
                output_file = std::move(*bound);
            }
            if (new_image
                    ? (destination.value("kind", std::string{}) != conversion::output_kind_name(profile->output_kind) ||
                       destination.size() != (output_file ? 2U : 1U) || input.contains("capacityPolicy"))
                    : destination.size() != 4U)
                return std::unexpected(
                    Error{"invalid_request", "Destination or capacity policy does not match this conversion profile"});
            const auto fidelity = input.at("fidelity").get<std::string>();
            if (fidelity != "BEST" && fidelity != "STRICT")
                return std::unexpected(Error{"invalid_request", "Fidelity must be BEST or STRICT"});
            auto source = open_source(input.at("source"), formats, profile->source_format, sandbox, uploads, images,
                                      context, conversion::Limits{}.maximum_bytes);
            if (!source)
                return std::unexpected(source.error());
            const auto budget = source_budget(*source, conversion::Limits{}.maximum_bytes);
            if (!budget)
                return std::unexpected(budget.error());
            conversion::Request request{source->document,
                                        {profile->destination_format, {}, {}, {}},
                                        input.at("options"),
                                        fidelity == "STRICT" ? conversion::Fidelity::strict
                                                             : conversion::Fidelity::best,
                                        {},
                                        {}};
            request.native_keys_by_id = source->native_keys_by_id;
            auto payload = std::make_shared<Payload>();
            std::optional<ImageSessionRead> held;
            if (!new_image) {
                const auto target = image_document_ref(destination);
                if (!destination.at("expectedRevision").is_number_integer() || destination.at("expectedRevision") < 1)
                    return std::unexpected(Error{"invalid_request", "Destination revision must be a positive integer"});
                const auto revision = destination.at("expectedRevision").get<std::uint64_t>();
                auto summary = images.inspect(target.image_id, context.owner_id);
                if (!summary)
                    return std::unexpected(summary.error());
                const auto selected =
                    std::ranges::find(summary->documents, target.document_id, &ImageDocumentSummary::id);
                if (selected == summary->documents.end() || selected->format_id != profile->destination_format)
                    return std::unexpected(Error{"invalid_request", "Destination document does not match the profile"});
                if (std::ranges::find(selected->available_operations, "images.alter.objects") ==
                    selected->available_operations.end())
                    return std::unexpected(
                        Error{"image_mutation_unsupported", "Destination document does not support object writes"});
                auto session = images.begin_read(target, context.owner_id, revision);
                if (!session)
                    return std::unexpected(session.error());
                auto fingerprint = session->content_fingerprint(context.cancellation);
                if (!fingerprint)
                    return std::unexpected(fingerprint.error());
                auto policy = capacity_policy(input);
                if (!policy)
                    return std::unexpected(policy.error());
                request.destination = {selected->format_id, session->reader, session->source.relative_path,
                                       destination.at("selection")};
                request.capacity_policy = *policy;
                payload->target = EditTarget{target, revision, std::move(*fingerprint), *policy};
                held.emplace(std::move(*session));
            }
            request.limits.maximum_bytes -= budget->preparing;
            auto prepared = profiles.prepare(profile_id, request, context.cancellation, context.progress);
            if (!prepared)
                return std::unexpected(failed(prepared.error()));
            if (held) {
                if (auto verified = held->verify_source_unchanged(); !verified)
                    return std::unexpected(verified.error());
            } else if (std::holds_alternative<conversion::NewImageSet>((*prepared)->output())) {
                payload->target = ImageSetDelivery{};
            } else {
                ImageDelivery delivery;
                delivery.file = std::move(output_file);
                const auto &image = std::get<conversion::NewImage>((*prepared)->output());
                if (!image.bytes.empty()) {
                    auto hash = detail::reader_sha256(PlanBytes(*prepared, image.bytes), context.cancellation);
                    if (!hash)
                        return std::unexpected(hash.error());
                    delivery.digest = std::move(*hash);
                }
                payload->target = std::move(delivery);
            }
            payload->prepared = std::move(*prepared);
            payload->sources = source->document->sources();
            payload->verify_workspace = std::move(source->verify_current);
            payload->source_retention_bytes = budget->retained;
            auto published = store->publish(*token, context.owner_id, std::move(payload));
            if (!published)
                return std::unexpected(published.error());
            return record_json(*published, 0U, 200U);
        } catch (const Json::exception &) {
            return std::unexpected(Error{"invalid_request", "Malformed conversion request"});
        }
    }

    Result<Json> apply(const Json &input, const OperationContext &context) {
        const auto id = input.at("planId").get<std::string>();
        auto claimed = store->start(id, context.owner_id);
        if (!claimed)
            return std::unexpected(claimed.error());
        const auto &payload = **claimed;
        if (payload.verify_workspace) {
            if (auto current = payload.verify_workspace(context.cancellation); !current)
                return std::unexpected(current.error());
        }
        if (!std::holds_alternative<EditTarget>(payload.target)) {
            if (auto current = payload.sources->verify(context.cancellation); !current)
                return std::unexpected(failed(current.error()));
            if (auto active = context.cancellation.check(); !active)
                return std::unexpected(failed(active.error()));
            const auto *delivery = std::get_if<ImageDelivery>(&payload.target);
            if (delivery && delivery->file) {
                const auto &target = delivery->file->reference();
                const auto &image = std::get<conversion::NewImage>(payload.prepared->output());
                const auto verify = [&]() -> Result<void> {
                    if (auto current = payload.sources->verify(context.cancellation); !current)
                        return std::unexpected(failed(current.error()));
                    return payload.verify_workspace ? payload.verify_workspace(context.cancellation) : Result<void>{};
                };
                auto published = sandbox.publish_file(*delivery->file, PlanBytes{payload.prepared, image.bytes},
                                                      context.cancellation, verify);
                if (!published)
                    return std::unexpected(published.error());
                Json result{{"planId", id},
                            {"file", {{"rootId", target.root_id}, {"relativePath", target.relative_path}}}};
                for (const auto &warning : published->warnings)
                    result["warnings"].push_back(
                        {{"code", warning.code}, {"message", warning.message}, {"context", Json::object()}});
                return result;
            }
            auto file = [&]() -> Result<DownloadArchiveSnapshot> {
                if (delivery) {
                    const auto &image = std::get<conversion::NewImage>(payload.prepared->output());
                    return downloads.retain(context.owner_id, "conversion." + image.extension,
                                            "application/x-raw-disk-image", image.bytes);
                }
                auto stage = sandbox.create_staging_directory("axklib-conversion-set");
                if (!stage)
                    return std::unexpected(stage.error());
                file_operations_internal::DirectoryCleanup cleanup{*stage};
                if (auto written =
                        conversion::stage_image_set(*payload.prepared, *stage, context.cancellation, context.progress);
                    !written)
                    return std::unexpected(failed(written.error()));
                if (auto current = payload.sources->verify(context.cancellation); !current)
                    return std::unexpected(failed(current.error()));
                if (payload.verify_workspace) {
                    if (auto current = payload.verify_workspace(context.cancellation); !current)
                        return std::unexpected(current.error());
                }
                return downloads.create_owned_directory(context.owner_id, *stage, "conversion.tar",
                                                        context.cancellation, context.progress);
            }();
            if (!file)
                return std::unexpected(file.error());
            return Json{{"planId", id},
                        {"download",
                         {{"archiveId", file->reference.archive_id},
                          {"filename", file->filename},
                          {"sizeBytes", file->size_bytes},
                          {"expiresInSeconds", file->expires_in_seconds},
                          {"contentPath", "/api/v1/download-archives/" + file->reference.archive_id + "/content"}}}};
        }
        const auto &edit = std::get<EditTarget>(payload.target);
        // Release the target read mutex before mutation admission, including same-workspace conversions.
        {
            auto read = images.begin_read(edit.destination, context.owner_id, edit.revision);
            if (!read)
                return std::unexpected(read.error());
            auto fingerprint = read->content_fingerprint(context.cancellation);
            if (!fingerprint)
                return std::unexpected(fingerprint.error());
            if (*fingerprint != edit.destination_fingerprint ||
                read->reader->size() !=
                    std::get<conversion::ExistingImagePatches>(payload.prepared->output()).destination_size)
                return std::unexpected(
                    Error{"conversion_plan_stale", "Conversion destination changed; prepare a new plan"});
        }
        std::vector<AlterationJournalPatch> patches;
        const auto &retained = std::get<conversion::ExistingImagePatches>(payload.prepared->output()).patches;
        patches.reserve(retained.size());
        for (const auto &patch : retained)
            patches.push_back({patch.offset,
                               AlterationJournalBytes{std::make_shared<PlanBytes>(payload.prepared, patch.original), 0U,
                                                      patch.original.size()},
                               AlterationJournalBytes{std::make_shared<PlanBytes>(payload.prepared, patch.replacement),
                                                      0U, patch.replacement.size()}});
        auto mutation = images.begin_mutation(edit.destination, context.owner_id, edit.revision, context.mutation_id);
        if (!mutation)
            return std::unexpected(mutation.error());
        const auto verify = [&]() -> Result<void> {
            auto current = payload.sources->verify(context.cancellation);
            return current ? Result<void>{} : std::unexpected(failed(current.error()));
        };
        auto committed =
            commit_prepared_session(images, journals, *mutation, patches,
                                    std::get<VolumeCapacityAdmission>(payload.prepared->report().destination),
                                    edit.capacity_policy, context, verify);
        if (!committed)
            return std::unexpected(committed.error());
        return Json{{"planId", id},
                    {"imageId", edit.destination.image_id},
                    {"documentId", edit.destination.document_id},
                    {"revision", committed->revision},
                    {"applied", !patches.empty()},
                    {"cleanupPending", !journals.storage_ready()},
                    {"warnings", Json::array()}};
    }
};

ConversionService::ConversionService(const Sandbox &sandbox, UploadStore &uploads, ImageSessionManager &images,
                                     AlterationJournalStore &journals, JobManager &jobs,
                                     DownloadArchiveStore &downloads, conversion::Registry profiles)
    : implementation_(std::make_shared<Implementation>(
          Implementation{sandbox, uploads, images, journals, jobs, downloads, std::move(profiles), {}})) {
    auto &impl = *implementation_;
    if (auto added = impl.formats.add(tx16w::provider()); !added)
        throw std::logic_error(added.error().message);
    if (auto added = impl.formats.add(typhoon::provider()); !added)
        throw std::logic_error(added.error().message);
    impl.subscription = jobs.subscribe([store = impl.store](const JobEvent &event) {
        if (event.operation_id == "conversions.execute" && is_terminal(event.state))
            store->finish_job(event.job_id, event.owner_id);
    });
    impl.collector = std::make_unique<ExpiryCollector>(impl.store);
}
ConversionService::~ConversionService() {
    implementation_->collector.reset();
    implementation_->jobs.unsubscribe(implementation_->subscription);
}

Result<void> ConversionService::bind(OperationRegistry &registry) {
    if (auto bound = registry.bind("conversions.inspect",
                                   [impl = implementation_](const Json &input, const OperationContext &context) {
                                       return inspect_conversion_source(input, impl->profiles, impl->formats,
                                                                        impl->sandbox, impl->uploads, impl->images,
                                                                        context);
                                   });
        !bound)
        return bound;
    if (auto bound = registry.bind("images.companionSources.resolve",
                                   [impl = implementation_](const Json &input, const OperationContext &context) {
                                       return resolve_companion_sources(input, impl->formats, impl->sandbox,
                                                                        impl->uploads, impl->images, context);
                                   });
        !bound)
        return bound;
    if (auto bound = registry.bind("conversions.prepare",
                                   [impl = implementation_](const Json &input, const OperationContext &context) {
                                       return impl->prepare(input, context);
                                   });
        !bound)
        return bound;
    return registry.bind("conversions.execute",
                         [impl = implementation_](const Json &input, const OperationContext &context) {
                             return impl->apply(input, context);
                         });
}

nlohmann::json ConversionService::profiles() const { return conversion::profiles_json(implementation_->profiles); }

Result<nlohmann::json> ConversionService::plan(std::string_view id, std::string_view owner, std::size_t offset,
                                               std::size_t limit) {
    if (limit == 0U || limit > 200U)
        return std::unexpected(Error{"invalid_request", "Conversion report page limit must be 1..200"});
    auto record = implementation_->store->get(id, owner);
    if (!record)
        return std::unexpected(record.error());
    return record_json(*record, offset, limit);
}
Result<void> ConversionService::release(std::string_view id, std::string_view owner) {
    return implementation_->store->release(id, owner);
}
Result<JobSnapshot> ConversionService::execute(std::string_view id, const Json &request, OperationContext context) {
    try {
        if (!request.is_object() || request.size() != 1U)
            return std::unexpected(Error{"invalid_request", "Execution accepts acknowledgements only"});
        const auto acknowledged = request.at("acknowledgedIssueIds").get<std::vector<std::string>>();
        if (acknowledged.size() > conversion::Limits{}.maximum_issues)
            return std::unexpected(Error{"invalid_request", "Too many conversion acknowledgements"});
        const auto owner = context.owner_id;
        auto job = implementation_->store->submit(id, owner, acknowledged, [&]() -> Result<std::string> {
            auto submitted = implementation_->jobs.submit("conversions.execute", {{"planId", id}}, std::move(context),
                                                          "conversion:" + std::string{id});
            if (!submitted)
                return std::unexpected(submitted.error());
            return submitted->job_id;
        });
        if (!job)
            return std::unexpected(job.error());
        return implementation_->jobs.status(*job, owner);
    } catch (const Json::exception &) {
        return std::unexpected(Error{"invalid_request", "Malformed conversion acknowledgements"});
    }
}

} // namespace axk::app
