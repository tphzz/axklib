#include "filesystem_image_inputs.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "axklib/application/secure_random.hpp"
#include "axklib/media.hpp"
#include "content_digest.hpp"

namespace axk::app {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t maximum_bytes = 4U * 1024U * 1024U;
constexpr std::uint64_t reservation_bytes = maximum_bytes * 4U;
constexpr std::uint64_t maximum_retained = 512U * 1024U * 1024U;
struct Record {
    std::string owner;
    Clock::time_point expires;
    filesystem_inputs::OpenedInput source;
    Json snapshot;
    std::map<std::string, std::shared_ptr<const RandomAccessReader>, std::less<>> files;
};
Result<std::vector<std::string>> components(std::string_view path) {
    std::vector<std::string> result;
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto name = path.substr(0, slash);
        if (name.empty() || name == "." || name == ".." || name.find_first_of("\\\0", 0U, 2U) != name.npos)
            return std::unexpected(Error{"filesystem_image_invalid", "The image contains an unsafe path"});
        result.emplace_back(name);
        if (slash == path.npos)
            break;
        path.remove_prefix(slash + 1U);
    }
    if (result.empty() || result.size() > 63U)
        return std::unexpected(Error{"filesystem_image_invalid", "The image path exceeds supported bounds"});
    return result;
}
} // namespace

struct FilesystemImageInputs::State {
    std::function<Clock::time_point()> now;
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Record>, std::less<>> records;
    std::vector<std::weak_ptr<Record>> live;
    std::uint64_t pending{};
    void cleanup() {
        std::erase_if(records, [this](const auto &item) { return item.second->expires <= now(); });
        std::erase_if(live, [](const auto &record) { return record.expired(); });
    }
};

FilesystemImageInputs::FilesystemImageInputs(const Sandbox &sandbox, UploadStore &uploads,
                                             std::function<Clock::time_point()> now)
    : state_(std::make_shared<State>()), sandbox_(sandbox), uploads_(uploads) {
    state_->now = std::move(now);
}

Result<Json> FilesystemImageInputs::inspect(const Json &source, const OperationContext &context) {
    {
        std::lock_guard lock{state_->mutex};
        state_->cleanup();
        if ((state_->live.size() + state_->pending + 1U) * reservation_bytes > maximum_retained)
            return std::unexpected(Error{"filesystem_image_capacity", "Close another image import before continuing"});
        ++state_->pending;
    }
    const auto admission = std::shared_ptr<void>(nullptr, [state = state_](void *) {
        std::lock_guard lock{state->mutex};
        --state->pending;
    });
    auto input = filesystem_inputs::open(source, context.owner_id, sandbox_, uploads_);
    if (!input)
        return std::unexpected(input.error());
    if (input->reader->size() > maximum_bytes)
        return std::unexpected(Error{"filesystem_image_invalid", "Contents import supports floppy images up to 4 MiB"});
    auto snapshot = input->snapshot(context.cancellation);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    std::vector<std::byte> bytes(static_cast<std::size_t>(input->reader->size()));
    if (auto read = input->reader->read_exact_at(0U, bytes); !read)
        return std::unexpected(Error{"filesystem_image_invalid", read.error().message});
    const auto reader = std::make_shared<MemoryReader>(std::move(bytes));
    auto digest = detail::reader_sha256(*reader, context.cancellation);
    if (!digest)
        return std::unexpected(digest.error());
    if (*digest != snapshot->at("sha256").get<std::string>())
        return std::unexpected(Error{"filesystem_input_changed", "The image changed during inspection"});
    auto fat = FatImage::open(reader, {}, context.cancellation);
    if (!fat)
        return std::unexpected(Error{"filesystem_image_invalid", fat.error().message});
    if (fat->files().size() + fat->directories().size() > 8192U)
        return std::unexpected(Error{"filesystem_image_invalid", "Too many floppy entries"});
    auto token = secure_random_hex(32U);
    if (!token)
        return std::unexpected(token.error());
    auto record = std::make_shared<Record>(
        Record{context.owner_id, state_->now() + std::chrono::minutes{15}, std::move(*input), *snapshot, {}});
    Json entries = Json::array();
    std::uint64_t payload_bytes{};
    std::size_t metadata_bytes{};
    auto append = [&](std::string_view path, bool directory, const std::string &id,
                      const Json &file_snapshot) -> Result<void> {
        auto parts = components(path);
        if (!parts)
            return std::unexpected(parts.error());
        metadata_bytes += path.size() * 2U + 512U;
        if (metadata_bytes > maximum_bytes)
            return std::unexpected(Error{"filesystem_image_invalid", "Floppy directory metadata exceeds the limit"});
        entries.push_back(
            {{"entryId", id}, {"relativePath", *parts}, {"directory", directory}, {"snapshot", file_snapshot}});
        return {};
    };
    for (const auto &directory : fat->directories()) {
        if (auto added = append(directory.path, true, "d" + std::to_string(entries.size()), nullptr); !added)
            return std::unexpected(added.error());
    }
    for (const auto &file : fat->files()) {
        if (file.size > maximum_bytes - payload_bytes)
            return std::unexpected(Error{"filesystem_image_invalid", "Floppy file data exceeds supported bounds"});
        payload_bytes += file.size;
        auto data = fat->read_file(file, context.cancellation);
        if (!data)
            return std::unexpected(Error{"filesystem_image_invalid", data.error().message});
        const auto id = "f" + std::to_string(entries.size());
        const auto payload = std::make_shared<MemoryReader>(std::move(*data));
        filesystem_inputs::OpenedInput opened{payload, *token + ":" + id, {}, []() -> Result<void> { return {}; }};
        auto file_snapshot = opened.snapshot(context.cancellation);
        if (!file_snapshot)
            return std::unexpected(file_snapshot.error());
        record->files.emplace(id, payload);
        if (auto added = append(file.path, false, id, *file_snapshot); !added)
            return std::unexpected(added.error());
    }
    if (auto verified = record->source.verify(record->snapshot, context.cancellation); !verified)
        return std::unexpected(verified.error());
    std::ranges::sort(entries.get_ref<Json::array_t &>(),
                      [](const Json &a, const Json &b) { return a.at("relativePath") < b.at("relativePath"); });
    {
        std::lock_guard lock{state_->mutex};
        if (!state_->records.emplace(*token, record).second)
            return std::unexpected(Error{"secure_random_failed", "Inspection token collision"});
        state_->live.push_back(record);
    }
    return Json{{"inspectionToken", *token}, {"entries", std::move(entries)}};
}

