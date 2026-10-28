#include "device_sessions_internal.hpp"
#include "device_sfs_validation.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "axklib/bytes.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace axk::app::device_detail {
namespace {
std::optional<std::uint64_t> declared_prefix(std::span<const std::byte> bytes, std::uint64_t physical_size) {
    constexpr std::string_view magic{"YAMAHA_dev3"};
    if (std::memcmp(bytes.data(), magic.data(), magic.size()) != 0)
        return {};
    const ByteReader reader{bytes};
    const auto size = static_cast<std::uint64_t>(*reader.be32(0xa0U)) * 512U;
    if (size < physical_size)
        return size;
    return {};
}
} // namespace

Result<std::shared_ptr<Source>> prepare_source(const std::shared_ptr<Connection> &connection,
                                               const DeviceTargetDescription &target) {
    auto full = std::make_shared<Source>(connection, target);
    if (target.size_bytes <= maximum_hds_size)
        return full;
    std::array<std::byte, 1024U> headers{};
    if (auto read = full->read_exact_at(0U, headers); !read)
        return std::unexpected(Error{"device_read_failed", read.error().message});
    const auto primary = std::span{headers}.first(512U);
    const auto backup = std::span{headers}.last(512U);
    const auto first = declared_prefix(primary, target.size_bytes);
    const auto second = declared_prefix(backup, target.size_bytes);
    if (first || second) {
        if (first && first == second && *first >= minimum_hds_size && *first <= maximum_hds_size &&
            std::equal(primary.begin(), primary.end(), backup.begin())) {
            auto bounded = std::make_shared<Source>(connection, target, *first);
            const auto opened = open_image(bounded, {});
            if (connection->invalidated)
                return std::unexpected(Error{"device_read_failed", "Device changed while validating SFS prefix"});
            if (!opened && opened.error().category == ErrorCategory::io)
                return std::unexpected(Error{"device_read_failed", opened.error().message});
            if (opened && valid_sfs_envelope(*opened, *first))
                return bounded;
        }
        for (auto &selected : connection->device.targets) {
            if (selected.id == target.id) {
                selected.read_only = true;
                selected.notices.push_back("Read-only: the smaller SFS declaration is damaged or inconsistent.");
            }
        }
    }
    if (target.size_bytes > maximum_sampler_device_bytes)
        return std::unexpected(
            Error{"device_prefix_invalid", "Large devices require a validated SFS extent up to 8 GiB"});
    return full;
}
} // namespace axk::app::device_detail
