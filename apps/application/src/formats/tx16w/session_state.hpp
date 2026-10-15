#pragma once

#include "axklib/formats/tx16w/document.hpp"
#include "native_document_state.hpp"

namespace axk::app::formats::tx16w {

class SessionState final : public NativeDocumentState, public NativeAudioProvider {
  public:
    explicit SessionState(std::shared_ptr<const axk::tx16w::Document> document);
    std::string_view format_id() const noexcept override { return "yamaha.tx16w"; }
    std::string_view profile() const noexcept override { return "yamaha-native"; }
    std::vector<std::string> available_operations(bool, bool) const override { return {"auditions.prepare"}; }
    Result<nlohmann::ordered_json> object_detail(const ImageDocumentContext &context,
                                                 std::string_view object_id) const override;
    NativeFilesystemBindings filesystem_bindings() const override { return {}; }
    Result<void> preserve_identity(const NativeDocumentState &previous) override;
    const NativeAudioProvider *audio() const noexcept override { return this; }
    Result<native::PcmSource> prepare_source(std::string_view object_id, native::PcmReadWindow window) const override;
    Result<std::vector<std::byte>> read_member_pcm(const native::PcmMember &member, std::uint64_t first_frame,
                                                   std::size_t frame_count,
                                                   const CancellationToken &cancellation) const override;

  private:
    std::shared_ptr<const axk::tx16w::Document> document_;
};

} // namespace axk::app::formats::tx16w
