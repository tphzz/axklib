#pragma once

#include <nlohmann/json.hpp>

#include "axklib/application/contracts.hpp"
#include "axklib/application/image_sessions.hpp"

namespace axk::app {

[[nodiscard]] Result<ImageSourceRef> image_source_ref_from_json(const nlohmann::json &reference);
[[nodiscard]] nlohmann::json image_source_ref_json(const ImageSourceRef &source);
[[nodiscard]] ImageDocumentRef image_document_ref(const nlohmann::json &request);
[[nodiscard]] Result<ImageOpenSources> image_open_sources_from_json(const nlohmann::json &request);
[[nodiscard]] nlohmann::json image_document_summary_json(const ImageDocumentSummary &summary);
[[nodiscard]] nlohmann::json image_session_summary_json(const ImageSessionSummary &summary);

} // namespace axk::app
