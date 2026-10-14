#include "axklib/conversion/file_plan.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

#include "axklib/conversion/tx16w_to_a_series/profile.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/formats/tx16w/document.hpp"
#include "axklib/native/registry.hpp"
#include "file_sources.hpp"

namespace axk::conversion {
nlohmann::json profiles_json() {
    const auto profile = tx16w::a_series::profile();
    return {{"profiles", nlohmann::json::array({{{"id", profile.id},
                                                 {"name", profile.name},
                                                 {"sourceFormat", profile.source_format},
                                                 {"destinationFormat", profile.destination_format},
                                                 {"optionsSchema", profile.options_schema}}})}};
}
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::io, std::move(message));
}
} // namespace
struct FilePlan::Impl {
    std::shared_ptr<const PreparedPlan> prepared;
    Report report;
    file_internal::Sources sources;
    file_internal::Binding destination;
    std::chrono::steady_clock::time_point expires;
    std::mutex mutex;
    bool consumed{};
};
FilePlan::FilePlan(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FilePlan::FilePlan(FilePlan &&) noexcept = default;
FilePlan &FilePlan::operator=(FilePlan &&) noexcept = default;
FilePlan::~FilePlan() = default;
const Report &FilePlan::report() const noexcept {
    static const Report empty;
    return impl_ ? impl_->report : empty;
}

Result<FilePlan> FilePlan::prepare(const FileRequest &request, const CancellationToken &cancellation,
                                   ProgressSink *progress) {
    Registry registry;
    if (const auto added = registry.add(tx16w::a_series::profile()); !added)
        return std::unexpected(added.error());
    const auto profile = std::ranges::find(registry.profiles(), request.profile_id, &Profile::id);
    if (profile == registry.profiles().end())
        return std::unexpected(invalid("Unknown conversion profile"));
    auto sources = file_internal::admit(request.sources, cancellation);
    if (!sources)
        return std::unexpected(sources.error());
    native::FormatRegistry formats;
    if (const auto added = formats.add(tx16w::provider()); !added)
        return std::unexpected(added.error());
    const auto opened = formats.open(sources->sources, {profile->source_format}, cancellation);
    if (!opened)
        return std::unexpected(opened.error());
    if (opened->selection_required || opened->documents.size() != 1U)
        return std::unexpected(invalid("Select exactly one native source document"));
    const auto destination = file_internal::bind(request.destination, cancellation);
    if (!destination)
        return std::unexpected(destination.error());
    const auto reader = FileReader::open(destination->path);
    if (!reader)
        return std::unexpected(reader.error());
    Request input{
        opened->documents.front(), {profile->destination_format, *reader, destination->path, request.selection},
        request.options,           request.fidelity,
        request.capacity_policy,   {}};
    // Source snapshots have a separate, fixed 64 MiB admission bound within the plan's 512 MiB limit.
    input.limits.maximum_bytes -= 64U * 1024U * 1024U;
    auto prepared = registry.prepare(request.profile_id, input, cancellation, progress);
    if (!prepared)
        return std::unexpected(prepared.error());
    const auto current = file_internal::bind(destination->path, cancellation);
    if (!current || current->digest != destination->digest || current->size != destination->size)
        return std::unexpected(invalid("Destination changed during preparation"));
    if (const auto checked = file_internal::verify(*sources, cancellation); !checked)
        return std::unexpected(checked.error());
    auto impl = std::make_unique<Impl>();
    impl->prepared = std::move(*prepared);
    impl->report = impl->prepared->report();
    impl->sources = std::move(*sources);
    impl->sources.sources.reset();
    impl->destination = *destination;
    impl->expires = std::chrono::steady_clock::now() + std::chrono::minutes{15};
    return FilePlan{std::move(impl)};
}

Result<PublicationOutcome> FilePlan::apply(const std::filesystem::path &output,
                                           std::span<const std::string> acknowledged,
                                           const CancellationToken &cancellation, ProgressSink *progress) {
    if (!impl_)
        return std::unexpected(invalid("Conversion plan has been moved"));
    std::shared_ptr<const PreparedPlan> prepared;
    {
        const std::scoped_lock lock{impl_->mutex};
        if (impl_->consumed || std::chrono::steady_clock::now() >= impl_->expires)
            return std::unexpected(invalid("Conversion plan is consumed or expired; prepare a new plan"));
        if (const auto reviewed = impl_->prepared->acknowledge(acknowledged); !reviewed)
            return std::unexpected(reviewed.error());
        impl_->consumed = true;
        prepared = std::move(impl_->prepared);
    }
    if (const auto checked = file_internal::verify(impl_->sources, cancellation); !checked)
        return std::unexpected(checked.error());
    const auto source = FileReader::open(impl_->destination.path);
    if (!source)
        return std::unexpected(source.error());
    if ((*source)->size() != impl_->destination.size)
        return std::unexpected(invalid("Destination changed; prepare a new plan"));
    auto temporary = detail::TemporaryPublication::create(output);
    if (!temporary)
        return std::unexpected(temporary.error());
    // Hash the exact bytes copied, so changes between verification and copying cannot be published.
    package_internal::Sha256State digest;
    std::vector<std::byte> buffer(1024U * 1024U);
    for (std::uint64_t offset = 0U; offset < (*source)->size();) {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), (*source)->size() - offset));
        const auto bytes = std::span{buffer}.first(count);
        if (const auto read = (*source)->read_exact_at(offset, bytes); !read)
            return std::unexpected(read.error());
        digest.update(bytes);
        if (const auto written = temporary->append(bytes); !written)
            return std::unexpected(written.error());
        offset += count;
        if (progress)
            progress->report({ProgressPhase::writing, offset, (*source)->size(), "Copying conversion destination", {}});
    }
    if (digest.finish() != impl_->destination.digest)
        return std::unexpected(invalid("Destination changed; prepare a new plan"));
    for (const auto &patch : prepared->patches()) {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        if (const auto written = temporary->write_at(patch.offset, patch.replacement); !written)
            return std::unexpected(written.error());
    }
    if (const auto checked = file_internal::verify(impl_->sources, cancellation); !checked)
        return std::unexpected(checked.error());
    if (const auto active = cancellation.check(); !active)
        return std::unexpected(active.error());
    if (progress)
        progress->report({ProgressPhase::publishing, 0U, 1U, "Publishing conversion output", {}});
    return temporary->publish(detail::PublicationMode::create_only);
}

