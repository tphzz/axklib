#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "axklib/application/contracts.hpp"

namespace axk::app {

struct ImageValidationSummary {
    std::size_t info_count{};
    std::size_t warning_count{};
    std::size_t error_count{};

    [[nodiscard]] bool valid() const noexcept { return error_count == 0U; }
};

enum class ImageFloppySetStatus : std::uint8_t { single, incomplete, complete, recovery };

struct ImageFloppySetMember {
    std::uint16_t index{};
    std::string label;
    std::string marker;
};

struct ImageFloppySetSummary {
    ImageFloppySetStatus status{ImageFloppySetStatus::single};
    std::string set_label;
    std::vector<ImageFloppySetMember> members;
    std::optional<std::uint16_t> next_required_index;
};

struct ImageDocumentRef {
    std::string image_id;
    std::string document_id;
};

struct ImageSourceTarget {
    std::string image_id;
    std::string member_id;
};

struct ImageSourceSummary {
    std::string id;
    ImageSourceRef source;
    std::string format;
    std::vector<std::string> filesystem_root_ids;
    struct Diagnostic {
        std::string code;
        std::string severity;
        std::string message;
        std::string path;
    };
    std::vector<Diagnostic> diagnostics{};
};

struct ImageDocumentSummary {
    std::string id;
    std::string format_id;
    std::string profile;
    std::string name;
    std::string completeness;
    std::string format;
    std::optional<ImageFloppySetSummary> floppy_set;
    std::vector<ImageSourceRef> companion_sources;
    ImageValidationSummary validation;
    std::vector<std::string> source_member_ids;
    std::vector<std::string> available_operations;
    std::size_t root_count{};
    std::size_t object_count{};
    std::size_t relationship_count{};
};

struct ImageDiscoveryCandidate {
    std::string format_id;
    std::string state;
    std::vector<std::string> source_member_ids;
    std::vector<std::string> diagnostics;
};

struct ImageDocumentDiscovery {
    bool selection_required{};
    std::vector<ImageDiscoveryCandidate> candidates;
};

struct ImageOpenSources {
    std::vector<ImageSourceRef> sources;
    std::vector<std::string> selected_formats;
};

} // namespace axk::app
