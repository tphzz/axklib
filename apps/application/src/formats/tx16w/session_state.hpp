#pragma once

#include "axklib/formats/tx16w/document.hpp"
#include "native_document_state.hpp"

namespace axk::app::formats::tx16w {

class SessionState final : public NativeDocumentState {
  public:
    explicit SessionState(std::shared_ptr<const axk::tx16w::Document> document);
    std::string_view format_id() const noexcept override { return "yamaha.tx16w"; }
    std::string_view profile() const noexcept override { return "yamaha-native"; }
    std::vector<std::string> available_operations(bool, bool) const override { return {}; }
    Result<nlohmann::ordered_json> object_detail(const ImageDocumentContext &context,
                                                 std::string_view object_id) const override;
    NativeFilesystemBindings filesystem_bindings() const override { return {}; }
    Result<void> preserve_identity(const NativeDocumentState &previous) override;
    void clear_transient_state() noexcept override {}

  private:
    std::shared_ptr<const axk::tx16w::Document> document_;
};

} // namespace axk::app::formats::tx16w
