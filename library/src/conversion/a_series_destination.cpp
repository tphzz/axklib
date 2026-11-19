#include "a_series_destination.hpp"

#include "axklib/native_bundle_media.hpp"
#include <algorithm>
#include <utility>

namespace axk::conversion {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, std::move(message));
}
struct Medium {
    BuildManifestKind format;
    const char *suffix, *name, *label, *medium, *extension;
};
constexpr Medium fresh_media[] = {
    {BuildManifestKind::fat12_floppy, "floppy", " floppy", "A-series - Floppy (FAT12)", "fat12-1440k", "img"},
    {BuildManifestKind::iso9660, "iso", " CD-ROM", "A-series - CD-ROM (ISO9660)", "iso9660", "iso"},
    {BuildManifestKind::hds, "sfs", " new hard disk", "A-series - New hard disk (SFS)", "sfs-hard-disk", "hds"}};
} // namespace

std::vector<Profile> fresh_volume_profiles(const Profile &base, const VolumePrepare &prepare) {
    std::vector<Profile> result;
    for (const auto medium : fresh_media) {
        auto schema = base.options_schema;
        schema["properties"]["volumeName"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 16}};
        schema["required"].push_back("volumeName");
        if (medium.format == BuildManifestKind::hds) {
            schema["properties"]["sizeBytes"] = {
                {"type", "integer"}, {"minimum", minimum_hds_size}, {"maximum", maximum_hds_size}, {"multipleOf", 512}};
            schema["properties"]["partitionCount"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}};
            schema["properties"]["partitionIndex"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 7}};
            for (auto name : {"sizeBytes", "partitionCount", "partitionIndex"})
                schema["required"].push_back(name);
        }
        const auto id = base.id + "." + medium.suffix;
        auto route = RouteDescriptor{medium.label,
                                     medium.medium,
                                     {"PLAYABLE_INSTRUMENTS", "WAVE_DATA"},
                                     {"disk-set-selection", "fresh-a-series-media", "new-image-file"},
                                     0};
        result.push_back(
            {id, base.name + medium.name, base.source_format, base.destination_format, std::move(schema),
             [id, medium, prepare](const Request &request, const CancellationToken &token,
                                   ProgressSink *progress) -> Result<std::shared_ptr<const PreparedPlan>> {
                 auto name = request.options.find("volumeName");
                 if (name == request.options.end() || !name->is_string())
                     return std::unexpected(invalid("A new A-series image requires a volume name"));
                 const auto &value = name->get_ref<const std::string &>();
                 if (value.empty() || value.size() > 16U || value.front() == ' ' || value.back() == ' ' ||
                     !std::ranges::all_of(value, [](unsigned char c) { return c >= 0x20U && c <= 0x7eU; }))
                     return std::unexpected(invalid(
                         "Enter a volume name of 1 to 16 printable ASCII characters without surrounding spaces"));
                 FreshVolume fresh{{medium.format, value}, id, medium.extension};
                 auto source_request = request;
                 source_request.options.erase("volumeName");
                 if (medium.format == BuildManifestKind::hds) {
                     const auto numeric = [&request](const char *key, std::uint64_t low,
                                                     std::uint64_t high) -> std::optional<std::uint64_t> {
                         const auto it = request.options.find(key);
                         if (it == request.options.end() || !it->is_number_integer() || *it < low || *it > high)
                             return {};
                         return it->get<std::uint64_t>();
                     };
                     const auto size = numeric("sizeBytes", minimum_hds_size, maximum_hds_size);
                     const auto count = numeric("partitionCount", 1, 8);
                     const auto index = numeric("partitionIndex", 0, 7);
                     if (!size || !count || !index || *index >= *count || *size % 512U)
                         return std::unexpected(invalid(
                             "Expected sector-aligned SFS size, partition count and a partition within that count"));
                     fresh.media.size_bytes = *size;
                     fresh.media.partition_count = static_cast<std::uint8_t>(*count);
                     fresh.media.partition_index = static_cast<std::uint8_t>(*index);
                     for (auto key : {"sizeBytes", "partitionCount", "partitionIndex"})
                         source_request.options.erase(key);
                 }
                 return prepare(source_request, token, progress, &fresh);
             },
             OutputKind::new_image, std::move(route)});
    }
    return result;
}

