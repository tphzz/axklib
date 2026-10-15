#include "axklib/formats/tx16w/document.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "axklib/formats/tx16w/dependencies.hpp"
#include "axklib/formats/tx16w/relationships.hpp"

namespace axk::tx16w {
namespace {

constexpr std::size_t maximum_native_files = 1024U;
constexpr std::size_t maximum_objects = 16384U;
constexpr std::size_t maximum_discovery_entries = 16384U;
constexpr std::string_view format_id = "yamaha.tx16w";

bool within_discovery_limit(const native::SourceSet &sources) {
    std::size_t entries{};
    for (const auto &member : sources.members())
        for (const auto &root : member.roots) {
            if (root.entries.size() > maximum_discovery_entries - entries)
                return false;
            entries += root.entries.size();
        }
    return true;
}

bool native_magic(std::span<const std::byte> bytes) {
    constexpr std::array magic{std::byte{'L'}, std::byte{'M'}, std::byte{'8'},
                               std::byte{'9'}, std::byte{'5'}, std::byte{'3'}};
    return bytes.size() >= magic.size() && std::ranges::equal(bytes.first(magic.size()), magic);
}

bool native_kind(char kind) { return std::string_view{"WSVUFPOCRX"}.find(kind) != std::string_view::npos; }

bool unsupported_magic(std::span<const std::byte> bytes) {
    const auto matches = [&](std::size_t offset, std::string_view value) {
        return bytes.size() >= offset + value.size() &&
               std::ranges::equal(bytes.subspan(offset, value.size()), value,
                                  [](std::byte left, char right) { return left == static_cast<std::byte>(right); });
    };
    return matches(0U, "FORM") &&
           (matches(8U, "AIFC") || matches(8U, "TYPP") || matches(8U, "TYPS") || matches(8U, "TYPO"));
}

std::string occurrence_id(const native::SourceLocation &location) {
    std::string id{"tx16w:"};
    for (const auto *component : {&location.member_id, &location.root_id, &location.entry_id})
        id += std::to_string(component->size()) + ":" + *component + ":";
    return id + std::to_string(location.offset);
}

Result<std::vector<std::byte>> prefix(const native::SourceSet &sources, native::SourceLocation location,
                                      std::uint64_t size, const CancellationToken &cancellation) {
    location.length = std::min(location.length, size);
    return sources.read(location, cancellation);
}

native::SourceLocation record_location(native::SourceLocation file, std::uint64_t offset, std::uint64_t length) {
    file.offset = offset;
    file.length = length;
    return file;
}

Error document_error(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, std::move(message));
}

} // namespace

Document::Document(std::shared_ptr<const native::SourceSet> sources)
    : summary_{"tx16w",
               std::string{format_id},
               "yamaha-native",
               "TX16W",
               native::Completeness::complete,
               {{"browse", true, {}}, {"edit", false, "Native TX16W writing is not supported"}}},
      sources_(std::move(sources)) {}

