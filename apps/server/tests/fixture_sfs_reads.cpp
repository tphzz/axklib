#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/conversion/a_series/wave_reader.hpp"
#include "axklib/sfs_disk_sources.hpp"

namespace {
class Meter final : public axk::RandomAccessReader {
  public:
    explicit Meter(std::shared_ptr<const axk::RandomAccessReader> reader) : reader_(std::move(reader)) {}
    std::uint64_t size() const noexcept override { return reader_->size(); }
    axk::Result<void> read_exact_at(std::uint64_t pos, std::span<std::byte> out) const override {
        reads.emplace_back(pos, out.size());
        return reader_->read_exact_at(pos, out);
    }
    mutable std::vector<std::pair<std::uint64_t, std::size_t>> reads;

  private:
    std::shared_ptr<const axk::RandomAccessReader> reader_;
};
} // namespace

int measure_sfs_window(const std::filesystem::path &path) {
    const auto disk = axk::FileReader::open(path);
    if (!disk)
        return 1;
    const auto meter = std::make_shared<Meter>(*disk);
    axk::native::SourceMember member{"source-1", "source.hds", meter, {}};
    axk::inspection::SfsDiskSources sfs;
    if (!sfs.admit(member, path, "measured", {}))
        return 1;
    const auto sources = axk::native::SourceSet::create({std::move(member)});
    axk::native::FormatRegistry registry;
    if (!sources || !registry.add(std::move(sfs).provider()))
        return 1;
    const auto opened = registry.open(*sources);
    if (!opened || opened->documents.size() != 1U)
        return 1;
    const auto doc = opened->documents.front();
    const auto &objects = doc->catalog().objects;
    const auto wanted =
        std::ranges::find_if(objects, [](const auto &row) { return row.volume_name == "TWO" && row.name == "TONE"; });
    if (wanted == objects.end())
        return 1;
    nlohmann::json result{{"metadataReads", meter->reads}};
    meter->reads.clear();
    axk::conversion::Request request{doc, {}, {}, axk::conversion::Fidelity::best, {}, {}};
    const std::vector<std::string> invalid{wanted->id, "missing"};
    if (axk::conversion::a_series_waves::select_waves(request, doc->summary().id, invalid))
        return 1;
    const auto waves =
        axk::conversion::a_series_waves::select_waves(request, doc->summary().id, std::span{&wanted->id, 1U});
    if (!waves || !meter->reads.empty())
        return 1;
    const auto pcm = waves->front().read(510U, 3U);
    if (!pcm)
        return 1;
    result["selectedReads"] = meter->reads;
    result["frames"] = *pcm;
    std::cout << result.dump() << '\n';
    return 0;
}