std::shared_ptr<const PreparedPlan> blocked_volume_plan(const Request &request, Report report) {
    const auto bytes = report_bytes(report);
    if (!request.destination.reader)
        return std::make_shared<const PreparedPlan>(std::move(report), NewImage{}, bytes);
    return std::make_shared<const PreparedPlan>(std::move(report), std::vector<Patch>{},
                                                request.destination.reader->size(), bytes);
}
Result<a_series::authoring::Destination> volume_destination(const nlohmann::json &selection) {
    try {
        if (!selection.is_object() || selection.size() != 3U)
            return std::unexpected(invalid("Expected destination partition, volume kind and name"));
        a_series::authoring::Destination destination;
        const auto &partition = selection.at("partitionIndex");
        if (!partition.is_number_integer() || partition < 0 || partition > 7)
            return std::unexpected(invalid("Destination partition index must be between 0 and 7"));
        const auto &kind = selection.at("kind");
        if (kind != "EXISTING_VOLUME" && kind != "CREATE_VOLUME")
            return std::unexpected(invalid("Select an existing or new destination volume"));
        destination = {partition.get<std::uint8_t>(), selection.at("volumeName").get<std::string>(),
                       kind == "CREATE_VOLUME"};
        if (destination.volume_name.empty())
            return std::unexpected(invalid("Conversion requires a destination volume name"));
        return destination;
    } catch (const nlohmann::json::exception &) {
        return std::unexpected(invalid("Malformed conversion destination"));
    }
}
Result<std::shared_ptr<const PreparedPlan>> finish_volume_plan(const Request &request,
                                                               const a_series::authoring::Destination &destination,
                                                               a_series::authoring::Bundle bundle, Report report,
                                                               const CancellationToken &cancellation,
                                                               ProgressSink *progress, const FreshVolume *fresh) {
    if (fresh) {
        const auto review_bytes = report_bytes(report);
        if (review_bytes >= request.limits.maximum_bytes)
            return std::unexpected(invalid("A-series conversion review exceeds the memory limit"));
        auto image = a_series::authoring::build_media(std::move(bundle), fresh->media,
                                                      (request.limits.maximum_bytes - review_bytes) / 2U, cancellation);
        if (!image) {
            if (image.error().code != ErrorCode::unsupported_profile &&
                image.error().code != ErrorCode::io_unsupported_size &&
                image.error().code != ErrorCode::transaction_rejected)
                return std::unexpected(image.error());
            report.issues.push_back({"issue-" + std::to_string(report.issues.size() + 1U),
                                     "a-series.media-admission",
                                     Impact::blocker,
                                     {},
                                     {},
                                     {},
                                     {},
                                     image.error().message});
            report.executable = false;
            return blocked_volume_plan(request, std::move(report));
        }
        auto retained = review_bytes + image->capacity();
        return std::make_shared<const PreparedPlan>(std::move(report), NewImage{std::move(*image), fresh->extension},
                                                    retained);
    }
    const auto policy = request.capacity_policy.value_or(VolumeCapacityPolicy{});
    auto authored = a_series::authoring::prepare(request.destination.reader, request.destination.label, destination,
                                                 bundle, policy, cancellation, progress);
    if (!authored)
        return std::unexpected(authored.error());
    const auto &admission = report.destination.emplace<VolumeCapacityAdmission>(std::move(authored->capacity));
    if (const auto admitted = enforce_volume_capacity_admission(admission, policy); !admitted) {
        report.executable = false;
        report.issues.push_back({"issue-" + std::to_string(report.issues.size() + 1U),
                                 "conversion.capacity",
                                 Impact::blocker,
                                 {},
                                 {},
                                 {},
                                 {},
                                 admitted.error().message});
    }
    auto retained_bytes = report_bytes(report);
    if (retained_bytes > request.limits.maximum_bytes || report.issues.size() > request.limits.maximum_issues)
        return std::unexpected(invalid("Conversion capacity report exceeds its retention limit"));
    if (!report.executable)
        return std::make_shared<const PreparedPlan>(std::move(report), std::vector<Patch>{}, authored->image_size_bytes,
                                                    retained_bytes);
    std::vector<Patch> patches;
    patches.reserve(authored->patches.size());
    const auto patch_overhead = patches.capacity() * sizeof(Patch);
    if (patch_overhead > request.limits.maximum_bytes - retained_bytes)
        return std::unexpected(invalid("Conversion patch list exceeds its retention limit"));
    retained_bytes += patch_overhead;
    for (auto &patch : authored->patches) {
        const auto bytes = patch.original.capacity() + patch.replacement.capacity();
        if (bytes > request.limits.maximum_bytes - retained_bytes)
            return std::unexpected(invalid("Conversion patches exceed their retained memory limit"));
        retained_bytes += bytes;
        patches.push_back({patch.offset, std::move(patch.original), std::move(patch.replacement)});
    }
    return std::make_shared<const PreparedPlan>(std::move(report), std::move(patches), authored->image_size_bytes,
                                                retained_bytes);
}
} // namespace axk::conversion
