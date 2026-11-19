#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/native/document.hpp"
#include "axklib/volume_capacity.hpp"

namespace axk::conversion {

enum class Fidelity : std::uint8_t { best, strict };
enum class Impact : std::uint8_t { exact, representation, loss, uncertainty, blocker };

struct Issue {
    std::string id;
    std::string code;
    Impact impact{Impact::exact};
    std::string source_object;
    std::string source_parameter;
    std::string target_object;
    std::string target_parameter;
    std::string message;
    std::string decision{};
};

struct SourceObject {
    std::string id, name, kind, location;
    bool selectable{};
    std::vector<std::string> dependencies{};
};
void to_json(nlohmann::json &json, const SourceObject &object);

struct Action {
    std::string kind;
    std::string name;
    std::string source_object;
    std::string action{"create"};
    struct Detail {
        std::string label;
        std::string value;
    };
    std::vector<Detail> details{};
};

struct NewImageReview {
    std::uint64_t size_bytes{};
};

struct ImageMemberReview {
    std::string filename;
    std::uint64_t size_bytes{};
    std::string sha256;
};
struct NewImageSetReview {
    std::vector<ImageMemberReview> members;
};
void to_json(nlohmann::json &json, const NewImageSetReview &review);

struct Report {
    std::string profile_id;
    Fidelity fidelity{Fidelity::best};
    bool executable{};
    std::vector<Action> actions;
    std::vector<Issue> issues;
    std::variant<VolumeCapacityAdmission, NewImageReview, NewImageSetReview> destination;
    std::vector<SourceObject> source_objects{};
};

[[nodiscard]] std::uint64_t report_bytes(const Report &report);

struct Patch {
    std::uint64_t offset{};
    std::vector<std::byte> original;
    std::vector<std::byte> replacement;
};

enum class OutputKind : std::uint8_t { existing_image, new_image, new_image_set };
[[nodiscard]] std::string_view output_kind_name(OutputKind kind) noexcept;
struct ExistingImagePatches {
    std::vector<Patch> patches;
    std::uint64_t destination_size{};
};
struct NewImage {
    std::vector<std::byte> bytes;
    std::string extension{"img"};
};
struct ImageMember {
    std::string filename;
    std::vector<std::byte> bytes;
};
struct NewImageSet {
    static constexpr std::size_t maximum_members = 64;
    std::vector<ImageMember> members;
    [[nodiscard]] Result<void> validate(std::uint64_t byte_limit) const;
};
using PreparedOutput = std::variant<ExistingImagePatches, NewImage, NewImageSet>;

struct Limits {
    std::uint64_t maximum_bytes{512ULL * 1024ULL * 1024ULL};
    std::size_t maximum_actions{32768U};
    std::size_t maximum_issues{16384U};
};

struct Destination {
    std::string format_id;
    std::shared_ptr<const RandomAccessReader> reader;
    std::filesystem::path label;
    nlohmann::json selection;
};

struct Request {
    std::shared_ptr<const native::Document> source;
    Destination destination;
    nlohmann::json options;
    Fidelity fidelity{Fidelity::best};
    std::optional<VolumeCapacityPolicy> capacity_policy;
    Limits limits;
    // Session projections may advertise IDs different from native object keys.
    std::shared_ptr<const std::unordered_map<std::string, std::string>> native_keys_by_id{};
};

class PreparedPlan final {
  public:
    PreparedPlan(Report report, std::vector<Patch> patches, std::uint64_t destination_size,
                 std::uint64_t retained_bytes);
    PreparedPlan(Report report, NewImage image, std::uint64_t retained_bytes);
    PreparedPlan(Report report, NewImageSet images);
    [[nodiscard]] const Report &report() const noexcept;
    [[nodiscard]] const PreparedOutput &output() const noexcept;
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
    [[nodiscard]] Result<void> acknowledge(std::span<const std::string> issue_ids) const;

  private:
    Report report_;
    PreparedOutput output_;
    std::uint64_t retained_bytes_{};
};

struct RouteDescriptor {
    std::string target_label, medium;
    std::vector<std::string> intents, editor_capabilities;
    int priority{};
};

struct Profile {
    std::string id;
    std::string name;
    std::string source_format;
    std::string destination_format;
    nlohmann::json options_schema;
    using Prepare = std::function<Result<std::shared_ptr<const PreparedPlan>>(
        const Request &, const CancellationToken &, ProgressSink *)>;
    Prepare prepare;
    OutputKind output_kind{OutputKind::existing_image};
    std::optional<RouteDescriptor> route{};
};

enum class WavePlayback {
    unknown,
    forward,
    forward_loop,
    forward_loop_release,
    reverse,
    forward_one_shot,
    reverse_one_shot
};

// A mono playback window. Loop and first-frame positions use source frame coordinates;
// reads are relative to the window and return signed 16-bit PCM without resampling.
struct WaveTransfer {
    std::string id, name;
    std::uint32_t rate{}, first_frame{}, frame_count{};
    WavePlayback loop_mode{WavePlayback::unknown};
    std::uint32_t loop_start{}, loop_length{};
    std::uint8_t root_key{};
    std::int8_t fine_tune{};
    using Read =
        std::function<Result<std::vector<std::int16_t>>(std::uint64_t, std::size_t, const CancellationToken &)>;
    Read read_pcm;
    [[nodiscard]] Result<std::vector<std::int16_t>> read(std::uint64_t first, std::size_t count,
                                                         const CancellationToken &cancel = {}) const;
};

// Wave adapters compose by their shared typed transfer contract. Source selection
// belongs to the reader; destination placement, fidelity and limits remain in Request.
struct WaveReaderAdapter {
    std::string source_format, name;
    nlohmann::json selection_schema;
    std::vector<std::string> editor_capabilities;
    std::function<Result<std::vector<WaveTransfer>>(const Request &, const CancellationToken &)> select;
};

struct WaveWriterAdapter {
    std::string route_key, destination_format, name;
    OutputKind output_kind;
    std::optional<RouteDescriptor> route;
    std::function<Result<std::shared_ptr<const PreparedPlan>>(
        std::string_view, std::span<const WaveTransfer>, const Request &, const CancellationToken &, ProgressSink *)>
        encode;
};

class Registry final {
  public:
    [[nodiscard]] Result<void> add(Profile profile);
    [[nodiscard]] Result<void> add(WaveReaderAdapter reader);
    [[nodiscard]] Result<void> add(WaveWriterAdapter writer);
    [[nodiscard]] std::span<const Profile> profiles() const noexcept;
    [[nodiscard]] Result<std::shared_ptr<const PreparedPlan>> prepare(std::string_view profile_id,
                                                                      const Request &request,
                                                                      const CancellationToken &cancellation = {},
                                                                      ProgressSink *progress = nullptr) const;

  private:
    std::vector<Profile> profiles_;
    std::vector<WaveReaderAdapter> wave_readers_;
    std::vector<WaveWriterAdapter> wave_writers_;
};

[[nodiscard]] std::string_view impact_name(Impact impact) noexcept;

} // namespace axk::conversion
