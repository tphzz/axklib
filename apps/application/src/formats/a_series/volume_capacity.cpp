#include "axklib/application/formats/a_series/session_read.hpp"
#include "formats/a_series/session_state.hpp"
#include "image_sessions_internal.hpp"

#include "axklib/volume_capacity.hpp"

axk::app::Result<axk::VolumeCapacityReport>
axk::app::ImageSessionManager::volume_capacity(const ImageDocumentRef &target, std::string_view owner_id,
                                               std::uint64_t expected_revision, std::string_view content_scope_id,
                                               const CancellationToken &cancellation) {
    const auto session = implementation_->owned(target.image_id, owner_id);
    if (!session)
        return std::unexpected{session.error()};
    const auto read = axk::app::formats::a_series::SessionReader::acquire(*this, target, owner_id, expected_revision);
    if (!read)
        return std::unexpected{read.error()};

    auto selected = Implementation::document(**session, target.document_id);
    if (!selected)
        return std::unexpected(selected.error());
    if ((*selected)->native->format_id() != "yamaha.a-series")
        return std::unexpected(
            session_error("native_format_unsupported", "This operation requires an A-series document"));
    const auto scope = read->volume_scopes_by_id.find(std::string{content_scope_id});
    const auto *container = std::get_if<Container>(&read->media->storage());
    if (!container || scope == read->volume_scopes_by_id.end())
        return std::unexpected{
            session_error("volume_capacity_unsupported", "Choose an A-Series SFS volume for capacity inspection")};
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{core_error(check.error(), read->source)};
    const auto key =
        (static_cast<std::uint64_t>(scope->second.partition_index) << 32U) | scope->second.volume_directory_id;
    auto &cache = axk::app::formats::a_series::state(*(*selected)->native).volume_capacity_cache;
    if (const auto found = cache.find(key); found != cache.end() && found->second.first == expected_revision)
        return found->second.second;
    const auto result = inspect_volume_capacity(*container, PartitionIndex{scope->second.partition_index},
                                                SfsId{scope->second.volume_directory_id}, cancellation);
    if (!result)
        return std::unexpected{core_error(result.error(), read->source)};
    if (const auto unchanged = read->verify_source_unchanged(); !unchanged)
        return std::unexpected{unchanged.error()};
    cache.insert_or_assign(key, std::pair{expected_revision, *result});
    return *result;
}
