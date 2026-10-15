#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "axklib/application/image_sessions.hpp"
#include "axklib/native/document.hpp"
#include "native_audio.hpp"
#include "native_filesystem_bindings.hpp"

namespace axk::app {

// Transport projections are retained with their native owner, not with source leases.
struct NativeDocumentState {
    virtual ~NativeDocumentState() = default;
    [[nodiscard]] virtual std::string_view format_id() const noexcept = 0;
    [[nodiscard]] virtual std::string_view profile() const noexcept = 0;
    [[nodiscard]] virtual std::vector<std::string> available_operations(bool source_writable,
                                                                        bool device_writable) const = 0;
    [[nodiscard]] virtual Result<nlohmann::ordered_json> object_detail(const ImageDocumentContext &session,
                                                                       std::string_view object_id) const = 0;
    [[nodiscard]] virtual NativeFilesystemBindings filesystem_bindings() const = 0;
    [[nodiscard]] virtual Result<void> preserve_identity(const NativeDocumentState &previous) = 0;
    virtual void clear_transient_state() noexcept { auditions.clear(); }
    [[nodiscard]] virtual const NativeAudioProvider *audio() const noexcept { return nullptr; }
    std::unordered_map<std::string, AuditionEntry> auditions;

    std::shared_ptr<const native::Document> native_document;
    std::unordered_map<std::string, std::string> native_object_ids;

    std::vector<ImageContentItem> content;
    std::unordered_map<std::string, std::vector<std::size_t>> content_children;
    std::vector<ImageObjectItem> objects;
    std::unordered_map<std::string, std::size_t> object_indices_by_id;
    std::unordered_map<std::string, std::vector<std::size_t>> object_indices_by_type;
    std::unordered_map<std::string, std::vector<std::size_t>> object_indices_by_content_scope;
    std::vector<ImageRelationshipItem> relationships;
    std::vector<ImageValidationItem> validation;
    std::size_t root_count{};
};

} // namespace axk::app
