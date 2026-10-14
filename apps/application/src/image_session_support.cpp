#include "image_sessions_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "axklib/application/secure_random.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/catalog.hpp"
#include "axklib/lookups.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"
#include "axklib/utf8.hpp"

#include "content_digest.hpp"

namespace axk::app::image_sessions_internal {

axk::app::Error session_error(std::string code, std::string message, bool retryable) {
    return {std::move(code), std::move(message), {}, retryable};
}

axk::app::Error core_error(const axk::Error &error, const axk::app::ImageSourceRef &source) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    context.relative_path = source.relative_path;
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled"
            : error.code == axk::ErrorCode::transaction_stale ? "image_source_changed"
                                                              : "image_open_failed",
            error.message, std::move(context), false};
}

axk::app::Result<std::string> random_identifier(std::string_view prefix) {
    auto suffix = axk::app::secure_random_hex(16U);
    if (!suffix)
        return std::unexpected(suffix.error());
    return std::string{prefix} + std::move(*suffix);
}

std::string media_kind_name(axk::MediaKind kind) {
    switch (kind) {
    case axk::MediaKind::sfs:
        return "sfs";
    case axk::MediaKind::fat12_floppy:
        return "fat12";
    case axk::MediaKind::ex5_disk:
        return "ex5-disk";
    case axk::MediaKind::fat16_disk:
        return "fat16-disk";
    case axk::MediaKind::fat12_floppy_set:
        return "fat12-set";
    case axk::MediaKind::iso9660:
        return "iso9660";
    case axk::MediaKind::standalone_object:
        return "standalone-object";
    case axk::MediaKind::axk_object_directory:
        return "axk-object-directory";
    case axk::MediaKind::a3k_archive:
        return "a3k-archive";
    }
    return "unknown";
}

std::string fold_ascii(std::string_view value) {
    std::string folded;
    folded.reserve(value.size());
    std::ranges::transform(value, std::back_inserter(folded), [](char character) {
        return character >= 'a' && character <= 'z' ? static_cast<char>(character - ('a' - 'A')) : character;
    });
    return folded;
}

axk::app::Result<std::vector<axk::app::DirectoryEntry>> list_bounded_directory(const axk::app::Sandbox &sandbox,
                                                                               const axk::app::DirectoryRef &reference,
                                                                               std::size_t maximum_entries) {
    std::vector<axk::app::DirectoryEntry> entries;
    std::optional<std::string> cursor;
    do {
        const auto remaining = maximum_entries + 1U - entries.size();
        const auto page_size = std::min<std::size_t>(remaining, 1000U);
        auto page = sandbox.list_directory(reference, page_size, cursor);
        if (!page)
            return std::unexpected(page.error());
        entries.insert(entries.end(), std::make_move_iterator(page->entries.begin()),
                       std::make_move_iterator(page->entries.end()));
        if (entries.size() > maximum_entries) {
            return std::unexpected(
                session_error("image_open_failed", "AXK object directory companion scan exceeds its entry limit"));
        }
        cursor = std::move(page->next_cursor);
    } while (cursor);
    return entries;
}

} // namespace axk::app::image_sessions_internal
