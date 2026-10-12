#include "device_sessions_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <limits>
#include <optional>

#include <hash-library/sha256.h>

#include "axklib/file_publication.hpp"
#include "device_io_timing.hpp"

namespace axk::app::device_detail {

Result<std::string> digest(std::span<const std::byte> bytes) {
    SHA256 hash;
    hash.add(bytes.data(), bytes.size());
    return hash.getHash();
}

Result<void> save_json(const std::filesystem::path &path, const Json &value, bool create, ReadCounters *counters) {
    std::optional<IoTiming> timing;
    if (counters)
        timing.emplace(counters->state_publications, counters->state_publication_nanoseconds);
    const auto text = value.dump();
    auto publication = axk::detail::TemporaryPublication::create(path, [&](const axk::detail::TemporaryFileSink &sink) {
        return sink(std::as_bytes(std::span{text.data(), text.size()}));
    });
    if (!publication)
        return std::unexpected(Error{"device_state_unavailable", publication.error().message});
    auto published = publication->publish(create ? axk::detail::PublicationMode::create_only
                                                 : axk::detail::PublicationMode::replace_existing);
    if (!published || published->durability != PublicationDurability::confirmed)
        return std::unexpected(Error{"device_state_unavailable", "device operation state durability is unconfirmed"});
    return {};
}

Result<void> verify_bound(const Connection &connection) {
    const IoTiming timing{connection.reads.binding_checks, connection.reads.binding_nanoseconds};
    return connection.device.transport->verify_bound();
}

Result<Json> load_json(const std::filesystem::path &path) {
    auto reader = FileReader::open(path);
    if (!reader || (*reader)->size() > 16U * 1024U * 1024U)
        return std::unexpected(Error{"device_state_unavailable", "device operation record is unavailable"});
    std::string text(static_cast<std::size_t>((*reader)->size()), '\0');
    if (auto read = (*reader)->read_exact_at(0U, std::as_writable_bytes(std::span{text.data(), text.size()})); !read)
        return std::unexpected(Error{"device_state_unavailable", read.error().message});
    auto parsed = Json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return std::unexpected(Error{"device_state_unavailable", "device operation record is malformed"});
    return parsed;
}

const DeviceTargetDescription *find_target(const Connection &connection, std::string_view id) {
    const auto found = std::ranges::find(connection.device.targets, id, &DeviceTargetDescription::id);
    return found == connection.device.targets.end() ? nullptr : &*found;
}

std::string target_identity(const Connection &connection, const DeviceTargetDescription &target) {
    const auto text = std::format("{}:{}:{}:{}:{}", connection.device.transport->media_identity(), target.id,
                                  target.offset, target.size_bytes, target.sector_size);
    return *digest(std::as_bytes(std::span{text.data(), text.size()}));
}

axk::Result<void> read_target(const Connection &connection, const DeviceTargetDescription &target, std::uint64_t offset,
                              std::span<std::byte> bytes) {
    if (connection.invalidated || offset > target.size_bytes || bytes.size() > target.size_bytes - offset)
        return std::unexpected(make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                          "device session is invalidated or read is outside target"));
    const auto started = std::chrono::steady_clock::now();
    const auto result = connection.device.transport->read_exact_at(target.offset + offset, bytes);
    ++connection.reads.calls;
    connection.reads.bytes += bytes.size();
    connection.reads.nanoseconds += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    return result;
}

Result<void> verify_guards(const Connection &connection, const DeviceTargetDescription &target, const Guards &guards,
                           std::span<const AlterationJournalPatch> excluded, const CancellationToken &cancellation,
                           ProgressSink *progress, std::string_view label) {
    ++connection.reads.verification_passes;
    const auto sector_size = connection.device.transport->sector_size();
    std::vector<std::byte> buffer;
    std::uint64_t completed{};
    const auto report = [&] {
        if (progress)
            progress->report(
                {ProgressPhase::validating, completed, guards.size() * sector_size, std::string{label}, {}});
    };
    report();
    for (auto at = guards.begin(); at != guards.end();) {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(Error{"operation_cancelled", active.error().message});
        const auto offset = at->first;
        if (offset % sector_size != 0U || offset > target.size_bytes || sector_size > target.size_bytes - offset)
            return std::unexpected(Error{"device_state_unavailable", "device guard is outside the target"});
        const auto overlaps = std::ranges::any_of(excluded, [&](const auto &patch) {
            return offset < patch.offset + patch.original.size() && patch.offset < offset + sector_size;
        });
        if (overlaps) {
            completed += sector_size;
            ++at;
            report();
            continue;
        }
        auto end = std::next(at);
        while (end != guards.end() && end->first == std::prev(end)->first + sector_size &&
               end->first - offset < 1024U * 1024U &&
               !std::ranges::any_of(
                   excluded,
                   [&](const auto &patch) {
                       return end->first < patch.offset + patch.original.size() &&
                              patch.offset < end->first + sector_size;
                   }))
            ++end;
        buffer.resize(static_cast<std::size_t>(std::prev(end)->first - offset + sector_size));
        ++connection.reads.verification_reads;
        if (auto read = read_target(connection, target, offset, buffer); !read)
            return std::unexpected(Error{"device_changed", read.error().message});
        for (; at != end; ++at) {
            const auto sector = std::span{buffer}.subspan(static_cast<std::size_t>(at->first - offset), sector_size);
            if (*digest(sector) != at->second)
                return std::unexpected(
                    Error{"device_changed", "device metadata changed; reopen and review the operation"});
            connection.reads.verified_bytes += sector_size;
        }
        completed += buffer.size();
        report();
    }
    if (const auto active = cancellation.check(); !active)
        return std::unexpected(Error{"operation_cancelled", active.error().message});
    return {};
}

