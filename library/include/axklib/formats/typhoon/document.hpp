#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/formats/typhoon/wave.hpp"
#include "axklib/native/registry.hpp"

namespace axk::typhoon {
struct Reference {
    std::string type, name, disk;
    std::uint32_t version{};
    std::optional<std::string> target{};
    bool ambiguous{};
};
enum class TriggerMode : std::int8_t { normal = -2, one_shot = -1, glide = 0, release = 1 };
struct Region {
    std::size_t reference{};
    std::uint8_t midi_low{}, midi_high{}, velocity_low{}, velocity_high{}, raw_key_scaling{};
    TriggerMode trigger{TriggerMode::normal};
    std::int8_t coarse_semitones{}, fine_steps{}; // Group pitch; 1024 fine steps per octave.
    std::uint8_t raw_group_volume{};              // 0 is loudest.
};
struct Part {
    std::size_t reference{};
    std::uint8_t channel{};
    std::int8_t transpose{};
    std::uint16_t volume{};
};
struct Item {
    native::SourceLocation location;
    std::string id, type, name, disk;
    std::uint32_t version{};
    std::optional<Wave> wave;
    std::vector<Reference> references;
    std::vector<Region> regions{};
    std::vector<Part> parts{};
    std::string mapping_error{};
    bool has_split_table{};
    std::string occurrence{};
};
class Document final : public native::Document {
  public:
    [[nodiscard]] static Result<std::shared_ptr<const Document>> open(std::shared_ptr<const native::SourceSet> sources,
                                                                      const CancellationToken &token = {},
                                                                      std::span<const std::string> member_ids = {});
    const native::DocumentSummary &summary() const noexcept override { return summary_; }
    const native::Catalog &catalog() const noexcept override { return catalog_; }
    const std::shared_ptr<const native::SourceSet> &sources() const noexcept override { return sources_; }
    [[nodiscard]] const Item *find(std::string_view id) const;
    [[nodiscard]] Result<std::vector<std::int16_t>> pcm(std::string_view id, std::uint32_t first, std::uint32_t count,
                                                        const CancellationToken &token = {}) const;

  private:
    native::DocumentSummary summary_;
    native::Catalog catalog_;
    std::shared_ptr<const native::SourceSet> sources_;
    std::map<std::string, Item, std::less<>> items_;
};
[[nodiscard]] native::FormatProvider provider();
// A native item needs both its DOS type and the matching IFF identification.
[[nodiscard]] std::string_view item_kind(std::string_view filename, std::span<const std::byte> prefix);
} // namespace axk::typhoon