Result<std::shared_ptr<const Document>> Document::open(std::shared_ptr<const native::SourceSet> sources,
                                                       const CancellationToken &cancellation,
                                                       std::span<const std::string> member_ids,
                                                       std::span<const std::string> ambiguous_requirements) {
    if (!sources)
        return std::unexpected(document_error("TX16W opening requires admitted sources"));
    if (!within_discovery_limit(*sources))
        return std::unexpected(document_error("TX16W discovery exceeds its 16,384-entry limit"));
    for (const auto &id : member_ids)
        if (std::ranges::find(sources->members(), id, &native::SourceMember::id) == sources->members().end())
            return std::unexpected(document_error("TX16W document scope contains an unknown source member"));
    if (const auto verified = sources->verify(cancellation); !verified)
        return std::unexpected(verified.error());
    auto result = std::shared_ptr<Document>{new Document{std::move(sources)}};
    result->catalog_.nodes.push_back({"tx16w", std::nullopt, std::nullopt, "document", "TX16W"});
    std::size_t native_files{};
    auto add = [&](const native::SourceLocation &location, const FileIdentity &file, ObjectValue value,
                   std::string type, std::string name) {
        const auto id = occurrence_id(location);
        result->catalog_.objects.push_back(
            {id,
             std::move(type),
             name,
             {location},
             {{"inspect", true, {}}, {"edit", false, "Native TX16W writing is not supported"}}});
        result->catalog_.nodes.push_back({id, "tx16w", id, "object", std::move(name)});
        result->objects_.push_back({id, location, file, std::move(value)});
    };
    auto malformed = [&](const native::SourceLocation &location, const FileIdentity &identity, const std::string &path,
                         const Error &error) {
        add(location, identity, OpaqueFile{identity.kind}, "native-file", path);
        result->catalog_.diagnostics.push_back(
            {"tx16w.invalid-record", native::Severity::error, error.message, occurrence_id(location)});
        result->summary_.completeness = native::Completeness::invalid;
    };
    for (const auto &member : result->sources_->members()) {
        if (!member_ids.empty() && std::ranges::find(member_ids, member.id) == member_ids.end())
            continue;
        for (const auto &root : member.roots) {
            for (const auto &entry : root.entries) {
                if (const auto active = cancellation.check(); !active)
                    return std::unexpected(active.error());
                const auto identity = identify_file(entry.path);
                if (entry.directory || !identity || !native_kind(identity->kind))
                    continue;
                const auto location = result->sources_->locate(member.id, root.id, entry.id);
                if (!location)
                    return std::unexpected(location.error());
                const auto header = prefix(*result->sources_, *location, 32U, cancellation);
                if (!header)
                    return std::unexpected(header.error());
                const bool unsupported = !native_magic(*header) && unsupported_magic(*header);
                result->files_.push_back({*location, *identity, false, unsupported});
                if (!native_magic(*header) && !unsupported)
                    continue;
                if (++native_files > maximum_native_files || result->objects_.size() > maximum_objects - 96U)
                    return std::unexpected(document_error("TX16W document exceeds its native catalog limits"));
                if (unsupported) {
                    add(*location, *identity, OpaqueFile{identity->kind}, "native-file", entry.path);
                    result->catalog_.diagnostics.push_back(
                        {"tx16w.unsupported-payload", native::Severity::warning,
                         "Typhoon/AIFC content is present but is not decoded by the Yamaha-native adapter",
                         occurrence_id(*location)});
                    continue;
                }
                if (identity->kind == 'W') {
                    auto wave = decode_wave_header(*header, location->length, entry.path);
                    if (!wave) {
                        malformed(*location, *identity, entry.path, wave.error());
                        continue;
                    }
                    if (wave->sample_rate_source == SampleRateSource::defaulted_33333)
                        result->catalog_.diagnostics.push_back(
                            {"tx16w.unknown-sample-rate", native::Severity::warning,
                             "Wave sample rate is uncertain; 33,333 Hz is only a decoding default",
                             occurrence_id(*location)});
                    add(*location, *identity, std::move(*wave), "wave", entry.path);
                    result->files_.back().decoded = true;
                    continue;
                }
                if (identity->kind != 'S' && identity->kind != 'V' && identity->kind != 'U') {
                    add(*location, *identity, OpaqueFile{identity->kind}, "native-file", entry.path);
                    result->catalog_.diagnostics.push_back(
                        {"tx16w.unsupported-record", native::Severity::warning,
                         "Native file is preserved but its record type is not decoded", occurrence_id(*location)});
                    continue;
                }
                const auto size = identity->kind == 'S' ? 1536U : identity->kind == 'V' ? 8704U : 5120U;
                const auto bytes = prefix(*result->sources_, *location, size, cancellation);
                if (!bytes)
                    return std::unexpected(bytes.error());
                if (identity->kind == 'S') {
                    auto setup = decode_setup_file(*bytes, identity->set_name);
                    if (!setup)
                        malformed(*location, *identity, entry.path, setup.error());
                    else {
                        add(*location, *identity, std::move(*setup), "setup", identity->set_name);
                        result->files_.back().decoded = true;
                    }
                } else if (identity->kind == 'V') {
                    auto voices = decode_voice_file(*bytes, entry.path);
                    if (!voices) {
                        malformed(*location, *identity, entry.path, voices.error());
                        continue;
                    }
                    for (auto &voice : voices->voices) {
                        const auto range =
                            record_location(*location, 16U + static_cast<std::uint64_t>(voice.slot) * 0x92U, 0x92U);
                        auto name = voice.name;
                        add(range, *identity, std::move(voice), "voice", std::move(name));
                    }
                    for (auto &timbre : voices->timbres) {
                        const auto range = record_location(
                            *location, 0x1250U + static_cast<std::uint64_t>(timbre.slot) * 0x38U, 0x38U);
                        auto name = timbre.name;
                        add(range, *identity, std::move(timbre), "timbre", std::move(name));
                    }
                    result->files_.back().decoded = true;
                } else {
                    auto performances = decode_performance_file(*bytes, entry.path);
                    if (!performances) {
                        malformed(*location, *identity, entry.path, performances.error());
                        continue;
                    }
                    for (auto &performance : *performances) {
                        const auto range = record_location(
                            *location, 16U + static_cast<std::uint64_t>(performance.slot) * 0x92U, 0x92U);
                        auto name = performance.name;
                        add(range, *identity, std::move(performance), "performance", std::move(name));
                    }
                    result->files_.back().decoded = true;
                }
            }
        }
    }
    if (native_files == 0U)
        return std::unexpected(make_error(ErrorCode::container_unrecognized, ErrorCategory::object,
                                          "Sources contain no recognized TX16W native files"));
    result->summary_.id = "tx16w-" + result->files_.front().location.member_id;
    if (auto linked = detail::build_relationships(result->objects_, result->files_, result->catalog_, cancellation);
        !linked)
        return std::unexpected(linked.error());
    result->summary_.source_requirements = detail::source_requirements(result->objects_, result->files_);
    for (auto &requirement : result->summary_.source_requirements) {
        if (std::ranges::find(ambiguous_requirements, requirement.name) != ambiguous_requirements.end())
            requirement.state = native::DependencyState::ambiguous;
        if (requirement.state == native::DependencyState::invalid) {
            result->summary_.completeness = native::Completeness::invalid;
            result->catalog_.diagnostics.push_back({"tx16w.invalid-dependency", native::Severity::error,
                                                    "Required native file is present but invalid: " + requirement.name,
                                                    requirement.object_id});
        }
        if (requirement.state == native::DependencyState::ambiguous)
            result->catalog_.diagnostics.push_back({"tx16w.ambiguous-dependency", native::Severity::warning,
                                                    "Several source occurrences can supply: " + requirement.name,
                                                    requirement.object_id});
        if (requirement.state == native::DependencyState::unsupported)
            result->catalog_.diagnostics.push_back({"tx16w.unsupported-dependency", native::Severity::warning,
                                                    "Native dependency " + requirement.name +
                                                        " remains unresolved; similarly named Typhoon/AIFC content is "
                                                        "present, but equivalence cannot be verified",
                                                    requirement.object_id});
    }
    if (result->summary_.completeness != native::Completeness::invalid &&
        std::ranges::any_of(result->summary_.source_requirements, [](const auto &requirement) {
            return requirement.state == native::DependencyState::missing ||
                   requirement.state == native::DependencyState::ambiguous ||
                   requirement.state == native::DependencyState::unsupported;
        }))
        result->summary_.completeness = native::Completeness::incomplete;
    if (const auto verified = result->sources_->verify(cancellation); !verified)
        return std::unexpected(verified.error());
    return std::shared_ptr<const Document>{std::move(result)};
}

