#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "axklib/application/contracts.hpp"
#include "axklib/application/filesystem.hpp"
#include "axklib/export.hpp"
#include "axklib/io.hpp"

namespace axk::app {

struct ImageValidationSummary {
    std::size_t info_count{};
    std::size_t warning_count{};
    std::size_t error_count{};

    [[nodiscard]] bool valid() const noexcept { return error_count == 0U; }
};

struct ImageSessionSummary {
    std::string image_id;
    FileRef source;
    std::string format;
    std::vector<std::string> available_operations;
    std::size_t root_count{};
    std::size_t object_count{};
    std::size_t relationship_count{};
    ImageValidationSummary validation;
};

struct ImageContentItem {
    std::string id;
    std::optional<std::string> parent_id;
    std::size_t depth{};
    std::string kind;
    std::string display_name;
    std::size_t child_count{};
    std::optional<std::string> object_id;
    std::optional<std::string> object_type;
    std::string quality;
    std::string basis;
    std::string notes;
    std::vector<std::string> details;
};

struct WaveformMetadata {
    std::uint16_t sample_rate{};
    std::uint16_t sample_width_bytes{};
    std::uint8_t root_key{};
    std::int8_t fine_tune_cents{};
    std::uint8_t loop_mode{};
    std::string loop_mode_label;
    std::uint32_t frame_count{};
    std::uint32_t loop_start_frame{};
    std::uint32_t loop_length_frames{};
};

struct ImageObjectItem {
    std::string id;
    std::string type;
    std::string name;
    std::string format;
    std::optional<std::uint8_t> partition_index;
    std::string partition_name;
    std::string volume_name;
    std::string category_name;
    std::string entry_name;
    std::optional<WaveformMetadata> waveform;
};

struct ImageRelationshipItem {
    std::string id;
    std::string source_object_id;
    std::optional<std::string> target_object_id;
    std::vector<std::string> candidate_object_ids;
    std::string type;
    std::string quality;
    std::string basis;
    std::string notes;
    std::optional<std::size_t> assignment_index;
    std::string assignment_name;
    std::string assignment_state;
};

struct ImageValidationItem {
    std::string code;
    std::string severity;
    std::string message;
    std::string sampler_path;
    std::optional<std::string> object_id;
};

struct ImagePreviewBin {
    std::int32_t minimum{};
    std::int32_t maximum{};
};

struct ImageWaveformPreview {
    std::string object_id;
    std::uint64_t frame_count{};
    std::vector<ImagePreviewBin> bins;
};

template <typename Item> struct ImagePage {
    std::vector<Item> items;
    std::size_t total_count{};
    std::optional<std::string> next_cursor;
};

class ImageSessionManager {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    ImageSessionManager(const Sandbox &sandbox, std::size_t maximum_sessions = 32U,
                        std::size_t maximum_page_size = 500U,
                        std::chrono::seconds idle_retention = std::chrono::minutes{15},
                        Clock clock = std::chrono::steady_clock::now);
    ~ImageSessionManager();
    ImageSessionManager(ImageSessionManager &&) noexcept;
    ImageSessionManager &operator=(ImageSessionManager &&) noexcept;
    ImageSessionManager(const ImageSessionManager &) = delete;
    ImageSessionManager &operator=(const ImageSessionManager &) = delete;

    [[nodiscard]] Result<ImageSessionSummary> open(const FileRef &source, std::string owner_id,
                                                   const CancellationToken &cancellation = {});
    [[nodiscard]] Result<ImageSessionSummary> inspect(std::string_view image_id, std::string_view owner_id);
    [[nodiscard]] Result<void> close(std::string_view image_id, std::string_view owner_id);
    [[nodiscard]] Result<ImagePage<ImageContentItem>> content(std::string_view image_id, std::string_view owner_id,
                                                              std::size_t limit,
                                                              std::optional<std::string_view> cursor = std::nullopt,
                                                              std::optional<std::string_view> parent_id = std::nullopt);
    [[nodiscard]] Result<ImagePage<ImageObjectItem>>
    objects(std::string_view image_id, std::string_view owner_id, std::size_t limit,
            std::optional<std::string_view> cursor = std::nullopt,
            std::optional<std::string_view> object_type = std::nullopt);
    [[nodiscard]] Result<ImagePage<ImageRelationshipItem>>
    relationships(std::string_view image_id, std::string_view owner_id, std::size_t limit,
                  std::optional<std::string_view> cursor = std::nullopt);
    [[nodiscard]] Result<ImagePage<ImageValidationItem>>
    validation_issues(std::string_view image_id, std::string_view owner_id, std::size_t limit,
                      std::optional<std::string_view> cursor = std::nullopt);
    [[nodiscard]] Result<ImageWaveformPreview> preview(std::string_view image_id, std::string_view owner_id,
                                                       std::string_view object_id, std::size_t bin_count,
                                                       const CancellationToken &cancellation = {});
    void cleanup();
    [[nodiscard]] bool root_in_use(std::string_view root_id);

  private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace axk::app
