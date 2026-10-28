#include "device_sessions_internal.hpp"

#include <algorithm>
#include <format>

namespace axk::app::device_detail {
std::string carrier_fence(const Connection &connection) {
    const auto &candidate = connection.device.candidate;
    const auto text = Json{{"model", candidate.model},
                           {"serial", candidate.serial},
                           {"size", candidate.size_bytes},
                           {"sector", candidate.sector_size}}
                          .dump();
    return *digest(std::as_bytes(std::span{text.data(), text.size()}));
}

Result<void> require_clear_carrier(const std::filesystem::path &directory, std::string_view fence,
                                   std::string_view except_directory) {
    std::error_code error;
    std::size_t count{};
    for (std::filesystem::directory_iterator at{directory, error}, end; !error && at != end; at.increment(error)) {
        if (++count > 4096U || at->is_symlink(error) || !at->is_directory(error))
            return std::unexpected(
                Error{"device_state_unavailable", "Device operation storage cannot be safely inspected"});
        const auto record_path = at->path() / "operation.json";
        if (!std::filesystem::exists(record_path, error)) {
            for (std::filesystem::directory_iterator entry{at->path(), error}, last; !error && entry != last;
                 entry.increment(error))
                if (entry->path().extension() == ".axkjournal")
                    return std::unexpected(
                        Error{"device_recovery_required", "An orphan device journal requires review"});
            if (error)
                return std::unexpected(
                    Error{"device_state_unavailable", "Unpublished device operation cannot be inspected"});
            continue;
        }
        auto record = load_json(record_path);
        if (!record)
            return std::unexpected(record.error());
        try {
            const auto state = record->at("state").get<std::string>();
            if (state == "committed" || state == "rolled_back" ||
                (state == "no_write" && record->value("fenced", false)))
                continue;
            if (at->path().filename().string() == except_directory)
                continue;
            if (record->at("carrierFence").get<std::string>() == fence)
                return std::unexpected(
                    Error{"device_recovery_required", "Resolve the existing carrier operation before writing"});
        } catch (const Json::exception &) {
            return std::unexpected(Error{"device_state_unavailable", "Device operation safety record is malformed"});
        }
    }
    if (error)
        return std::unexpected(Error{"device_state_unavailable", "Device operation storage is unavailable"});
    if (count >= 4096U && except_directory.empty())
        return std::unexpected(
            Error{"device_operation_capacity", "Device operation retention is full; no new write can be admitted"});
    return {};
}

std::string device_confirmation(const DeviceCandidate &candidate, const DeviceTargetDescription &target,
                                std::string_view verb) {
    auto locator = candidate.identity.device_path;
    if (locator.starts_with(R"(\\.\)"))
        locator.erase(0U, 4U);
    return std::string{verb} + " " + locator + (target.scsi ? " SCSI " + std::to_string(target.scsi->id) : "");
}

Result<void> verify_physical_guards(const Connection &connection, const Guards &guards, std::uint64_t target_offset,
                                    std::span<const AlterationJournalPatch> excluded,
                                    const CancellationToken &cancellation) {
    if (guards.size() > maximum_guard_sectors)
        return std::unexpected(Error{"device_guard_capacity", "Physical guard budget is exhausted"});
    for (auto at = guards.begin(); at != guards.end();) {
        if (cancellation.is_cancelled())
            return std::unexpected(Error{"operation_cancelled", "Device verification cancelled"});
        const auto base = at->first;
        auto end = std::next(at);
        while (end != guards.end() && end->first == std::prev(end)->first + 512U && end->first - base < 64U * 1024U)
            ++end;
        const auto size = std::prev(end)->first - base + 512U;
        if (base % 512U != 0U || base > connection.device.transport->size() ||
            size > connection.device.transport->size() - base)
            return std::unexpected(Error{"device_guard_invalid", "Physical guard range is invalid"});
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (auto read = connection.device.transport->read_exact_at(base, bytes); !read)
            return std::unexpected(Error{"device_read_failed", read.error().message});
        for (; at != end; ++at) {
            const auto overwritten = std::ranges::any_of(excluded, [&](const auto &patch) {
                return at->first >= target_offset + patch.offset &&
                       at->first - target_offset - patch.offset < patch.replacement.size();
            });
            if (!overwritten &&
                *digest(std::span{bytes}.subspan(static_cast<std::size_t>(at->first - base), 512U)) != at->second)
                return std::unexpected(
                    Error{"device_changed", "Reviewed device metadata changed; review the device again"});
        }
    }
    return {};
}
} // namespace axk::app::device_detail
