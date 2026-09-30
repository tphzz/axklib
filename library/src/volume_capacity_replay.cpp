#include "axklib/volume_capacity_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace axk::detail {
namespace {
using Identity = std::pair<ObjectType, std::array<std::byte, 16>>;

std::array<std::byte, 16> stored_name(std::string_view value) {
    std::array<std::byte, 16> result;
    result.fill(std::byte{' '});
    std::ranges::transform(value, result.begin(), [](char byte) { return static_cast<std::byte>(byte); });
    return result;
}

Result<void> covered(const CapacityVolume &volume, const VolumeCapacityProfile &profile) {
    const bool a3 = profile.target == ASeriesLoadTarget::a3000;
    const auto invalid = [&](const CapacityObject &object, std::string_view reason) -> Result<void> {
        std::string name;
        for (const auto byte : object.name)
            name += static_cast<char>(std::to_integer<unsigned char>(byte));
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "Cannot inspect " + std::string{a_series_load_target_name(profile.target)} +
                                              " capacity: " + std::string{capacity_type_name(object.type)} + "/" +
                                              name + ": " + std::string{reason})};
    };
    for (const auto &object : volume.objects) {
        if (object.type == ObjectType::sbnk &&
            (object.older_body_bytes < 308U ||
             (object.selector < 4U ? object.encoded_body_bytes < 308U || object.encoded_body_bytes > 344U
                                   : object.encoded_body_bytes < 344U)))
            return invalid(object, "Sample body is too short or cannot be safely converted during loading.");
        if (object.type != ObjectType::smpl &&
            (a3 ? object.older_body_bytes : object.encoded_body_bytes) > object.physical_body_bytes)
            return invalid(object, "The declared object body exceeds its physical extent.");
        if (object.type == ObjectType::prog && capacity_registered_name(object) == stored_name("TEMP"))
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "The sampler cannot load a Program named TEMP; rename this Program.")};
        if (object.type == ObjectType::sbac || object.type == ObjectType::prog) {
            const auto base = object.type == ObjectType::sbac ? (a3 ? 276U : 312U) : (a3 ? 232U : 408U);
            const auto stride = object.type == ObjectType::sbac ? 20U : 56U;
            auto body = a3 ? object.older_body_bytes : object.encoded_body_bytes;
            if (!a3 && object.selector < 4U)
                body += object.type == ObjectType::sbac ? 36U : 176U;
            if (body < base + 8U || (body - base - 8U) % stride != 0U ||
                object.counted_rows > (body - base - 8U) / stride)
                return invalid(object, "Counted rows exceed the declared Bank or Program body.");
        }
    }
    return {};
}

struct Block {
    std::size_t id{};
    std::uint64_t start{}, bytes{};
    bool locked{};
    std::uint32_t descriptor{};
};

class Heap {
  public:
    explicit Heap(const VolumeCapacityProfile &row)
        : byte_limit_{row.parameter_byte_limit}, slot_limit_{row.shared_object_slot_limit},
          descriptor_base_{row.target == ASeriesLoadTarget::a3000 ? 0x09130f20U : 0x01443000U} {
        for (std::uint32_t index = 0U; index < slot_limit_; ++index)
            free_descriptors_.push_back(descriptor_base_ + index * 24U);
    }

    bool allocate(std::size_t id, std::uint64_t bytes) {
        bytes = (bytes + 3U) & ~std::uint64_t{3U};
        if (used + bytes > byte_limit_ || blocks_.size() >= slot_limit_)
            return false;
        auto start = gap(bytes);
        if (!start) {
            std::uint64_t cursor{};
            for (auto &block : blocks_) {
                if (!block.locked)
                    block.start = cursor;
                cursor = block.start + block.bytes;
            }
            start = gap(bytes);
        }
        if (!start)
            return false;
        const auto descriptor = free_descriptors_.front();
        free_descriptors_.pop_front();
        blocks_.push_back({id, *start, bytes, false, descriptor});
        std::ranges::sort(blocks_, {}, &Block::start);
        used += bytes;
        peak = std::max(peak, used);
        peak_slots = std::max(peak_slots, static_cast<std::uint64_t>(blocks_.size()));
        return true;
    }

