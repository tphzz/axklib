#include "axklib/application/su700_import_operations.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "axklib/su700.hpp"
#include "filesystem_inputs.hpp"

namespace axk::app {
namespace {
using Json = nlohmann::json;
class FloppyFileReader final : public RandomAccessReader {
    std::shared_ptr<const FatImage> image_;
    FatFile file_;

  public:
    FloppyFileReader(std::shared_ptr<const FatImage> image, FatFile file)
        : image_(std::move(image)), file_(std::move(file)) {}
    std::uint64_t size() const noexcept override { return file_.size; }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        auto bytes = image_->read_file_range(file_, offset, destination.size());
        if (!bytes)
            return std::unexpected(bytes.error());
        std::ranges::copy(*bytes, destination.begin());
        return {};
    }
};
Result<Json> run(const Json &request, const OperationContext &context, const Sandbox &sandbox, UploadStore &uploads,
                 ImageSessionManager &images, AlterationJournalStore &journals, bool execute) {
    auto input = filesystem_inputs::open(request.at("source"), context.owner_id, sandbox, uploads);
    if (!input)
        return std::unexpected(input.error());
    if (input->reader->size() > 4U * 1024U * 1024U)
        return std::unexpected(
            Error{"unsupported_operation", "Choose one FAT12 floppy image, not a hard-disk image or disk set."});
    if (execute) {
        if (auto verified = input->verify(request.at("expectedSource"), context.cancellation); !verified)
            return std::unexpected(verified.error());
    }
    auto snapshot = input->snapshot(context.cancellation);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    auto opened = FatImage::open(input->reader, {}, context.cancellation);
    if (auto checked = context.cancellation.check(); !checked)
        return std::unexpected(Error{"operation_cancelled", "SU700 inspection was cancelled"});
    if (!opened)
        return std::unexpected(Error{"su700_source_invalid", opened.error().message});
    auto fat = std::make_shared<FatImage>(std::move(*opened));
    auto inspection = inspect_su700_floppy(*fat, context.cancellation);
    if (auto checked = context.cancellation.check(); !checked)
        return std::unexpected(Error{"operation_cancelled", "SU700 inspection was cancelled"});
    if (!inspection)
        return std::unexpected(Error{"su700_source_invalid", inspection.error().message});
    const auto &plan = *inspection;
    Json files = Json::array();
    const std::array roles{"CONTROL", "SONG", "SAMPLE", "EXTRA"};
    for (const auto &file : plan.files)
        files.push_back({{"sourcePath", file.source_path},
                         {"relativePath", file.destination_path},
                         {"sizeBytes", file.size_bytes},
                         {"role", roles.at(static_cast<std::size_t>(file.role))}});
    Json result{{"status", plan.status == Su700ImportStatus::complete    ? "COMPLETE"
                           : plan.status == Su700ImportStatus::unrelated ? "UNRELATED"
                                                                         : "UNSUPPORTED"},
                {"issue", plan.issue},
                {"snapshot", *snapshot},
                {"suggestedVolumeName", plan.suggested_volume_name},
                {"songCount", plan.song_count},
                {"sampleCount", plan.sample_count},
                {"files", files},
                {"destinationReady", false},
                {"totalBytes", 0U}};
    if (plan.status != Su700ImportStatus::complete) {
        if (execute)
            return std::unexpected(
                Error{"su700_source_invalid", plan.issue.empty() ? "Not a supported SU700 floppy." : plan.issue});
        return result;
    }
    const auto &destination = request.at("destination");
    if (destination.is_null()) {
        if (execute)
            return std::unexpected(Error{"invalid_request", "An import destination is required."});
        return result;
    }
    const auto image_id = destination.at("imageId").get<std::string>();
    const auto revision = destination.at("expectedRevision").get<std::uint64_t>();
    const auto root = destination.at("rootEntryId").get<std::string>();
    const auto name = destination.at("volumeName").get<std::string>();
    std::set<std::string> extras;
    if (request.at("includedExtras").is_null()) {
        for (const auto &file : plan.files)
            if (file.role == Su700ImportRole::extra)
                extras.insert(file.source_path);
    } else {
        const auto names = request.at("includedExtras").get<std::vector<std::string>>();
        for (const auto &extra : names) {
            if (!extras.insert(extra).second || !std::ranges::any_of(plan.files, [&](const auto &file) {
                    return file.role == Su700ImportRole::extra && file.source_path == extra;
                }))
                return std::unexpected(Error{"invalid_request", "Unknown or duplicate extra file selection."});
        }
    }
    std::vector<FilesystemEdit> edits{CreateFilesystemDirectory{{name}}, CreateFilesystemDirectory{{name, "SUSQ"}},
                                      CreateFilesystemDirectory{{name, "SUSP"}}};
    std::uint64_t total{};
    for (const auto &file : plan.files) {
        if (file.role == Su700ImportRole::extra && !extras.contains(file.source_path))
            continue;
        auto path = file.destination_path;
        path.insert(path.begin(), name);
        const auto source = std::ranges::find(fat->files(), file.source_path, &FatFile::path);
        if (source == fat->files().end())
            return std::unexpected(Error{"su700_source_invalid", "An inspected source disappeared."});
        edits.emplace_back(PutFilesystemFile{std::move(path), std::make_shared<FloppyFileReader>(fat, *source)});
        total += file.size_bytes;
    }
    auto reviewed =
        images.inspect_su700_import(image_id, context.owner_id, revision, root, name, edits, context.cancellation);
    if (!reviewed) {
        if (execute)
            return std::unexpected(reviewed.error());
        result["issue"] = reviewed.error().message;
        return result;
    }
    if (auto verified = input->verify(*snapshot, context.cancellation); !verified)
        return std::unexpected(verified.error());
    if (!execute) {
        result["destinationReady"] = true;
        result["totalBytes"] = total;
        return result;
    }
    auto updated = apply_filesystem_edits(images, journals, image_id, context.owner_id, revision, reviewed->partition,
                                          reviewed->edits, context.cancellation, context.progress,
                                          [&]() { return input->verify(*snapshot); });
    if (!updated)
        return std::unexpected(updated.error());
    return Json{{"imageId", image_id},
                {"revision", updated->revision},
                {"rootEntryId", root},
                {"volumeName", name},
                {"warnings", Json::array()}};
}
} // namespace