Source::Source(std::shared_ptr<Connection> connection, DeviceTargetDescription target)
    : connection_(std::move(connection)), target_(std::move(target)), revision_(connection_->revision),
      initial_reads_(connection_->reads) {}
std::uint64_t Source::size() const noexcept { return target_.size_bytes; }

axk::Result<void> Source::read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const {
    const std::scoped_lock lock{connection_->io};
    if (connection_->invalidated || connection_->revision != revision_ || offset > size() ||
        bytes.size() > size() - offset)
        return std::unexpected(make_error(ErrorCode::io_read_failed, ErrorCategory::io, "device read is unavailable"));
    const auto sector_size = connection_->device.transport->sector_size();
    return cache_.read(
        size(), sector_size, offset, bytes,
        [&](std::uint64_t at, std::span<std::byte> data) {
            auto read = read_target(*connection_, target_, at, data);
            if (!read) {
                connection_->invalidated = true;
                connection_->authorized_target.clear();
            }
            return read;
        },
        [&](std::uint64_t start, std::span<const std::byte> data) -> axk::Result<void> {
            if (sealed_)
                return {};
            for (std::size_t at = 0U; at < data.size(); at += sector_size) {
                if (guards_.contains(start + at))
                    continue;
                if (guards_.size() >= maximum_guard_sectors)
                    return std::unexpected(make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                                      "device metadata guard budget exhausted"));
                guards_.emplace(start + at, *digest(data.subspan(at, sector_size)));
            }
            return {};
        });
}

Result<void> Source::seal(const CancellationToken &cancellation, ProgressSink *progress) {
    const std::scoped_lock lock{connection_->io};
    if (connection_->invalidated || connection_->revision != revision_)
        return std::unexpected(Error{"device_changed", "device session changed before metadata verification"});
    if (auto verified = verify_guards(*connection_, target_, guards_, {}, cancellation, progress); !verified)
        return verified;
    connection_->metadata_guards[target_.id] = guards_;
    sealed_ = true;
    return {};
}

Mutation::Mutation(std::shared_ptr<Connection> value, DeviceTargetDescription description, std::string operation,
                   std::filesystem::path path, bool recovery)
    : connection(std::move(value)), target(std::move(description)), operation_id(std::move(operation)),
      directory(std::move(path)), lease_(connection->io), writing_(recovery), initial_reads_(connection->reads) {
    connection->mutation_active = true;
    guards = connection->metadata_guards[target.id];
}
Mutation::~Mutation() { connection->mutation_active = false; }
std::uint64_t Mutation::size() const noexcept { return target.size_bytes; }
std::uint32_t Mutation::write_alignment() const noexcept { return connection->device.transport->sector_size(); }

