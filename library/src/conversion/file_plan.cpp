#include "axklib/conversion/file_plan.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

#include "axklib/conversion/image_set_publication.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/formats/tx16w/document.hpp"
#include "axklib/formats/typhoon/document.hpp"
#include "axklib/native/registry.hpp"
#include "file_sources.hpp"

namespace axk::conversion {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::io, std::move(message));
}
} // namespace
struct FilePlan::Impl {
    std::shared_ptr<const PreparedPlan> prepared;
    Report report;
    std::string image_digest;
    file_internal::Sources sources;
    std::optional<file_internal::Binding> destination;
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
                                   ProgressSink *progress, const Registry &registry) {
    const auto profile = std::ranges::find(registry.profiles(), request.profile_id, &Profile::id);
    if (profile == registry.profiles().end())
        return std::unexpected(invalid("Unknown conversion profile"));
    const bool new_image = profile->output_kind != OutputKind::existing_image;
    if (new_image && (!request.destination.empty() || !request.selection.is_null() || request.capacity_policy))
        return std::unexpected(
            invalid("New-image conversion accepts no destination template, selection or capacity policy"));
    if (!new_image && request.destination.empty())
        return std::unexpected(invalid("This conversion profile requires a destination template"));
    auto sources = file_internal::admit(request.sources, cancellation);
    if (!sources)
        return std::unexpected(sources.error());
    native::FormatRegistry formats;
    if (const auto added = formats.add(std::move(sources->sfs).provider()); !added)
        return std::unexpected(added.error());
    if (const auto added = formats.add(tx16w::provider()); !added)
        return std::unexpected(added.error());
    if (const auto added = formats.add(typhoon::provider()); !added)
        return std::unexpected(added.error());
    const auto opened = formats.open_one(sources->sources, profile->source_format, cancellation);
    if (!opened)
        return std::unexpected(opened.error());
    std::optional<file_internal::Binding> destination;
    Request input{*opened,
                  {profile->destination_format, {}, {}, request.selection},
                  request.options,
                  request.fidelity,
                  request.capacity_policy,
                  {}};
    if (!new_image) {
        auto binding = file_internal::bind(request.destination, cancellation);
        if (!binding)
            return std::unexpected(binding.error());
        destination = std::move(*binding);
        const auto reader = FileReader::open(destination->path);
        if (!reader)
            return std::unexpected(reader.error());
        input.destination.reader = *reader;
        input.destination.label = destination->path;
    }
    // Source snapshots have a separate, fixed 64 MiB admission bound within the plan's 512 MiB limit.
    input.limits.maximum_bytes -= 64U * 1024U * 1024U;
    auto prepared = registry.prepare(request.profile_id, input, cancellation, progress);
    if (!prepared)
        return std::unexpected(prepared.error());
    if (destination) {
        const auto current = file_internal::bind(destination->path, cancellation);
        if (!current || current->digest != destination->digest || current->size != destination->size)
            return std::unexpected(invalid("Destination changed during preparation"));
    }
    if (const auto checked = file_internal::verify(*sources, cancellation); !checked)
        return std::unexpected(checked.error());
    auto impl = std::make_unique<Impl>();
    impl->prepared = std::move(*prepared);
    impl->report = impl->prepared->report();
    if (const auto *image = std::get_if<NewImage>(&impl->prepared->output()); image && !image->bytes.empty())
        impl->image_digest = package_internal::hex_digest(package_internal::sha256(image->bytes));
    impl->sources = std::move(*sources);
    impl->sources.sources.reset();
    impl->destination = std::move(destination);
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
    if (std::holds_alternative<NewImageSet>(prepared->output()))
        return publish_image_set(
            *prepared, output, [&] { return file_internal::verify(impl_->sources, cancellation); }, cancellation,
            progress);
    auto temporary = detail::TemporaryPublication::create(output);
    if (!temporary)
        return std::unexpected(temporary.error());
    if (const auto *image = std::get_if<NewImage>(&prepared->output())) {
        if (const auto written = temporary->append(image->bytes); !written)
            return std::unexpected(written.error());
    } else {
        const auto &destination = impl_->destination.value();
        const auto source = FileReader::open(destination.path);
        if (!source)
            return std::unexpected(source.error());
        if ((*source)->size() != destination.size)
            return std::unexpected(invalid("Destination changed; prepare a new plan"));
        // Hash the exact bytes copied, so changes between verification and copying cannot be published.
        package_internal::Sha256State digest;
        std::vector<std::byte> buffer(1024U * 1024U);
        for (std::uint64_t offset = 0U; offset < (*source)->size();) {
            if (const auto active = cancellation.check(); !active)
                return std::unexpected(active.error());
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), (*source)->size() - offset));
            const auto bytes = std::span{buffer}.first(count);
            if (const auto read = (*source)->read_exact_at(offset, bytes); !read)
                return std::unexpected(read.error());
            digest.update(bytes);
            if (const auto written = temporary->append(bytes); !written)
                return std::unexpected(written.error());
            offset += count;
            if (progress)
                progress->report(
                    {ProgressPhase::writing, offset, (*source)->size(), "Copying conversion destination", {}});
        }
        if (digest.finish() != destination.digest)
            return std::unexpected(invalid("Destination changed; prepare a new plan"));
        for (const auto &patch : std::get<ExistingImagePatches>(prepared->output()).patches) {
            if (const auto active = cancellation.check(); !active)
                return std::unexpected(active.error());
            if (const auto written = temporary->write_at(patch.offset, patch.replacement); !written)
                return std::unexpected(written.error());
        }
    }
    if (const auto checked = file_internal::verify(impl_->sources, cancellation); !checked)
        return std::unexpected(checked.error());
    if (progress)
        progress->report({ProgressPhase::publishing, 0U, 1U, "Publishing conversion output", {}});
    if (const auto active = cancellation.check(); !active)
        return std::unexpected(active.error());
    return temporary->publish(detail::PublicationMode::create_only);
}

nlohmann::json FilePlan::report_json() const {
    const auto &report = this->report();
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
    for (const auto &issue : report.issues) {
        issues.push_back({{"id", issue.id},
                          {"code", issue.code},
                          {"impact", impact_name(issue.impact)},
                          {"sourceObject", issue.source_object},
                          {"sourceParameter", issue.source_parameter},
                          {"targetObject", issue.target_object},
                          {"targetParameter", issue.target_parameter},
                          {"message", issue.message}});
        if (!issue.decision.empty())
            issues.back()["decision"] = issue.decision;
    }
    nlohmann::json result{{"profileId", report.profile_id},
                          {"fidelity", report.fidelity == Fidelity::strict ? "STRICT" : "BEST"},
                          {"executable", report.executable},
                          {"actions", std::move(actions)},
                          {"issues", std::move(issues)}};
    if (!report.source_objects.empty()) {
        result["sourceCount"] = report.source_objects.size();
        result["sourceObjects"] = report.source_objects;
    }
    if (const auto *capacity = std::get_if<VolumeCapacityAdmission>(&report.destination)) {
        result["capacity"] = {{"allowed", capacity->allowed}, {"target", a_series_load_target_name(capacity->target)}};
    } else if (const auto *set = std::get_if<NewImageSetReview>(&report.destination)) {
        result["newImageSet"] = *set;
    } else {
        const auto &image = std::get<NewImageReview>(report.destination);
        result["newImage"] = {
            {"sizeBytes", image.size_bytes},
            {"sha256", impl_->image_digest.empty() ? nlohmann::json(nullptr) : nlohmann::json(impl_->image_digest)}};
    }
    return result;
}
} // namespace axk::conversion
