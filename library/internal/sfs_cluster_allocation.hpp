#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace axk::detail {

template <typename IsUsed>
std::optional<std::vector<std::uint32_t>> select_sfs_payload_clusters(std::uint32_t first, std::uint32_t end,
                                                                      std::uint32_t count, IsUsed &&is_used,
                                                                      std::uint32_t unit = 1U) {
    if (unit == 0U || count % unit != 0U)
        return std::nullopt;
    if (count == 0U)
        return std::vector<std::uint32_t>{};
    const auto aligned_first = ((static_cast<std::uint64_t>(first) + unit - 1U) / unit) * unit;
    end -= end % unit;
    if (aligned_first >= end || count > end - aligned_first)
        return std::nullopt;
    first = static_cast<std::uint32_t>(aligned_first);
    const auto unit_is_free = [&](std::uint32_t cluster) {
        for (std::uint32_t offset = 0U; offset < unit; ++offset)
            if (std::invoke(is_used, cluster + offset))
                return false;
        return true;
    };

    std::uint32_t run_begin{};
    std::uint32_t run_length{};
    for (std::uint32_t cluster = first; cluster < end; cluster += unit) {
        if (!unit_is_free(cluster)) {
            run_length = 0U;
            continue;
        }
        if (run_length == 0U)
            run_begin = cluster;
        run_length += unit;
        if (run_length == count) {
            std::vector<std::uint32_t> selected;
            selected.reserve(count);
            for (std::uint32_t value = run_begin; value < run_begin + count; ++value)
                selected.push_back(value);
            return selected;
        }
    }

    std::vector<std::uint32_t> selected;
    selected.reserve(count);
    for (std::uint32_t cluster = first; cluster < end && selected.size() < count; cluster += unit) {
        if (unit_is_free(cluster))
            for (std::uint32_t offset = 0U; offset < unit; ++offset)
                selected.push_back(cluster + offset);
    }
    if (selected.size() != count)
        return std::nullopt;
    return selected;
}

} // namespace axk::detail
