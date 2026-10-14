#include "axklib/application/conversions.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "axklib/application/image_session_contracts.hpp"
#include "axklib/application/volume_capacity.hpp"
#include "axklib/conversion/tx16w_to_a_series/profile.hpp"
#include "axklib/formats/tx16w/document.hpp"
#include "conversion_plan_store.hpp"
#include "conversion_sources.hpp"
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
    }
    result["report"] = {{"profileId", report.profile_id},
                        {"fidelity", report.fidelity == conversion::Fidelity::best ? "BEST" : "STRICT"},
                        {"executable", report.executable},
                        {"actions", std::move(actions)},
                        {"issues", std::move(issues)},
                        {"actionCount", report.actions.size()},
                        {"issueCount", report.issues.size()},
                        {"offset", offset},
                        {"capacity", capacity_admission_json(report.capacity)},
                        {"destination",
                         {{"imageId", payload.destination.image_id},
                          {"documentId", payload.destination.document_id},
                          {"expectedRevision", payload.revision}}}};
    return result;
}
} // namespace

struct ConversionService::Implementation {
    const Sandbox &sandbox;
    UploadStore &uploads;
    ImageSessionManager &images;
    AlterationJournalStore &journals;
    JobManager &jobs;
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
                !input.at("destination").is_object() || input.at("destination").size() != 4U)
                return std::unexpected(
                    Error{"invalid_request", "Conversion request contains unknown or missing fields"});
            const auto profile_id = input.at("profileId").get<std::string>();
            const auto profile = std::ranges::find(profiles.profiles(), profile_id, &conversion::Profile::id);
            if (profile == profiles.profiles().end())
                return std::unexpected(Error{"invalid_request", "Unknown conversion profile"});
            const auto fidelity = input.at("fidelity").get<std::string>();
            if (fidelity != "BEST" && fidelity != "STRICT")
                return std::unexpected(Error{"invalid_request", "Fidelity must be BEST or STRICT"});
            auto source =
                open_source(input.at("source"), formats, profile->source_format, sandbox, uploads, images, context);
            if (!source)
                return std::unexpected(source.error());
            // Conservatively charge native metadata, entry readers and source leases as well as output patches.
            // TX16W PCM is lazy; its decoded buffers are accounted for by the directional profile.
            const auto &catalog = source->document->catalog();
            std::uint64_t source_bytes = static_cast<std::uint64_t>(catalog.objects.size()) * 65536U +
                                         static_cast<std::uint64_t>(catalog.relationships.size() +
                                                                    catalog.nodes.size() + catalog.diagnostics.size()) *
                                             8192U;
            for (const auto &member : source->document->sources()->members()) {
                source_bytes += 65536U;
                for (const auto &root : member.roots)
                    source_bytes += static_cast<std::uint64_t>(root.entries.size()) * 16384U;
            }
            if (source_bytes >= conversion::Limits{}.maximum_bytes)
                return std::unexpected(
                    Error{"conversion_limit_exceeded", "Native source metadata exceeds the plan retention limit"});
            const auto &destination = input.at("destination");
            const auto target = image_document_ref(destination);
            if (!destination.at("expectedRevision").is_number_integer() || destination.at("expectedRevision") < 1)
                return std::unexpected(Error{"invalid_request", "Destination revision must be a positive integer"});
            const auto revision = destination.at("expectedRevision").get<std::uint64_t>();
            auto summary = images.inspect(target.image_id, context.owner_id);
            if (!summary)
                return std::unexpected(summary.error());
            const auto selected = std::ranges::find(summary->documents, target.document_id, &ImageDocumentSummary::id);
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
            conversion::Request request{
                source->document,
                {selected->format_id, session->reader, session->source.relative_path, destination.at("selection")},
                input.at("options"),
                fidelity == "STRICT" ? conversion::Fidelity::strict : conversion::Fidelity::best,
                *policy,
                {}};
            request.limits.maximum_bytes -= source_bytes;
            auto prepared = profiles.prepare(profile_id, request, context.cancellation, context.progress);
            if (!prepared)
                return std::unexpected(failed(prepared.error()));
            if (auto verified = session->verify_source_unchanged(); !verified)
                return std::unexpected(verified.error());
            auto payload =
                std::make_shared<Payload>(Payload{*prepared, target, revision, *fingerprint, *policy, source->document,
                                                  std::move(source->verify_current), source_bytes});
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
            if (auto current = payload.verify_workspace(); !current)
                return std::unexpected(current.error());
        }
        // Release the target read mutex before mutation admission, including same-workspace conversions.
        {
            auto read = images.begin_read(payload.destination, context.owner_id, payload.revision);
            if (!read)
                return std::unexpected(read.error());
            auto fingerprint = read->content_fingerprint(context.cancellation);
            if (!fingerprint)
                return std::unexpected(fingerprint.error());
            if (*fingerprint != payload.destination_fingerprint ||
                read->reader->size() != payload.prepared->destination_size())
                return std::unexpected(
                    Error{"conversion_plan_stale", "Conversion destination changed; prepare a new plan"});
        }
        std::vector<AlterationJournalPatch> patches;
        patches.reserve(payload.prepared->patches().size());
        for (const auto &patch : payload.prepared->patches())
            patches.push_back({patch.offset,
                               AlterationJournalBytes{std::make_shared<PlanBytes>(payload.prepared, patch.original), 0U,
                                                      patch.original.size()},
                               AlterationJournalBytes{std::make_shared<PlanBytes>(payload.prepared, patch.replacement),
                                                      0U, patch.replacement.size()}});
        auto mutation =
            images.begin_mutation(payload.destination, context.owner_id, payload.revision, context.mutation_id);
        if (!mutation)
            return std::unexpected(mutation.error());
        const auto verify = [&]() -> Result<void> {
            auto current = payload.source->sources()->verify(context.cancellation);
            return current ? Result<void>{} : std::unexpected(failed(current.error()));
        };
        auto committed =
            commit_prepared_session(images, journals, *mutation, patches, payload.prepared->report().capacity,
                                    payload.capacity_policy, context, verify);
        if (!committed)
            return std::unexpected(committed.error());
        return Json{{"planId", id},
                    {"imageId", payload.destination.image_id},
                    {"documentId", payload.destination.document_id},
                    {"revision", committed->revision},
                    {"applied", !patches.empty()},
                    {"cleanupPending", !journals.storage_ready()},
                    {"warnings", Json::array()}};
    }
};

ConversionService::ConversionService(const Sandbox &sandbox, UploadStore &uploads, ImageSessionManager &images,
                                     AlterationJournalStore &journals, JobManager &jobs)
    : implementation_(
          std::make_shared<Implementation>(Implementation{sandbox, uploads, images, journals, jobs, {}, {}})) {
    auto &impl = *implementation_;
    if (auto added = impl.profiles.add(tx16w::a_series::profile()); !added)
        throw std::logic_error(added.error().message);
    if (auto added = impl.formats.add(tx16w::provider()); !added)
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

nlohmann::json ConversionService::profiles() const {
    Json result = Json::array();
    for (const auto &profile : implementation_->profiles.profiles())
        result.push_back({{"id", profile.id},
                          {"name", profile.name},
                          {"sourceFormat", profile.source_format},
                          {"destinationFormat", profile.destination_format},
                          {"optionsSchema", profile.options_schema}});
    return {{"profiles", std::move(result)}};
}

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