Result<void> bind_su700_import_operations(OperationRegistry &registry, const Sandbox &sandbox, UploadStore &uploads,
                                          ImageSessionManager &images, AlterationJournalStore &journals) {
    for (const bool execute : {false, true}) {
        const std::string operation = execute ? "images.su700.import" : "images.su700.import.inspect";
        if (registry.is_implemented(operation))
            continue;
        auto bound = registry.bind(
            operation, [&, execute](const Json &request, const OperationContext &context) -> Result<Json> {
                try {
                    return run(request, context, sandbox, uploads, images, journals, execute);
                } catch (const Json::exception &) {
                    return std::unexpected(
                        Error{"invalid_request", "SU700 import request is incomplete or malformed."});
                }
            });
        if (!bound)
            return bound;
        bound = registry.bind_path_accesses(
            operation, [](const Json &request, const OperationContext &) -> Result<std::vector<PathAccess>> {
                try {
                    const auto &source = request.at("source");
                    if (!source.contains("fileRef"))
                        return std::vector<PathAccess>{};
                    const auto &ref = source.at("fileRef");
                    return std::vector<PathAccess>{
                        {{ref.at("rootId").get<std::string>(), ref.at("relativePath").get<std::string>()},
                         PathAccessMode::shared}};
                } catch (const Json::exception &) {
                    return std::unexpected(Error{"invalid_request", "Invalid SU700 input reference."});
                }
            });
        if (!bound)
            return bound;
    }
    return {};
}
} // namespace axk::app