Result<FilesystemImageLease> FilesystemImageInputs::lease(std::string_view token, std::string_view owner) {
    std::lock_guard lock{state_->mutex};
    state_->cleanup();
    const auto found = state_->records.find(token);
    if (found == state_->records.end() || found->second->owner != owner)
        return std::unexpected(Error{"filesystem_image_expired", "Inspect the source image again before importing"});
    const auto record = found->second;
    return FilesystemImageLease{
        [record](const CancellationToken &cancel) { return record->source.verify(record->snapshot, cancel); },
        [record, revision = std::string{token}](std::string_view id) -> Result<filesystem_inputs::OpenedInput> {
            const auto file = record->files.find(id);
            if (file == record->files.end())
                return std::unexpected(Error{"filesystem_image_entry_missing", "The inspected file does not exist"});
            return filesystem_inputs::OpenedInput{
                file->second, revision + ":" + std::string{id}, {}, [record]() -> Result<void> {
                    static_cast<void>(record); // Retain the admission while this reader is in use.
                    return {};
                }};
        }};
}

Result<void> FilesystemImageInputs::release(std::string_view token, std::string_view owner) {
    std::lock_guard lock{state_->mutex};
    const auto found = state_->records.find(token);
    if (found != state_->records.end() && found->second->owner == owner)
        state_->records.erase(found);
    return {};
}

Result<void> bind_filesystem_image_inputs(OperationRegistry &registry,
                                          const std::shared_ptr<FilesystemImageInputs> &inputs) {
    if (auto bound =
            registry.bind("filesystem.images.inspect",
                          [inputs](const Json &request, const OperationContext &context) -> Result<Json> {
                              try {
                                  return inputs->inspect(request.at("source"), context);
                              } catch (const Json::exception &) {
                                  return std::unexpected(
                                      Error{"invalid_request", "Image inspection request is incomplete or malformed"});
                              }
                          });
        !bound)
        return bound;
    if (auto bound = registry.bind_path_accesses(
            "filesystem.images.inspect",
            [](const Json &request, const OperationContext &) -> Result<std::vector<PathAccess>> {
                try {
                    const auto &source = request.at("source");
                    if (!source.contains("fileRef"))
                        return std::vector<PathAccess>{};
                    const auto &ref = source.at("fileRef");
                    return std::vector<PathAccess>{
                        {{ref.at("rootId").get<std::string>(), ref.at("relativePath").get<std::string>()},
                         PathAccessMode::shared}};
                } catch (const Json::exception &) {
                    return std::unexpected(Error{"invalid_request", "Image input reference is malformed"});
                }
            });
        !bound)
        return bound;
    return registry.bind(
        "filesystem.images.release", [inputs](const Json &request, const OperationContext &context) -> Result<Json> {
            try {
                if (auto released = inputs->release(request.at("inspectionToken").get<std::string>(), context.owner_id);
                    !released)
                    return std::unexpected(released.error());
                return Json::object();
            } catch (const Json::exception &) {
                return std::unexpected(Error{"invalid_request", "Image inspection reference is malformed"});
            }
        });
}
} // namespace axk::app