const native::DocumentSummary &Document::summary() const noexcept { return summary_; }
const native::Catalog &Document::catalog() const noexcept { return catalog_; }
const std::shared_ptr<const native::SourceSet> &Document::sources() const noexcept { return sources_; }
std::span<const NativeObject> Document::native_objects() const noexcept { return objects_; }
std::span<const NativeFile> Document::native_files() const noexcept { return files_; }

Result<const NativeObject *> Document::object(std::string_view id) const {
    const auto found = std::ranges::find(objects_, id, &NativeObject::id);
    if (found == objects_.end())
        return std::unexpected(
            make_error(ErrorCode::object_missing, ErrorCategory::object, "TX16W object does not exist"));
    return &*found;
}

Result<Wave> Document::read_wave(std::string_view id, const CancellationToken &cancellation) const {
    auto found = object(id);
    if (!found)
        return std::unexpected(found.error());
    const auto *header = std::get_if<WaveHeader>(&(*found)->value);
    if (!header)
        return std::unexpected(document_error("Native object is not a TX16W Wave"));
    auto location = (*found)->location;
    const auto frames = static_cast<std::uint64_t>(header->attack_frames) + header->repeat_frames;
    location.length = 32U + ((frames + 1U) / 2U) * 3U;
    auto bytes = sources_->read(location, cancellation);
    if (!bytes)
        return std::unexpected(bytes.error());
    auto wave = decode_wave(*bytes, header->name);
    if (!wave)
        return std::unexpected(wave.error());
    if (static_cast<const WaveHeader &>(*wave) != *header)
        return std::unexpected(make_error(ErrorCode::transaction_stale, ErrorCategory::object,
                                          "TX16W Wave metadata changed after the native document was opened"));
    wave->source_member = location.member_id;
    return wave;
}

