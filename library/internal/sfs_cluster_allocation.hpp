#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace axk::detail {

template <typename IsUsed>
std::optional<std::vector<std::uint32_t>> select_sfs_payload_clusters(std::uint32_t first, std::uint32_t end,
                                                                      std::uint32_t count, IsUsed &&is_used) {
    if (count == 0U)
        return std::vector<std::uint32_t>{};

    std::uint32_t run_begin{};
    std::uint32_t run_length{};
    for (std::uint32_t cluster = first; cluster < end; ++cluster) {
        if (std::invoke(is_used, cluster)) {
            run_length = 0U;
            continue;
        }
        if (run_length == 0U)
            run_begin = cluster;
        ++run_length;
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
    for (std::uint32_t cluster = first; cluster < end && selected.size() < count; ++cluster) {
        if (!std::invoke(is_used, cluster))
            selected.push_back(cluster);
    }
    if (selected.size() != count)
        return std::nullopt;
    return selected;
}

} // namespace axk::detail
