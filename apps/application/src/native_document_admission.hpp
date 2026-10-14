#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

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
};

} // namespace axk::app