Result<std::vector<std::int16_t>> Document::read_wave_frames(std::string_view id, std::uint64_t first_frame,
                                                             std::size_t frame_count,
                                                             const CancellationToken &cancellation) const {
    const auto found = object(id);
    if (!found)
        return std::unexpected(found.error());
    const auto *header = std::get_if<WaveHeader>(&(*found)->value);
    if (!header)
        return std::unexpected(document_error("Native object is not a TX16W Wave"));
    const auto frames = static_cast<std::uint64_t>(header->attack_frames) + header->repeat_frames;
    if (first_frame > frames || frame_count > frames - first_frame)
        return std::unexpected(document_error("TX16W PCM range exceeds the declared Wave"));
    auto bytes = prefix(*sources_, (*found)->location, 32U, cancellation);
    if (!bytes)
        return std::unexpected(bytes.error());
    const auto current = decode_wave_header(*bytes, (*found)->location.length, header->name);
    if (!current || *current != *header)
        return std::unexpected(make_error(ErrorCode::transaction_stale, ErrorCategory::object,
                                          "TX16W Wave metadata changed after opening"));
    if (frame_count == 0U)
        return std::vector<std::int16_t>{};
    auto location = (*found)->location;
    const auto skip = static_cast<std::size_t>(first_frame % 2U);
    location.offset += 32U + (first_frame / 2U) * 3U;
    location.length = ((static_cast<std::uint64_t>(frame_count) + skip + 1U) / 2U) * 3U;
    bytes = sources_->read(location, cancellation);
    if (!bytes)
        return std::unexpected(bytes.error());
    return decode_packed_pcm(*bytes, skip, frame_count);
}

native::FormatProvider provider() {
    return {{std::string{format_id}, "Yamaha TX16W", {"yamaha-native"}},
            [](const native::SourceSet &sources, const CancellationToken &cancellation) -> Result<native::Recognition> {
                native::Recognition result{std::string{format_id}, native::RecognitionState::no_match, {}};
                if (const auto active = cancellation.check(); !active)
                    return std::unexpected(active.error());
                if (!within_discovery_limit(sources)) {
                    result.state = native::RecognitionState::limit_exceeded;
                    result.diagnostics.push_back({"tx16w.discovery-limit", native::Severity::warning,
                                                  "TX16W discovery exceeds its 16,384-entry limit", std::nullopt});
                    return result;
                }
                for (const auto &member : sources.members())
                    for (const auto &root : member.roots)
                        for (const auto &entry : root.entries) {
                            if (const auto active = cancellation.check(); !active)
                                return std::unexpected(active.error());
                            const auto identity = identify_file(entry.path);
                            if (entry.directory || !identity || !native_kind(identity->kind))
                                continue;
                            const auto location = sources.locate(member.id, root.id, entry.id);
                            if (!location)
                                return std::unexpected(location.error());
                            const auto bytes = prefix(sources, *location, 6U, cancellation);
                            if (!bytes)
                                return std::unexpected(bytes.error());
                            if (native_magic(*bytes)) {
                                result.state = native::RecognitionState::supported;
                                result.claims.push_back(*location);
                            }
                        }
                return result;
            },
            [](auto sources, const auto &cancellation) -> Result<std::vector<std::shared_ptr<const native::Document>>> {
                return detail::open_documents(std::move(sources), cancellation);
            }};
}

} // namespace axk::tx16w