nlohmann::json report_json(const Report &report) {
    auto actions = nlohmann::json::array();
    for (const auto &action : report.actions) {
        auto details = nlohmann::json::array();
        for (const auto &detail : action.details)
            details.push_back({{"label", detail.label}, {"value", detail.value}});
        actions.push_back({{"kind", action.kind},
                           {"name", action.name},
                           {"sourceObject", action.source_object},
                           {"action", action.action},
                           {"details", std::move(details)}});
    }
    auto issues = nlohmann::json::array();
    for (const auto &issue : report.issues)
        issues.push_back({{"id", issue.id},
                          {"code", issue.code},
                          {"impact", impact_name(issue.impact)},
                          {"sourceObject", issue.source_object},
                          {"sourceParameter", issue.source_parameter},
                          {"targetObject", issue.target_object},
                          {"targetParameter", issue.target_parameter},
                          {"message", issue.message}});
    return {{"profileId", report.profile_id},
            {"fidelity", report.fidelity == Fidelity::strict ? "STRICT" : "BEST"},
            {"executable", report.executable},
            {"actions", std::move(actions)},
            {"issues", std::move(issues)},
            {"capacity",
             {{"allowed", report.capacity.allowed}, {"target", a_series_load_target_name(report.capacity.target)}}}};
}
} // namespace axk::conversion