    void release(std::size_t id) {
        const auto block = std::ranges::find(blocks_, id, &Block::id);
        used -= block->bytes;
        free_descriptors_.push_back(block->descriptor);
        blocks_.erase(block);
    }
    void shrink(std::size_t id, std::uint64_t bytes) {
        auto &block = *std::ranges::find(blocks_, id, &Block::id);
        used -= block.bytes - bytes;
        block.bytes = bytes;
    }
    void lock(std::size_t id, bool value) { std::ranges::find(blocks_, id, &Block::id)->locked = value; }
    [[nodiscard]] std::uint64_t size(std::size_t id) const { return std::ranges::find(blocks_, id, &Block::id)->bytes; }
    [[nodiscard]] std::uint32_t descriptor(std::size_t id) const {
        return std::ranges::find(blocks_, id, &Block::id)->descriptor;
    }

    std::uint64_t used{}, peak{}, peak_slots{};
    [[nodiscard]] std::uint64_t slots() const { return blocks_.size(); }

    void discard_startup_waves() {
        // Seven built-in Wave/Sample pairs are allocated at boot, then freed by Wipe.
        for (unsigned index = 0U; index < 14U; ++index) {
            free_descriptors_.push_back(free_descriptors_.front());
            free_descriptors_.pop_front();
        }
    }

  private:
    std::optional<std::uint64_t> gap(std::uint64_t bytes) const {
        std::uint64_t cursor{};
        for (const auto &block : blocks_) {
            if (bytes <= block.start - cursor)
                return cursor;
            cursor = block.start + block.bytes;
        }
        return bytes <= byte_limit_ - cursor ? std::optional{cursor} : std::nullopt;
    }
    std::uint64_t byte_limit_, slot_limit_;
    std::uint32_t descriptor_base_;
    std::deque<std::uint32_t> free_descriptors_;
    std::vector<Block> blocks_;
};

struct Replay {
    bool success{};
    std::uint64_t resident{}, slots{}, peak{}, peak_slots{};
    std::string failed_name;
    std::vector<VolumeCapacityReason> notices;
};

struct RuntimeRows {
    ObjectType type{};
    std::vector<CapacityRow> rows;
};

RuntimeRows cleared_rows(const CapacityObject &object) {
    auto rows = object.rows;
    if (rows.empty()) {
        for (const auto &reference : object.references)
            rows.push_back({reference.type, reference.name, reference.raw_handle});
        for (std::size_t index = 0U; index < object.empty_counted_rows; ++index)
            rows.push_back({ObjectType::unknown,
                            {},
                            index < object.inactive_row_handles.size() ? object.inactive_row_handles[index] : 0U});
    }
    for (auto &row : rows)
        if (row.name.front() != std::byte{})
            row.raw_handle = 0U;
    return {object.type, std::move(rows)};
}

