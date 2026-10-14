#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/application/image_documents.hpp"

#include "image_sources.hpp"
#include "native_document_state.hpp"

namespace axk::app {

struct NativeDocumentAdmission {
    std::string id;
    std::vector<std::string> member_ids;
    std::shared_ptr<ImageSourceMember> primary;
    std::vector<ImageSourceRef> companion_sources;
    std::optional<ImageFloppySetSummary> floppy_set;
    std::string format;
    std::shared_ptr<const RandomAccessReader> snapshot;
    std::shared_ptr<const MediaContainer> media;
    std::unique_ptr<NativeDocumentState> native;
    ImageCompanionSummary companions;
    std::optional<Error> companion_attachment_error;
    std::function<Result<std::vector<ImageSourceRef>>(const Sandbox &, std::string_view, const CancellationToken &)>
        find_companions;
};

} // namespace axk::app
