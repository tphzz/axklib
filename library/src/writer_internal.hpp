#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "axklib/writer.hpp"

namespace axk::detail {

enum class RecordKind : std::uint8_t { hidden, system, directory, object };

struct PreparedRecord {
  std::uint32_t id{};
  std::vector<std::byte> payload;
  RecordKind kind{};
  std::uint16_t tail{};
  std::uint32_t cluster{};
  std::uint32_t clusters{};
};

struct PreparedWaveformMember {
  std::string name;
  std::uint32_t link_id{};
  std::uint32_t sample_rate{};
  std::uint32_t frame_count{};
};

Result<std::vector<std::byte>>
prepare_smpl_payload(const WaveformSpec &spec, const ImportedAudio &audio, std::uint32_t link_id);
Result<std::vector<std::byte>>
prepare_sbnk_payload(const SampleBankSpec &spec, const PreparedWaveformMember &left,
                     const std::optional<PreparedWaveformMember> &right = {}, bool grouped = false,
                     const std::vector<std::uint8_t> &linked_programs = {});
Result<std::vector<std::byte>>
prepare_sbac_payload(const SampleBankGroupSpec &group,
                     const std::map<std::string, SampleBankSpec> &banks);
Result<std::vector<std::byte>> prepare_prog_payload(const ProgramSpec &program);

Result<std::vector<PreparedRecord>>
prepare_partition_records(const PartitionSpec &partition, const PartitionGeometry &geometry,
                          std::size_t partition_count, const CancellationToken &cancellation);

} // namespace axk::detail