Replay run(const CapacityVolume &volume, const VolumeCapacityProfile &profile) {
    Heap heap{profile};
    const bool a3 = profile.target == ASeriesLoadTarget::a3000;
    std::size_t next_id{};
    std::map<Identity, std::size_t> loaded;
    std::map<Identity, const CapacityObject *> objects;
    using WaveLinks = std::array<std::optional<std::size_t>, 2>;
    std::map<std::size_t, WaveLinks> sample_links;
    std::map<std::uint32_t, std::size_t> sample_descriptors;
    std::map<std::size_t, RuntimeRows> assignment_rows;
    std::vector<VolumeCapacityReason> notices;
    const auto baseline = [&](std::string_view name) {
        const auto id = next_id++;
        loaded.emplace(Identity{ObjectType::prog, stored_name(name)}, id);
        return heap.allocate(id, a3 ? 680U : 856U);
    };
    baseline("TEMP");
    if (!a3)
        baseline("TEMPFORTGUM");
    for (unsigned slot = 1U; slot <= 128U; ++slot)
        baseline(std::format("{:03}", slot));
    heap.discard_startup_waves();
    std::map<Identity, const CapacityObject *> files;
    for (const auto &object : volume.objects)
        files.emplace(Identity{object.type, capacity_filename(object)}, &object);
    const std::array order{ObjectType::sbnk, ObjectType::sbac, ObjectType::sequ, ObjectType::prog};
    const auto failure = [&](ObjectType type, const std::array<std::byte, 16> &raw_name, std::string_view stage) {
        std::string name{capacity_type_name(type)};
        name += "/";
        for (const auto byte : raw_name)
            name.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
        while (name.back() == ' ')
            name.pop_back();
        name += " (" + std::string{stage} + ")";
        return Replay{false, 0U, 0U, 0U, 0U, name, notices};
    };
    const auto release_wave = [&](std::size_t wave_id) {
        for (auto &[sample_id, links] : sample_links) {
            (void)sample_id;
            for (auto &link : links)
                if (link == wave_id)
                    link.reset();
        }
        heap.release(wave_id);
        std::erase_if(loaded, [&](const auto &entry) {
            return entry.first.first == ObjectType::smpl && entry.second == wave_id;
        });
    };
    const auto release_sample = [&](std::size_t id, const Identity &identity) {
        const auto links = sample_links.at(id);
        for (std::size_t lane = 0U; lane < links.size(); ++lane) {
            const auto link = links[lane];
            if (!link || (lane == 1U && link == links[0U]))
                continue;
            const auto owners = std::ranges::count_if(
                sample_links, [&](const auto &entry) { return entry.second[0U] == link || entry.second[1U] == link; });
            if (owners < 2)
                release_wave(*link);
        }
        sample_links.erase(id);
        sample_descriptors.erase(heap.descriptor(id));
        heap.release(id);
        loaded.erase(identity);
        objects.erase(identity);
    };
    for (const auto type : order) {
        for (const auto &object : volume.objects) {
            if (object.type != type)
                continue;
            const Identity filename{type, capacity_filename(object)};
            if (loaded.contains(filename) &&
                (type == ObjectType::sbnk || type == ObjectType::sequ || (a3 && type == ObjectType::sbac)))
                continue;
            const auto id = next_id++;
            auto bytes = (a3 ? object.older_body_bytes : object.encoded_body_bytes) - 8U;
            if (!a3 && type == ObjectType::sbnk && object.selector < 4U)
                bytes = 336U;
            else if (!a3 && object.selector < 4U && (type == ObjectType::sbac || type == ObjectType::prog))
                bytes += type == ObjectType::sbac ? 36U : 176U;
            if (!heap.allocate(id, bytes))
                return failure(object.type, object.name,
                               type == ObjectType::prog ? "Program replacement overlap" : "object metadata");
            const Identity identity{type, capacity_registered_name(object)};
            if (const auto old = loaded.find(identity); old != loaded.end()) {
                sample_links.erase(old->second);
                sample_descriptors.erase(heap.descriptor(old->second));
                assignment_rows.erase(old->second);
                heap.release(old->second);
            }
            loaded.insert_or_assign(identity, id);
            objects.insert_or_assign(identity, &object);
            if (type == ObjectType::sbac || type == ObjectType::prog)
                assignment_rows.insert_or_assign(id, cleared_rows(object));
            if (type != ObjectType::sbnk)
                continue;
            sample_links.emplace(id, WaveLinks{});
            sample_descriptors.emplace(heap.descriptor(id), id);
            for (std::size_t index = 0U; !a3 && index < object.references.size(); ++index) {
                const auto &reference = object.references[index];
                if (const auto wave = loaded.find(Identity{ObjectType::smpl, reference.name}); wave != loaded.end())
                    sample_links.at(id)[index + (object.has_left_wave ? 0U : 1U)] = wave->second;
            }
            heap.lock(id, true);
            bool missing_wave{};
            for (std::size_t index = 0U; index < object.references.size(); ++index) {
                const auto &reference = object.references[index];
                const auto lane = index + (object.has_left_wave ? 0U : 1U);
                const auto attach = [&](std::size_t wave_id) {
                    sample_links.at(id)[lane] = wave_id;
                    heap.shrink(wave_id, 72U);
                };
                const Identity wave{reference.type, reference.name};
                if (loaded.contains(wave)) {
                    attach(loaded.at(wave));
                    continue;
                }
                const auto source = files.find(wave);
                if (source == files.end()) {
                    missing_wave = true;
                    sample_links.at(id)[lane].reset();
                    if (lane == 1U && sample_links.at(id)[0U])
                        release_wave(*sample_links.at(id)[0U]);
                    notices.push_back({"SAMPLE_SKIPPED_MISSING_WAVE",
                                       "A Sample is skipped during loading because its Wave Data file is missing."});
                    break;
                }
                const auto wave_id = next_id++;
                if (!heap.allocate(wave_id, 120U))
                    return failure(reference.type, reference.name, "Wave Data metadata before shrink");
                const Identity own_wave{ObjectType::smpl, capacity_registered_name(*source->second)};
                if (const auto old = loaded.find(own_wave); old != loaded.end())
                    heap.release(old->second);
                loaded.insert_or_assign(own_wave, wave_id);
                // Registration retargets raw matching lanes, but does not resize the Wave.
                for (const auto &[owner, source_object] : objects) {
                    if (owner.first != ObjectType::sbnk)
                        continue;
                    auto &links = sample_links.at(loaded.at(owner));
                    for (std::size_t ref = 0U; ref < source_object->references.size(); ++ref) {
                        if (source_object->references[ref].name != own_wave.second)
                            continue;
                        const auto owner_lane = ref + (source_object->has_left_wave ? 0U : 1U);
                        links[owner_lane] = wave_id;
                        if (!a3 && owner_lane == 0U)
                            links[1U].reset();
                    }
                }
                if (own_wave == wave)
                    attach(wave_id);
            }
            heap.lock(id, false);
            if (missing_wave || (!a3 && !sample_links.at(id)[0U]))
                release_sample(id, identity);
        }
    }
    const auto identity_of = [&](std::size_t id) {
        return std::ranges::find_if(loaded, [&](const auto &entry) { return entry.second == id; })->first;
    };
    const auto backfill_programs = [&](const Identity &identity, std::size_t id) {
        for (auto &[owner, assignments] : assignment_rows) {
            (void)owner;
            if (assignments.type != ObjectType::prog)
                continue;
            for (auto &row : assignments.rows)
                if (row.type == identity.first && row.name == identity.second)
                    row.raw_handle = heap.descriptor(id);
        }
    };
    const auto rename_sample = [&](std::size_t id, std::array<std::byte, 16> candidate) {
        CapacityObject named;
        named.name = candidate;
        candidate = capacity_registered_name(named);
        if (std::ranges::all_of(candidate, [](std::byte byte) { return byte == std::byte{' '}; }))
            return false;
        const auto previous = identity_of(id);
        const Identity identity{ObjectType::sbnk, candidate};
        if (previous == identity)
            return true;
        if (loaded.contains(identity))
            return false;
        for (auto &[owner, assignments] : assignment_rows) {
            (void)owner;
            for (auto &row : assignments.rows) {
                if (row.raw_handle != heap.descriptor(id))
                    continue;
                row.name = candidate;
                if (assignments.type == ObjectType::prog)
                    break;
            }
        }
        auto object = objects.extract(previous);
        object.key() = identity;
        objects.insert(std::move(object));
        auto entry = loaded.extract(previous);
        entry.key() = identity;
        loaded.insert(std::move(entry));
        return true;
    };
    const auto unique_source_name = [&](std::size_t id) {
        const auto original = identity_of(id).second;
        auto candidate = original;
        while (true) {
            std::size_t index = candidate.size();
            while (index > 1U && candidate[index - 1U] != std::byte{' '})
                --index;
            if (index <= 1U)
                break;
            candidate[index - 1U] = std::byte{'*'};
            if (rename_sample(id, candidate))
                return;
        }
        candidate = original;
        while (true) {
            for (auto &byte : candidate) {
                const auto value = std::to_integer<unsigned>(byte) + 1U;
                byte = value > 0x7eU ? std::byte{'0'} : static_cast<std::byte>(value);
                if (value <= 0x7eU)
                    break;
            }
            if (rename_sample(id, candidate))
                return;
        }
    };
    // Every Sample pass and copy performs Program backfill before Bank binding.
    std::vector<std::size_t> banks;
    for (const auto &[id, assignments] : assignment_rows)
        if (assignments.type == ObjectType::sbac)
            banks.push_back(id);
    std::ranges::sort(banks, [&](auto left, auto right) { return heap.descriptor(left) < heap.descriptor(right); });
    for (auto iterator = sample_descriptors.begin(); iterator != sample_descriptors.end(); ++iterator) {
        auto source_id = iterator->second;
        const auto original = identity_of(source_id).second;
        backfill_programs(identity_of(source_id), source_id);
        bool referenced = std::ranges::any_of(assignment_rows, [&](const auto &owner) {
            return std::ranges::any_of(owner.second.rows,
                                       [&](const auto &row) { return row.raw_handle == heap.descriptor(source_id); });
        });
        std::optional<std::size_t> previous_bank;
        while (true) {
            const auto current_name = identity_of(source_id).second;
            const auto next_bank = std::ranges::find_if(banks, [&](auto bank) {
                return bank != previous_bank &&
                       std::ranges::any_of(assignment_rows.at(bank).rows, [&](const auto &row) {
                           return row.name.front() != std::byte{} && row.name == current_name && row.raw_handle == 0U;
                       });
            });
            if (next_bank == banks.end())
                break;
            if (referenced) {
                auto temporary = stored_name("New Sample");
                for (unsigned suffix = 1U; loaded.contains({ObjectType::sbnk, temporary}); ++suffix) {
                    temporary = stored_name("New Sample");
                    const auto text = std::to_string(suffix);
                    std::ranges::transform(text, temporary.end() - static_cast<std::ptrdiff_t>(text.size()),
                                           [](char byte) { return static_cast<std::byte>(byte); });
                }
                const auto copy_id = next_id++;
                if (!heap.allocate(copy_id, heap.size(source_id)))
                    return failure(ObjectType::sbnk, original, "shared Sample Bank member copy");
                const Identity copy_identity{ObjectType::sbnk, temporary};
                objects.emplace(copy_identity, objects.at(identity_of(source_id)));
                loaded.emplace(copy_identity, copy_id);
                sample_links.emplace(copy_id, sample_links.at(source_id));
                sample_descriptors.emplace(heap.descriptor(copy_id), copy_id);
                backfill_programs(copy_identity, copy_id);
                unique_source_name(source_id);
                rename_sample(copy_id, original);
                source_id = copy_id;
            }
            const auto name = identity_of(source_id).second;
            for (auto &row : assignment_rows.at(*next_bank).rows)
                if (row.name == name)
                    row.raw_handle = heap.descriptor(source_id);
            referenced = true;
            previous_bank = *next_bank;
        }
    }
    for (const auto id : banks)
        backfill_programs(identity_of(id), id);
    for (const auto type : {ObjectType::sbac, ObjectType::prog}) {
        for (const auto &[identity, object] : objects) {
            if (identity.first != type)
                continue;
            const auto removed = static_cast<std::size_t>(std::ranges::count_if(
                assignment_rows.at(loaded.at(identity)).rows, [](const auto &row) { return row.raw_handle == 0U; }));
            const auto count = object->counted_rows - removed;
            if (removed == 0U || (object->counted_rows <= 8U && count <= 8U))
                continue;
            const auto base = type == ObjectType::sbac ? (a3 ? 276U : 312U) : (a3 ? 232U : 408U);
            const auto stride = type == ObjectType::sbac ? 20U : 56U;
            heap.shrink(loaded.at(identity), base + stride * std::max(std::size_t{8U}, count));
        }
    }
    return {true, heap.used, heap.slots(), heap.peak, heap.peak_slots, {}, notices};
}
} // namespace

Result<void> replay_native_capacity(const CapacityVolume &volume, VolumeCapacityProfile &profile) {
    if (const auto valid = covered(volume, profile); !valid)
        return std::unexpected{valid.error()};
    const auto replay = run(volume, profile);
    profile.reasons.insert(profile.reasons.end(), replay.notices.begin(), replay.notices.end());
    if (!replay.success) {
        profile.status = VolumeCapacityStatus::does_not_fit;
        profile.reasons.push_back(
            {"LOAD_ALLOCATION_FAILED", "A clean load cannot allocate " + replay.failed_name + "."});
    } else {
        profile.status = VolumeCapacityStatus::fits;
        profile.resident_bytes = replay.resident;
        profile.minimum_resident_bytes = replay.resident;
        profile.minimum_resident_slots = replay.slots;
        profile.peak_bytes = replay.peak;
        profile.peak_slots = replay.peak_slots;
    }
    return {};
}
} // namespace axk::detail
