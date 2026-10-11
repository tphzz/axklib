#include "device_sessions_internal.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>

#include <hash-library/sha256.h>

#include "axklib/file_publication.hpp"

namespace axk::app::device_detail {

Result<std::string> digest(std::span<const std::byte> bytes) {
    SHA256 hash;
    hash.add(bytes.data(), bytes.size());
    return hash.getHash();
}

Result<void> save_json(const std::filesystem::path &path, const Json &value, bool create) {
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
    return connection.device.transport->read_exact_at(target.offset + offset, bytes);
}

Result<void> verify_guards(const Connection &connection, const DeviceTargetDescription &target, const Guards &guards,
                           std::span<const AlterationJournalPatch> excluded) {
    const auto sector_size = connection.device.transport->sector_size();
    std::vector<std::byte> buffer;
    for (auto at = guards.begin(); at != guards.end();) {
        const auto offset = at->first;
        if (offset % sector_size != 0U || offset > target.size_bytes || sector_size > target.size_bytes - offset)
            return std::unexpected(Error{"device_state_unavailable", "device guard is outside the target"});
        const auto overlaps = std::ranges::any_of(excluded, [&](const auto &patch) {
            return offset < patch.offset + patch.original.size() && patch.offset < offset + sector_size;
        });
        if (overlaps) {
            ++at;
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
        if (auto read = read_target(connection, target, offset, buffer); !read)
            return std::unexpected(Error{"device_changed", read.error().message});
        for (; at != end; ++at) {
            const auto sector = std::span{buffer}.subspan(static_cast<std::size_t>(at->first - offset), sector_size);
            if (*digest(sector) != at->second)
                return std::unexpected(
                    Error{"device_changed", "device metadata changed; reopen and review the operation"});
        }
    }
    return {};
}

Source::Source(std::shared_ptr<Connection> connection, DeviceTargetDescription target)
    : connection_(std::move(connection)), target_(std::move(target)), revision_(connection_->revision) {}
std::uint64_t Source::size() const noexcept { return target_.size_bytes; }

axk::Result<void> Source::read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const {
    const std::scoped_lock lock{connection_->io};
    if (connection_->invalidated || connection_->revision != revision_ || offset > size() ||
        bytes.size() > size() - offset)
        return std::unexpected(make_error(ErrorCode::io_read_failed, ErrorCategory::io, "device read is unavailable"));
    const auto sector_size = connection_->device.transport->sector_size();
    const auto block_size = std::max<std::uint32_t>(64U * 1024U, sector_size);
    while (!bytes.empty()) {
        const auto aligned = offset - offset % block_size;
        const auto within = static_cast<std::size_t>(offset - aligned);
        auto found = cache_.find(aligned);
        if (found == cache_.end()) {
            std::vector<std::byte> data(
                static_cast<std::size_t>(std::min<std::uint64_t>(block_size, size() - aligned)));
            if (auto read = read_target(*connection_, target_, aligned, data); !read) {
                connection_->invalidated = true;
                connection_->authorized_target.clear();
                return read;
            }
            if (cache_.size() >= 64U)
                cache_.erase(cache_.begin());
            found = cache_.emplace(aligned, std::move(data)).first;
        }
        const auto count = std::min<std::size_t>(bytes.size(), found->second.size() - within);
        if (!sealed_) {
            for (auto at = within - within % sector_size; at < within + count; at += sector_size) {
                if (!guards_.contains(aligned + at) && guards_.size() >= maximum_guard_sectors)
                    return std::unexpected(make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                                      "device metadata guard budget exhausted"));
                guards_.try_emplace(aligned + at, *digest(std::span{found->second}.subspan(at, sector_size)));
            }
        }
        std::copy_n(found->second.begin() + static_cast<std::ptrdiff_t>(within), count, bytes.begin());
        bytes = bytes.subspan(count);
        offset += count;
    }
    return {};
}

Result<void> Source::seal() {
    const std::scoped_lock lock{connection_->io};
    if (auto verified = verify_guards(*connection_, target_, guards_); !verified)
        return verified;
    connection_->metadata_guards[target_.id] = guards_;
    sealed_ = true;
    return {};
}

Mutation::Mutation(std::shared_ptr<Connection> value, DeviceTargetDescription description, std::string operation,
                   std::filesystem::path path, bool recovery)
    : connection(std::move(value)), target(std::move(description)), operation_id(std::move(operation)),
      directory(std::move(path)), lease_(connection->io), writing_(recovery) {
    connection->mutation_active = true;
    guards = connection->metadata_guards[target.id];
}
Mutation::~Mutation() { connection->mutation_active = false; }
std::uint64_t Mutation::size() const noexcept { return target.size_bytes; }
std::uint32_t Mutation::write_alignment() const noexcept { return connection->device.transport->sector_size(); }

axk::Result<void> Mutation::read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const {
    if (writing_ || journal_streaming_ || bytes.empty())
        return read_target(*connection, target, offset, bytes);
    if (offset > size() || bytes.size() > size() - offset)
        return std::unexpected(
            make_error(ErrorCode::out_of_bounds, ErrorCategory::io, "device read is outside target"));
    const auto alignment = write_alignment();
    while (!bytes.empty()) {
        const auto start = offset - offset % alignment;
        const auto within = static_cast<std::size_t>(offset - start);
        const auto count = std::min<std::size_t>(bytes.size(), 64U * 1024U - within);
        const auto rounded = ((within + count + alignment - 1U) / alignment) * alignment;
        std::vector<std::byte> data(rounded);
        if (auto read = read_target(*connection, target, start, data); !read)
            return read;
        for (std::size_t at = 0U; at < data.size(); at += alignment) {
            const auto hash = *digest(std::span{data}.subspan(at, alignment));
            const auto existing = guards.find(start + at);
            if (existing != guards.end() && existing->second != hash)
                return std::unexpected(make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                                  "device changed during mutation planning"));
            if (existing == guards.end() && guards.size() >= maximum_guard_sectors)
                return std::unexpected(make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                                  "device planning guard budget exhausted"));
            guards.try_emplace(start + at, hash);
        }
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(within), count, bytes.begin());
        bytes = bytes.subspan(count);
        offset += count;
    }
    return {};
}
Result<void> Mutation::write_exact_at(std::uint64_t offset, std::span<const std::byte> bytes) {
    if (offset > size() || bytes.size() > size() - offset)
        return std::unexpected(Error{"device_write_range", "device write is outside the selected target"});
    if (!writing_) {
        Json entries = Json::array();
        for (const auto &[at, hash] : guards)
            entries.push_back({{"offset", at}, {"sha256", hash}});
        if (auto saved = save_json(directory / "guards.json", {{"sectors", std::move(entries)}}, false); !saved)
            return saved;
        ++connection->revision;
    }
    writing_ = true;
    return connection->device.transport->write_exact_at(target.offset + offset, bytes);
}
Result<void> Mutation::flush() { return connection->device.transport->flush(); }
Result<void> Mutation::verify_bound() const { return connection->device.transport->verify_bound(); }
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
    return save_json(directory / "operation.json", *record, false);
}

} // namespace axk::app::device_detail