axk::Result<void> Mutation::read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const {
    if (writing_ || journal_streaming_ || bytes.empty())
        return read_target(*connection, target, offset, bytes);
    const auto alignment = write_alignment();
    return cache_.read(
        size(), alignment, offset, bytes,
        [&](std::uint64_t at, std::span<std::byte> data) { return read_target(*connection, target, at, data); },
        [&](std::uint64_t start, std::span<const std::byte> data) -> axk::Result<void> {
            for (std::size_t at = 0U; at < data.size(); at += alignment) {
                const auto hash = *digest(data.subspan(at, alignment));
                const auto existing = guards.find(start + at);
                if (existing != guards.end() && existing->second != hash)
                    return std::unexpected(make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                                      "device changed during mutation planning"));
                if (existing == guards.end() && guards.size() >= maximum_guard_sectors)
                    return std::unexpected(make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                                      "device planning guard budget exhausted"));
                guards.try_emplace(start + at, hash);
            }
            return {};
        });
}
Result<void> Mutation::write_exact_at(std::uint64_t offset, std::span<const std::byte> bytes) {
    if (offset > size() || bytes.size() > size() - offset)
        return std::unexpected(Error{"device_write_range", "device write is outside the selected target"});
    if (!writing_) {
        Json entries = Json::array();
        for (const auto &[at, hash] : guards)
            entries.push_back({{"offset", at}, {"sha256", hash}});
        if (auto saved =
                save_json(directory / "guards.json", {{"sectors", std::move(entries)}}, false, &connection->reads);
            !saved)
            return saved;
        ++connection->revision;
    }
    cache_.clear();
    writing_ = true;
    const IoTiming timing{connection->reads.writes, connection->reads.write_nanoseconds};
    connection->reads.written_bytes += bytes.size();
    return connection->device.transport->write_exact_at(target.offset + offset, bytes);
}
Result<void> Mutation::flush() {
    const IoTiming timing{connection->reads.flushes, connection->reads.flush_nanoseconds};
    return connection->device.transport->flush();
}
Result<void> Mutation::verify_bound() const { return device_detail::verify_bound(*connection); }
Result<void> Mutation::verify_unchanged() const {
    if (auto checked = verify_bound(); !checked)
        return checked;
    return verify_guards(*connection, target, guards);
}
std::string Mutation::revision() const { return std::format("{}:{}", connection->id, connection->revision); }
std::string Mutation::stable_identity() const { return target_identity(*connection, target); }
MutationIdentity Mutation::journal_identity() const {
    return {MutationSourceKind::device, connection->device.transport->media_identity(), target.id};
}
Result<void> Mutation::set_state(std::string_view state) const {
    auto record = load_json(directory / "operation.json");
    if (!record)
        return std::unexpected(record.error());
    (*record)["state"] = state;
    return save_json(directory / "operation.json", *record, false, &connection->reads);
}

namespace {
Json metrics(const Connection &connection, const ReadCounters &initial, const MetadataReadCache &cache,
             std::size_t guards) {
    return {{"physicalReads", connection.reads.calls - initial.calls},
            {"physicalBytes", connection.reads.bytes - initial.bytes},
            {"readTimeUs", (connection.reads.nanoseconds - initial.nanoseconds) / 1000U},
            {"bindingChecks", connection.reads.binding_checks - initial.binding_checks},
            {"bindingTimeUs", (connection.reads.binding_nanoseconds - initial.binding_nanoseconds) / 1000U},
            {"physicalWrites", connection.reads.writes - initial.writes},
            {"writtenBytes", connection.reads.written_bytes - initial.written_bytes},
            {"writeTimeUs", (connection.reads.write_nanoseconds - initial.write_nanoseconds) / 1000U},
            {"flushes", connection.reads.flushes - initial.flushes},
            {"flushTimeUs", (connection.reads.flush_nanoseconds - initial.flush_nanoseconds) / 1000U},
            {"journalPublications", connection.reads.journal_publications - initial.journal_publications},
            {"journalPublicationTimeUs",
             (connection.reads.journal_publication_nanoseconds - initial.journal_publication_nanoseconds) / 1000U},
            {"statePublications", connection.reads.state_publications - initial.state_publications},
            {"statePublicationTimeUs",
             (connection.reads.state_publication_nanoseconds - initial.state_publication_nanoseconds) / 1000U},
            {"cacheHits", cache.hits},
            {"cacheMisses", cache.misses},
            {"verificationPasses", connection.reads.verification_passes - initial.verification_passes},
            {"verificationReads", connection.reads.verification_reads - initial.verification_reads},
            {"verifiedBytes", connection.reads.verified_bytes - initial.verified_bytes},
            {"verificationGapBytes", connection.reads.verification_gap_bytes - initial.verification_gap_bytes},
            {"guardSectors", guards}};
}
} // namespace

Json Source::read_metrics() const {
    const std::scoped_lock lock{connection_->io};
    return metrics(*connection_, initial_reads_, cache_, guards_.size());
}

Json Mutation::read_metrics() const { return metrics(*connection, initial_reads_, cache_, guards.size()); }

Json device_read_metrics(const RandomAccessReader *reader) {
    if (const auto *source = dynamic_cast<const Source *>(reader))
        return source->read_metrics();
    if (const auto *mutation = dynamic_cast<const Mutation *>(reader))
        return mutation->read_metrics();
    return Json::object();
}

void record_device_journal_publication(const RandomAccessReader *reader, std::chrono::steady_clock::duration elapsed) {
    if (const auto *mutation = dynamic_cast<const Mutation *>(reader)) {
        ++mutation->connection->reads.journal_publications;
        mutation->connection->reads.journal_publication_nanoseconds +=
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }
}

} // namespace axk::app::device_detail
